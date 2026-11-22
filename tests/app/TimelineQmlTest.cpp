#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>
#include <QUrl>

#include "AppController.h"
#include "AudioSink.h"
#include "PreviewController.h"
#include "ExportController.h"
#include "TimelineController.h"
#include "TimelineModel.h"
#include "VideoPreviewItem.h"
#include "WaveformImageProvider.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

AudioEngine::SinkFactory nullSinkFactory()
{
    return [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
}

void registerPreviewType()
{
    static const bool registered = [] {
        qmlRegisterType<VideoPreviewItem>("VideoEditor", 1, 0,
                                          "VideoPreview");
        return true;
    }();
    Q_UNUSED(registered);
}

QQuickItem *findVisualItem(QQuickItem *parent, const QString &name)
{
    if (!parent)
        return nullptr;
    if (parent->objectName() == name)
        return parent;
    const auto children = parent->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findVisualItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *findItem(QQuickWindow *window, const QString &name)
{
    QQuickItem *found = findVisualItem(window->contentItem(), name);
    if (!found)
        found = window->findChild<QQuickItem *>(name);
    return found;
}

// QTest::mouseMove sends moves without the left button held, which keeps a
// Flickable from exercising its steal logic. Send explicit QMouseEvents so the
// test covers the same button state as a real drag.
void sendMouse(QQuickWindow *window, QEvent::Type type, const QPointF &pos,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, pos, window->mapToGlobal(pos.toPoint()), button,
                      buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
    QCoreApplication::processEvents();
}

void dragMouse(QQuickWindow *window, const QPointF &from, const QPointF &to,
               int steps = 4)
{
    sendMouse(window, QEvent::MouseButtonPress, from, Qt::LeftButton,
              Qt::LeftButton);
    for (int i = 1; i <= steps; ++i) {
        const QPointF point = from + (to - from) * (double(i) / steps);
        sendMouse(window, QEvent::MouseMove, point, Qt::NoButton,
                  Qt::LeftButton);
    }
    sendMouse(window, QEvent::MouseButtonRelease, to, Qt::LeftButton,
              Qt::NoButton);
    // Let the controller commit and the bindings settle before the caller
    // reads the project; 100 ms was intermittently too short on CI desktops.
    QTest::qWait(200);
}

void clickItem(QQuickWindow *window, QQuickItem *item)
{
    const QPointF point = item->mapToScene(
        QPointF(item->width() / 2.0, item->height() / 2.0));
    sendMouse(window, QEvent::MouseButtonPress, point, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, point, Qt::LeftButton,
              Qt::NoButton);
    QTest::qWait(100);
}

// Exercises the delegate's onDoubleClicked path through Qt Test's full
// double-click sequence (press/release/dblclick/release).
void doubleClickItem(QQuickWindow *window, QQuickItem *item)
{
    const QPointF point = item->mapToScene(
        QPointF(item->width() / 2.0, item->height() / 2.0));
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier,
                       point.toPoint());
    QTest::qWait(150);
}

const Clip *firstClip(const Project &project)
{
    for (const Track &track : project.tracks) {
        if (!track.clips.isEmpty())
            return &track.clips.first();
    }
    return nullptr;
}

const Clip *findClip(const Project &project, const QString &clipId)
{
    for (const Track &track : project.tracks) {
        for (const Clip &clip : track.clips) {
            if (clip.id == clipId)
                return &clip;
        }
    }
    return nullptr;
}

} // namespace

TEST(TimelineQml, LoadsAndCommitsDragAndTrim)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;
    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());

    // A cue exercises the subtitle panel's edit field while the QML loads.
    const QString cueId = controller.timeline()->addCueAtPlayhead();
    ASSERT_FALSE(cueId.isEmpty());
    controller.timeline()->setCueText(cueId, QStringLiteral("测试字幕"));

    // Long, zoomed-in timeline: the 2 s clip is 800 px wide while the visible
    // timeline viewport is roughly 700 px, so the Flickable may try to steal a
    // horizontal gesture.
    controller.timeline()->setPixelsPerSecond(400.0);

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("waveform"),
                            new WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("Export"),
                                             controller.exportController());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QStringLiteral("test"));

    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(VIDEOEDITOR_SOURCE_DIR
                                           "/src/app/Main.qml")));
    QScopedPointer<QObject> root(component.create());
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    ASSERT_NE(root.data(), nullptr);

    auto *window = qobject_cast<QQuickWindow *>(root.data());
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for gesture delivery";
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000))
        GTEST_SKIP() << "window was not exposed (no desktop/D3D session)";
    QTest::qWait(300);

    QQuickItem *cueText = findItem(window, QStringLiteral("cue-text-") + cueId);
    ASSERT_NE(cueText, nullptr);
    EXPECT_EQ(cueText->property("text").toString(), QStringLiteral("测试字幕"));
    cueText->forceActiveFocus();
    QTest::qWait(50);
    if (cueText->hasActiveFocus()) {
        EXPECT_EQ(cueText->property("text").toString(),
                  QStringLiteral("测试字幕"))
            << "the caption must remain visible while the field is focused";
    }

    QQuickItem *clip = findItem(window, QStringLiteral("clip-") + clipId);
    ASSERT_NE(clip, nullptr);
    ASSERT_GT(clip->width(), 0.0);

    // The inspector must be initialized from the selected clip rather than
    // showing hard-coded defaults.
    controller.timeline()->selectClip(clipId);
    controller.timeline()->setClipVolume(clipId, 1500);
    controller.timeline()->setClipTransform(clipId, 1500, 250, -300, 500);
    QTest::qWait(100);
    QQuickItem *volume = findItem(window, QStringLiteral("inspector-volume"));
    QQuickItem *scale = findItem(window, QStringLiteral("inspector-scale"));
    QQuickItem *opacity = findItem(window, QStringLiteral("inspector-opacity"));
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(scale, nullptr);
    ASSERT_NE(opacity, nullptr);
    EXPECT_NEAR(volume->property("value").toDouble(), 150.0, 0.001);
    EXPECT_NEAR(scale->property("value").toDouble(), 150.0, 0.001);
    EXPECT_NEAR(opacity->property("value").toDouble(), 50.0, 0.001);
    // Audio controls on a video clip are inaudible; the inspector disables
    // them and shows a hint instead of silently writing dead parameters.
    EXPECT_FALSE(volume->property("enabled").toBool());

    // Dragging one fade slider must preserve the other fade, even after
    // switching selection between clips with different fade values.
    const QString clip2Id = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 5'000'000);
    ASSERT_FALSE(clip2Id.isEmpty());
    // Fades are audio parameters and the inspector intentionally disables them
    // for a video clip; select the linked A-track halves for this gesture.
    ASSERT_GE(controller.document()->project().tracks.at(1).clips.size(), 2);
    const QString audioClipId =
        controller.document()->project().tracks.at(1).clips.at(0).id;
    const QString audioClip2Id =
        controller.document()->project().tracks.at(1).clips.at(1).id;
    controller.timeline()->setClipFades(audioClipId, 300'000, 400'000);
    controller.timeline()->setClipFades(audioClip2Id, 100'000, 900'000);
    controller.timeline()->selectClip(audioClip2Id);
    QTest::qWait(80);
    controller.timeline()->selectClip(audioClipId);
    QTest::qWait(80);

    QQuickItem *fadeIn = findItem(window, QStringLiteral("inspector-fadeIn"));
    QQuickItem *fadeOut = findItem(window, QStringLiteral("inspector-fadeOut"));
    ASSERT_NE(fadeIn, nullptr);
    ASSERT_NE(fadeOut, nullptr);

    const auto dragSlider = [window](QQuickItem *slider, double fromFraction,
                                     double toFraction) {
        const QPointF from = slider->mapToScene(
            QPointF(slider->width() * fromFraction, slider->height() / 2.0));
        const QPointF to = from
                           + QPointF(slider->width()
                                         * (toFraction - fromFraction), 0);
        dragMouse(window, from, to);
    };

    dragSlider(fadeIn, 0.5, 0.8);
    const Clip *afterFadeIn =
        findClip(controller.document()->project(), audioClipId);
    ASSERT_NE(afterFadeIn, nullptr);
    const qint64 fadeInAfterDrag = afterFadeIn->audio.fadeInUs;
    EXPECT_NE(fadeInAfterDrag, 300'000);
    EXPECT_EQ(afterFadeIn->audio.fadeOutUs, 400'000)
        << "dragging fade-in must preserve the sibling fade-out";

    dragSlider(fadeOut, 0.8, 0.3);
    const Clip *afterFadeOut =
        findClip(controller.document()->project(), audioClipId);
    ASSERT_NE(afterFadeOut, nullptr);
    EXPECT_EQ(afterFadeOut->audio.fadeInUs, fadeInAfterDrag)
        << "dragging fade-out must preserve the sibling fade-in";
    EXPECT_NE(afterFadeOut->audio.fadeOutUs, 400'000);

    const QPointF dragFrom = clip->mapToScene(
        QPointF(clip->width() / 2.0, clip->height() / 2.0));
    const QPointF dragTo = dragFrom + QPointF(160, 0);
    dragMouse(window, dragFrom, dragTo);

    const Clip *moved = firstClip(controller.document()->project());
    ASSERT_NE(moved, nullptr);
    EXPECT_GT(moved->timelineStartUs, 0)
        << "clip drag was stolen or not committed";

    const qint64 startAfterDrag = moved->timelineStartUs;
    const qint64 durationAfterDrag = moved->timelineDurationUs();

    // At 400 px/s the right edge is under the inspector; zoom out so the
    // handle is inside the visible timeline viewport, as a user would.
    controller.timeline()->setPixelsPerSecond(200.0);
    QTest::qWait(100);

    QQuickItem *trimEnd = findItem(window,
                                   QStringLiteral("trim-end-") + clipId);
    ASSERT_NE(trimEnd, nullptr);
    const QPointF trimFrom = trimEnd->mapToScene(
        QPointF(trimEnd->width() / 2.0, trimEnd->height() / 2.0));
    dragMouse(window, trimFrom, trimFrom - QPointF(80, 0));

    const Clip *trimmed = firstClip(controller.document()->project());
    ASSERT_NE(trimmed, nullptr);
    EXPECT_EQ(trimmed->timelineStartUs, startAfterDrag)
        << "right trim must not move the clip start";
    EXPECT_LT(trimmed->timelineDurationUs(), durationAfterDrag)
        << "clip trim was stolen or not committed";
}

TEST(TimelineQml, TrackHeadersSelectTargetAndToggleAudioMuteSolo)
{
    const QString videoPath = fixture(QStringLiteral("h264.mp4"));
    const QString musicPath = fixture(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(videoPath) || !QFileInfo::exists(musicPath))
        GTEST_SKIP() << "missing fixtures";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(videoPath),
                           QUrl::fromLocalFile(musicPath)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 2);
    const QString videoId = controller.document()->project().assets.at(0).id;
    const QString musicId = controller.document()->project().assets.at(1).id;

    // Build V2/A2 through the same controller commands the track menu uses.
    const QString video2 = controller.timeline()->addTrack(0);
    const QString audio2 = controller.timeline()->addTrack(1);
    ASSERT_FALSE(video2.isEmpty());
    ASSERT_FALSE(audio2.isEmpty());
    const QString videoClip = controller.timeline()->addAssetToTimeline(
        videoId, video2, 0);
    ASSERT_FALSE(videoClip.isEmpty());
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(musicId, QStringLiteral("track-a1"), 0)
                     .isEmpty());
    // Original audio paired with V2 lands on A2; BGM is on A1.
    ASSERT_EQ(controller.document()->trackById(video2)->clips.size(), 1);
    ASSERT_EQ(controller.document()->trackById(audio2)->clips.size(), 1);
    ASSERT_EQ(controller.document()
                  ->trackById(QStringLiteral("track-a1"))
                  ->clips.first()
                  .assetId,
              musicId);

    // A long project at low zoom exercises the adaptive ruler: one item per
    // second would create 600 delegate items.
    const QString longCue = controller.timeline()->addCueAtPlayhead();
    ASSERT_FALSE(longCue.isEmpty());
    ASSERT_TRUE(controller.timeline()->setCueTiming(longCue, 500'000'000,
                                                    600'000'000));
    controller.timeline()->setPixelsPerSecond(20.0);

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("waveform"),
                            new WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("Export"),
                                             controller.exportController());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QStringLiteral("test"));

    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(VIDEOEDITOR_SOURCE_DIR
                                           "/src/app/Main.qml")));
    QScopedPointer<QObject> root(component.create());
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for click delivery";
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000))
        GTEST_SKIP() << "window was not exposed (no desktop/D3D session)";
    QTest::qWait(300);

    // Track management lives in the timeline status strip now; the duplicate
    // global menu was removed.
    EXPECT_NE(findItem(window, QStringLiteral("add-track-button")), nullptr);
    EXPECT_NE(findItem(window, QStringLiteral("track-header-track-a1")),
              nullptr);

    // M/S are audio controls; video/subtitle headers must not show dead ones.
    QQuickItem *videoMute =
        findItem(window, QStringLiteral("track-mute-") + video2);
    QQuickItem *audioMute =
        findItem(window, QStringLiteral("track-mute-") + audio2);
    ASSERT_NE(videoMute, nullptr);
    ASSERT_NE(audioMute, nullptr);
    EXPECT_FALSE(videoMute->isVisible());
    EXPECT_TRUE(audioMute->isVisible());

    QObject *rulerTicks =
        window->findChild<QObject *>(QStringLiteral("ruler-ticks"));
    ASSERT_NE(rulerTicks, nullptr);
    const int tickCount = rulerTicks->property("count").toInt();
    EXPECT_GT(tickCount, 0);
    EXPECT_LT(tickCount, 200) << "ruler must virtualize ticks for long projects";

    QQuickItem *a1Header =
        findItem(window, QStringLiteral("track-header-track-a1"));
    ASSERT_NE(a1Header, nullptr);
    // Click the header's name area (not a button) to make A1 the target.
    const QPointF headerPoint =
        a1Header->mapToScene(QPointF(10.0, 8.0));
    sendMouse(window, QEvent::MouseButtonPress, headerPoint, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, headerPoint, Qt::LeftButton,
              Qt::NoButton);
    QTest::qWait(100);
    EXPECT_EQ(controller.timeline()->targetTrackId(),
              QStringLiteral("track-a1"));

    QQuickItem *a1Mute =
        findItem(window, QStringLiteral("track-mute-track-a1"));
    ASSERT_NE(a1Mute, nullptr);
    EXPECT_TRUE(a1Mute->isVisible());
    clickItem(window, a1Mute);
    ASSERT_TRUE(controller.document()->trackById(QStringLiteral("track-a1"))
                    ->muted);
    EXPECT_FALSE(controller.document()->trackById(audio2)->muted);

    QQuickItem *a2Solo = findItem(
        window, QStringLiteral("track-solo-") + audio2);
    ASSERT_NE(a2Solo, nullptr);
    clickItem(window, a2Solo);
    EXPECT_TRUE(controller.document()->trackById(audio2)->solo);
    EXPECT_FALSE(controller.document()->trackById(QStringLiteral("track-a1"))
                     ->solo);

    // Lanes are grouped/stacked: the V2 clip is above the A1 BGM clip.
    QQuickItem *videoItem = findItem(window, QStringLiteral("clip-")
                                                 + videoClip);
    ASSERT_NE(videoItem, nullptr);
    const QString bgmClipId =
        controller.document()->trackById(QStringLiteral("track-a1"))
            ->clips.first().id;
    QQuickItem *bgmItem =
        findItem(window, QStringLiteral("clip-") + bgmClipId);
    ASSERT_NE(bgmItem, nullptr);
    EXPECT_NE(videoItem->y(), bgmItem->y());

    // Double-clicking a media item through QML must add to the target lane
    // without leaving timeline mode, pausing, or resetting the playhead.
    controller.refreshTimelinePreview();
    controller.seekTimeline(1'500'000);
    ASSERT_TRUE(controller.preview()->timelineMode());
    const int v1ClipsBefore =
        int(controller.document()->trackById(QStringLiteral("track-v1"))
                ->clips.size());
    QQuickItem *mediaItem =
        findItem(window, QStringLiteral("media-item-") + videoId);
    ASSERT_NE(mediaItem, nullptr);
    doubleClickItem(window, mediaItem);

    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_FALSE(controller.preview()->playing());
    EXPECT_EQ(controller.timeline()->positionUs(), 1'500'000);
    EXPECT_EQ(controller.preview()->positionMs(), 1500);
    EXPECT_EQ(controller.document()->trackById(QStringLiteral("track-v1"))
                  ->clips.size(),
              v1ClipsBefore + 1);
}

TEST(TimelineQml, AuthorsKeyframesAndCuePosition)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());

    const Project &project = controller.document()->project();
    ASSERT_EQ(project.tracks.at(1).clips.size(), 1);
    const QString audioClipId = project.tracks.at(1).clips.first().id;
    controller.timeline()->selectClip(audioClipId);
    const QString cueId = controller.timeline()->addCueAtPlayhead();
    ASSERT_FALSE(cueId.isEmpty());
    // Adding a cue selects it (one-click subtitle flow); restore the audio
    // clip selection this test authors keyframes on.
    controller.timeline()->selectClip(audioClipId);
    controller.timeline()->seekUs(500'000);

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("waveform"),
                            new WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("Export"),
                                             controller.exportController());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QStringLiteral("test"));

    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(VIDEOEDITOR_SOURCE_DIR
                                           "/src/app/Main.qml")));
    QScopedPointer<QObject> root(component.create());
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    ASSERT_NE(root.data(), nullptr);

    auto *window = qobject_cast<QQuickWindow *>(root.data());
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for gesture delivery";
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000))
        GTEST_SKIP() << "window was not exposed (no desktop/D3D session)";
    QTest::qWait(300);

    QQuickItem *addKeyframe =
        findItem(window, QStringLiteral("inspector-add-keyframe"));
    QQuickItem *keyframeList =
        findItem(window, QStringLiteral("inspector-keyframe-list"));
    ASSERT_NE(addKeyframe, nullptr);
    ASSERT_NE(keyframeList, nullptr);
    EXPECT_EQ(keyframeList->property("count").toInt(), 0);
    // Keyframes are progressive disclosure: they stay hidden until the user
    // opens 高级, so the first screen only shows volume/fades.
    EXPECT_FALSE(keyframeList->isVisible());
    QQuickItem *advancedToggle =
        findItem(window, QStringLiteral("inspector-advanced-toggle"));
    ASSERT_NE(advancedToggle, nullptr);
    clickItem(window, advancedToggle);
    QTest::qWait(80);
    EXPECT_TRUE(keyframeList->isVisible());

    clickItem(window, addKeyframe);
    controller.timeline()->seekUs(1'500'000);
    QTest::qWait(100);
    clickItem(window, addKeyframe);

    EXPECT_EQ(keyframeList->property("count").toInt(), 2);
    const Clip *audioClip =
        findClip(controller.document()->project(), audioClipId);
    ASSERT_NE(audioClip, nullptr);
    ASSERT_EQ(audioClip->audio.keyframes.size(), 2);
    EXPECT_EQ(audioClip->audio.keyframes.at(0).timeUs, 500'000);
    EXPECT_EQ(audioClip->audio.keyframes.at(1).timeUs, 1'500'000);

    QQuickItem *deleteKeyframe =
        findItem(window, QStringLiteral("inspector-keyframe-delete-0"));
    ASSERT_NE(deleteKeyframe, nullptr);
    clickItem(window, deleteKeyframe);
    EXPECT_EQ(keyframeList->property("count").toInt(), 1);
    audioClip = findClip(controller.document()->project(), audioClipId);
    ASSERT_NE(audioClip, nullptr);
    ASSERT_EQ(audioClip->audio.keyframes.size(), 1);
    EXPECT_EQ(audioClip->audio.keyframes.first().timeUs, 1'500'000);

    // A non-preset speed stays visible in the editable combo instead of being
    // displayed as 1x.
    controller.timeline()->setClipSpeed(audioClipId, 1250, false);
    QTest::qWait(100);
    QQuickItem *speedBox = findItem(window, QStringLiteral("inspector-speed"));
    ASSERT_NE(speedBox, nullptr);
    EXPECT_EQ(speedBox->property("editText").toString(),
              QStringLiteral("自定义 1.25x"));

    // Position sliders bind to the selected cue and commit once on release.
    controller.timeline()->selectCue(cueId);
    QTest::qWait(100);
    QQuickItem *positionX =
        findItem(window, QStringLiteral("subtitle-position-x"));
    QQuickItem *positionY =
        findItem(window, QStringLiteral("subtitle-position-y"));
    ASSERT_NE(positionX, nullptr);
    ASSERT_NE(positionY, nullptr);

    const auto dragSlider = [window](QQuickItem *slider, double fromFraction,
                                     double toFraction) {
        const QPointF from = slider->mapToScene(
            QPointF(slider->width() * fromFraction, slider->height() / 2.0));
        const QPointF to = from
                           + QPointF(slider->width()
                                         * (toFraction - fromFraction), 0);
        dragMouse(window, from, to);
    };

    int undoIndex = controller.document()->undoStack()->index();
    dragSlider(positionX, 0.5, 0.8);
    EXPECT_EQ(controller.document()->undoStack()->index(), undoIndex + 1)
        << "dragging the X slider must produce exactly one undo step";
    undoIndex = controller.document()->undoStack()->index();
    dragSlider(positionY, 0.5, 0.3);
    EXPECT_EQ(controller.document()->undoStack()->index(), undoIndex + 1)
        << "dragging the Y slider must produce exactly one undo step";

    const SubtitleCue *cue = nullptr;
    for (const Track &track : controller.document()->project().tracks) {
        for (const SubtitleCue &candidate : track.cues) {
            if (candidate.id == cueId)
                cue = &candidate;
        }
    }
    ASSERT_NE(cue, nullptr);
    EXPECT_NE(cue->positionXPermille, 500);
    EXPECT_NE(cue->positionYPermille, 880);

    const TimelineModel *model = controller.timeline()->timelineModel();
    int cueRow = -1;
    for (int i = 0; i < model->rowCount(); ++i) {
        if (model->data(model->index(i, 0), TimelineModel::ItemIdRole).toString()
            == cueId) {
            cueRow = i;
        }
    }
    ASSERT_GE(cueRow, 0);
    EXPECT_EQ(model->data(model->index(cueRow, 0),
                          TimelineModel::PositionXPermilleRole)
                  .toInt(),
              cue->positionXPermille);
    EXPECT_EQ(model->data(model->index(cueRow, 0),
                          TimelineModel::PositionYPermilleRole)
                  .toInt(),
              cue->positionYPermille);
}

namespace {

int cueCount(const Project &project)
{
    int count = 0;
    for (const Track &track : project.tracks)
        count += int(track.cues.size());
    return count;
}

int clipCount(const Project &project)
{
    int count = 0;
    for (const Track &track : project.tracks)
        count += int(track.clips.size());
    return count;
}

// Loads Main.qml against a prepared controller and exposes the window. The
// caller owns the returned root object.
// Types plain ASCII into whichever item currently has active focus. Qt's test
// library only ships keyClicks() for widgets, not QWindow.
void typeText(QQuickWindow *window, const QString &text)
{
    for (const QChar &ch : text) {
        Qt::Key key = Qt::Key_unknown;
        if (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) {
            key = static_cast<Qt::Key>(Qt::Key_A + (ch.unicode() - 'a'));
        } else if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) {
            key = static_cast<Qt::Key>(Qt::Key_A + (ch.unicode() - 'A'));
        } else {
            key = static_cast<Qt::Key>(ch.unicode());
        }
        QTest::keyClick(window, key);
    }
}

QQuickWindow *showMainWindow(QQmlEngine &engine, AppController &controller,
                             QScopedPointer<QObject> &root)
{
    engine.addImageProvider(QStringLiteral("waveform"),
                            new WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("Export"),
                                             controller.exportController());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QStringLiteral("test"));

    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(VIDEOEDITOR_SOURCE_DIR
                                           "/src/app/Main.qml")));
    root.reset(component.create());
    if (component.isError()) {
        ADD_FAILURE() << component.errorString().toStdString();
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    if (!window)
        return nullptr;
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000))
        return nullptr;
    // Keyboard shortcuts only fire for an active window; mouse tests do not
    // need this but Shortcut delivery does.
    window->requestActivate();
    QTest::qWaitForWindowActive(window, 2000);
    QTest::qWait(300);
    return window;
}

} // namespace

TEST(TimelineQml, ShortcutsRespectTextFocusAndEditTimeline)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());
    const QString cueId = controller.timeline()->addCueAtPlayhead();
    ASSERT_FALSE(cueId.isEmpty());
    controller.timeline()->setCueText(cueId, QStringLiteral("字幕"));
    // The preview must be in timeline mode with a real duration before Space
    // can toggle playback.
    controller.refreshTimelinePreview();
    controller.timeline()->seekUs(1'000'000);

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key/click delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key delivery";

    // --- Space toggles playback. -----------------------------------------
    if (QQuickItem *focusSink =
            findItem(window, QStringLiteral("import-button")))
        focusSink->forceActiveFocus();
    QTest::qWait(50);
    const bool playingBefore = controller.preview()->playing();
    QTest::keyClick(window, Qt::Key_Space);
    QTest::qWait(80);
    EXPECT_NE(controller.preview()->playing(), playingBefore)
        << "Space must toggle playback";
    QTest::keyClick(window, Qt::Key_Space);
    QTest::qWait(80);

    // --- A focused text editor swallows editing shortcuts. ---------------
    QQuickItem *subtitleText =
        findItem(window, QStringLiteral("subtitle-text"));
    ASSERT_NE(subtitleText, nullptr);
    subtitleText->forceActiveFocus();
    QTest::qWait(50);
    ASSERT_TRUE(subtitleText->hasActiveFocus());

    const int clipsBeforeTyping = clipCount(controller.document()->project());
    const int undoBeforeTyping = controller.document()->undoStack()->index();
    const double zoomBeforeTyping = controller.timeline()->pixelsPerSecond();
    QTest::keyClick(window, Qt::Key_S);
    QTest::keyClick(window, Qt::Key_Plus);
    QTest::keyClick(window, Qt::Key_Minus);
    QTest::qWait(80);
    EXPECT_EQ(clipCount(controller.document()->project()), clipsBeforeTyping)
        << "typing s/+/- must not split or edit the timeline";
    EXPECT_EQ(controller.document()->undoStack()->index(), undoBeforeTyping);
    EXPECT_DOUBLE_EQ(controller.timeline()->pixelsPerSecond(),
                     zoomBeforeTyping)
        << "typing +/- must not zoom the timeline";
    EXPECT_TRUE(subtitleText->property("text").toString()
                    .contains(QStringLiteral("s")))
        << "typed text must reach the subtitle field";

    // Arrow keys must not move the playhead while text editing either.
    const qint64 positionBeforeTyping = controller.timeline()->positionUs();
    QTest::keyClick(window, Qt::Key_Right);
    QTest::qWait(50);
    EXPECT_EQ(controller.timeline()->positionUs(), positionBeforeTyping);

    // Release text focus and make sure editing shortcuts work again.
    if (QQuickItem *focusSink =
            findItem(window, QStringLiteral("import-button")))
        focusSink->forceActiveFocus();
    QTest::qWait(50);

    // --- Ctrl+B splits the clip under the playhead even when a cue is
    // selected elsewhere. -------------------------------------------------
    controller.timeline()->selectCue(cueId);
    controller.timeline()->seekUs(1'000'000);
    const int videoClipsBefore = int(
        controller.document()->trackById(QStringLiteral("track-v1"))
            ->clips.size());
    const int undoBeforeSplit = controller.document()->undoStack()->index();
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(80);
    EXPECT_EQ(int(controller.document()->trackById(QStringLiteral("track-v1"))
                      ->clips.size()),
              videoClipsBefore + 1)
        << "Ctrl+B must split the clip under the playhead";
    EXPECT_GE(controller.document()->undoStack()->index(), undoBeforeSplit + 1);

    // --- Delete dispatches to the selected cue. --------------------------
    controller.timeline()->selectCue(cueId);
    const int cuesBeforeDelete = cueCount(controller.document()->project());
    QTest::keyClick(window, Qt::Key_Delete);
    QTest::qWait(80);
    EXPECT_EQ(cueCount(controller.document()->project()), cuesBeforeDelete - 1)
        << "Delete must remove the selected subtitle cue";
    EXPECT_TRUE(controller.timeline()->selectedCueId().isEmpty());

    // --- Delete removes a selected clip. ---------------------------------
    const QString splitClipId = controller.document()
                                    ->trackById(QStringLiteral("track-v1"))
                                    ->clips.first().id;
    controller.timeline()->selectClip(splitClipId);
    const int clipsBeforeDelete = clipCount(controller.document()->project());
    QTest::keyClick(window, Qt::Key_Delete);
    QTest::qWait(80);
    // A linked A/V pair is deleted together, so assert a decrease rather than
    // exactly one clip.
    EXPECT_LT(clipCount(controller.document()->project()), clipsBeforeDelete)
        << "Delete must remove the selected clip";
    EXPECT_TRUE(controller.timeline()->selectedClipId().isEmpty());

    // --- Ctrl+Z / Ctrl+Y mirror the undo stack. --------------------------
    if (QQuickItem *focusSink =
            findItem(window, QStringLiteral("import-button")))
        focusSink->forceActiveFocus();
    const int undoBeforeUndo = controller.document()->undoStack()->index();
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(80);
    EXPECT_LT(controller.document()->undoStack()->index(), undoBeforeUndo)
        << "Ctrl+Z must undo";
    QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
    QTest::qWait(80);
    EXPECT_EQ(controller.document()->undoStack()->index(), undoBeforeUndo)
        << "Ctrl+Y must redo the undone step";

    // --- Arrow keys step one frame when no editor has focus. -------------
    controller.timeline()->seekUs(1'000'000);
    const qint64 frameStart = controller.timeline()->positionUs();
    QTest::keyClick(window, Qt::Key_Right);
    QTest::qWait(80);
    const qint64 expected = controller.timeline()->frameQuantizeUs(
        1'000'000) + (1'000'000 / 30);
    EXPECT_GT(controller.timeline()->positionUs(), frameStart);
    EXPECT_LE(qMax<qint64>(0, controller.timeline()->positionUs() - expected),
              40'000)
        << "Right must advance about one frame";
    QTest::keyClick(window, Qt::Key_Left);
    QTest::qWait(80);
    EXPECT_LT(controller.timeline()->positionUs(), frameStart + 1'000'000);
}

TEST(TimelineQml, OneClickSubtitleExportEntryAndDisclosure)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key/click delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for click delivery";

    // --- Export entry is fixed in the right half and enabled. ------------
    QQuickItem *exportButton =
        findItem(window, QStringLiteral("exportButton"));
    ASSERT_NE(exportButton, nullptr);
    EXPECT_TRUE(exportButton->isVisible());
    EXPECT_TRUE(exportButton->isEnabled());
    const QPointF exportScene = exportButton->mapToScene(
        QPointF(exportButton->width() / 2.0, exportButton->height() / 2.0));
    EXPECT_GT(exportScene.x(), window->width() / 2.0)
        << "export must stay in the top-right corner";

    // --- One-click subtitle creates and focuses the editor. --------------
    QQuickItem *subtitleAdd = findItem(window, QStringLiteral("subtitle-add"));
    ASSERT_NE(subtitleAdd, nullptr);
    const int cuesBefore = cueCount(controller.document()->project());
    clickItem(window, subtitleAdd);
    QTest::qWait(120);
    EXPECT_EQ(cueCount(controller.document()->project()), cuesBefore + 1);
    QQuickItem *subtitleText =
        findItem(window, QStringLiteral("subtitle-text"));
    ASSERT_NE(subtitleText, nullptr);
    EXPECT_TRUE(subtitleText->hasActiveFocus())
        << "the one-click subtitle flow must focus the text editor";

    // The selected cue's style is visible and stays in sync both ways.
    QQuickItem *styleBox = findItem(window, QStringLiteral("subtitle-style"));
    ASSERT_NE(styleBox, nullptr);
    const QString activeCueId = controller.timeline()->selectedCueId();
    ASSERT_FALSE(activeCueId.isEmpty());
    EXPECT_EQ(styleBox->property("currentIndex").toInt(), 0); // default
    controller.timeline()->applySubtitleStyle(QStringLiteral("boxed"),
                                              activeCueId);
    QTest::qWait(80);
    EXPECT_EQ(styleBox->property("currentIndex").toInt(), 1)
        << "the style combo must follow the selected cue's style";
    styleBox->setProperty("currentIndex", 2);
    QMetaObject::invokeMethod(styleBox, "activated", Q_ARG(int, 2));
    QTest::qWait(80);
    QString appliedStyle;
    for (const Track &track : controller.document()->project().tracks) {
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id == activeCueId)
                appliedStyle = cue.styleId;
        }
    }
    EXPECT_EQ(appliedStyle, QStringLiteral("bold-outline"));

    // --- Progressive disclosure: keyframes stay hidden until 高级. --------
    QQuickItem *keyframeList =
        findItem(window, QStringLiteral("inspector-keyframe-list"));
    ASSERT_NE(keyframeList, nullptr);
    EXPECT_FALSE(keyframeList->isVisible());
    QQuickItem *advancedToggle =
        findItem(window, QStringLiteral("inspector-advanced-toggle"));
    ASSERT_NE(advancedToggle, nullptr);
    clickItem(window, advancedToggle);
    QTest::qWait(80);
    EXPECT_TRUE(keyframeList->isVisible());

    // --- Context menus exist without occupying the primary bar. ----------
    const QString clipId = controller.document()
                               ->trackById(QStringLiteral("track-v1"))
                               ->clips.first().id;
    controller.timeline()->selectClip(clipId);
    QTest::qWait(80);
    QQuickItem *clipItem =
        findItem(window, QStringLiteral("clip-") + clipId);
    ASSERT_NE(clipItem, nullptr);
    const QPointF menuPoint = clipItem->mapToScene(
        QPointF(clipItem->width() / 2.0, clipItem->height() / 2.0));
    sendMouse(window, QEvent::MouseButtonPress, menuPoint, Qt::RightButton,
              Qt::RightButton);
    sendMouse(window, QEvent::MouseButtonRelease, menuPoint, Qt::RightButton,
              Qt::NoButton);
    QTest::qWait(150);

    QObject *clipMenu =
        window->findChild<QObject *>(QStringLiteral("clip-context-menu"));
    ASSERT_NE(clipMenu, nullptr);
    EXPECT_TRUE(clipMenu->property("visible").toBool());
    for (const auto &name : {
             QStringLiteral("clip-menu-split"),
             QStringLiteral("clip-menu-delete"),
             QStringLiteral("clip-menu-ripple"),
             QStringLiteral("clip-menu-detach"),
             QStringLiteral("clip-menu-link"),
             QStringLiteral("clip-menu-paste") }) {
        EXPECT_NE(window->findChild<QObject *>(name), nullptr)
            << name.toStdString();
    }

    // The menu's split action targets the right-clicked clip at the playhead,
    // not another lane's clip.
    controller.timeline()->seekUs(1'000'000);
    const QString menuTargetId = controller.document()
                                     ->trackById(QStringLiteral("track-v1"))
                                     ->clips.first().id;
    const int v1BeforeMenuSplit = int(
        controller.document()->trackById(QStringLiteral("track-v1"))
            ->clips.size());
    QObject *splitItem =
        window->findChild<QObject *>(QStringLiteral("clip-menu-split"));
    ASSERT_NE(splitItem, nullptr);
    QMetaObject::invokeMethod(splitItem, "clicked");
    QTest::qWait(100);
    EXPECT_EQ(int(controller.document()->trackById(QStringLiteral("track-v1"))
                      ->clips.size()),
              v1BeforeMenuSplit + 1)
        << "clip-menu split must target the right-clicked clip";
    EXPECT_NE(findClip(controller.document()->project(), menuTargetId),
              nullptr)
        << "the target clip must still exist as one of the split halves";

    EXPECT_NE(findItem(window, QStringLiteral("selection-context-bar")),
              nullptr);

    // --- Status chips reflect the bound controller state. ----------------
    QQuickItem *snapToggle = findItem(window, QStringLiteral("snap-toggle"));
    QQuickItem *proxyToggle = findItem(window, QStringLiteral("proxy-toggle"));
    ASSERT_NE(snapToggle, nullptr);
    ASSERT_NE(proxyToggle, nullptr);
    EXPECT_EQ(snapToggle->property("checked").toBool(),
              controller.timeline()->snapEnabled());
    EXPECT_EQ(proxyToggle->property("checked").toBool(),
              controller.useProxy());
}

TEST(TimelineQml, ExportEntryDisabledOnEmptyTimeline)
{
    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    ASSERT_EQ(clipCount(controller.document()->project()), 0);

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key/click delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);

    QQuickItem *exportButton =
        findItem(window, QStringLiteral("exportButton"));
    ASSERT_NE(exportButton, nullptr);
    EXPECT_FALSE(exportButton->isEnabled())
        << "export must be disabled before any clip exists";
}

TEST(TimelineQml, BgmLandsOnItsOwnAudioLane)
{
    const QString video = fixture(QStringLiteral("h264.mp4"));
    const QString music = fixture(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(video) || !QFileInfo::exists(music))
        GTEST_SKIP() << "missing fixtures";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(video),
                           QUrl::fromLocalFile(music)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 2);
    const QString videoId = controller.document()->project().assets.at(0).id;
    const QString musicId = controller.document()->project().assets.at(1).id;

    // The video's original audio occupies A1.
    controller.addAssetToTimeline(videoId);
    ASSERT_EQ(controller.document()->trackById(QStringLiteral("track-a1"))
                  ->clips.size(),
              1);

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key/click delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for click delivery";

    // Double-click the music item with no explicit target lane.
    QQuickItem *musicItem =
        findItem(window, QStringLiteral("media-item-") + musicId);
    ASSERT_NE(musicItem, nullptr);
    doubleClickItem(window, musicItem);
    QTest::qWait(120);

    const Project &project = controller.document()->project();
    int audioTracks = 0;
    QString musicTrackId;
    for (const Track &track : project.tracks) {
        if (track.type != TrackType::Audio)
            continue;
        ++audioTracks;
        for (const Clip &clip : track.clips) {
            if (clip.assetId == musicId)
                musicTrackId = track.id;
        }
    }
    EXPECT_GE(audioTracks, 2) << "a separate BGM lane must be created";
    EXPECT_FALSE(musicTrackId.isEmpty());
    EXPECT_NE(musicTrackId, QStringLiteral("track-a1"))
        << "BGM must not overlap the video's original audio on A1";
    EXPECT_EQ(controller.document()->trackById(QStringLiteral("track-a1"))
                  ->clips.size(),
              1)
        << "A1 must keep only the original audio clip";
    EXPECT_FALSE(controller.timeline()->selectedClipId().isEmpty())
        << "the inserted BGM clip should be selected";
}

TEST(TimelineQml, PlayheadAutoFollowsDuringSeeks)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());
    // Extend the project so the playhead can move far beyond the viewport.
    const QString cueId = controller.timeline()->addCueAtPlayhead();
    ASSERT_FALSE(cueId.isEmpty());
    ASSERT_TRUE(controller.timeline()->setCueTiming(cueId, 60'000'000,
                                                    62'000'000));
    controller.timeline()->setPixelsPerSecond(80.0);

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key/click delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for scroll delivery";

    QQuickItem *flick =
        findItem(window, QStringLiteral("timeline-flickable"));
    ASSERT_NE(flick, nullptr);
    EXPECT_NEAR(flick->property("contentX").toDouble(), 0.0, 0.001);

    controller.timeline()->seekUs(50'000'000);
    QTest::qWait(120);
    EXPECT_GT(flick->property("contentX").toDouble(), 0.0)
        << "the timeline must scroll to keep the playhead visible";
}

TEST(TimelineQml, OneClickSubtitleCommitsPreviousCueText)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);

    QQuickItem *subtitleAdd = findItem(window, QStringLiteral("subtitle-add"));
    ASSERT_NE(subtitleAdd, nullptr);

    clickItem(window, subtitleAdd);
    QTest::qWait(120);
    const QString firstCueId = controller.timeline()->selectedCueId();
    ASSERT_FALSE(firstCueId.isEmpty());
    QQuickItem *subtitleText =
        findItem(window, QStringLiteral("subtitle-text"));
    ASSERT_NE(subtitleText, nullptr);
    ASSERT_TRUE(subtitleText->hasActiveFocus());
    typeText(window, QStringLiteral("aaa"));
    QTest::qWait(80);
    EXPECT_EQ(subtitleText->property("text").toString(), QStringLiteral("aaa"));

    // Adding the next cue must commit the previous cue's uncommitted text.
    clickItem(window, subtitleAdd);
    QTest::qWait(150);
    const QString secondCueId = controller.timeline()->selectedCueId();
    ASSERT_FALSE(secondCueId.isEmpty());
    EXPECT_NE(secondCueId, firstCueId);

    QString firstText;
    QString secondText;
    for (const Track &track : controller.document()->project().tracks) {
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id == firstCueId)
                firstText = cue.text;
            else if (cue.id == secondCueId)
                secondText = cue.text;
        }
    }
    EXPECT_EQ(firstText, QStringLiteral("aaa"))
        << "the previous cue's typed text must be committed before adding";

    ASSERT_TRUE(subtitleText->hasActiveFocus());
    typeText(window, QStringLiteral("bbb"));
    QTest::qWait(80);
    // Lose focus so the new cue commits too.
    if (QQuickItem *importButton =
            findItem(window, QStringLiteral("import-button")))
        importButton->forceActiveFocus();
    QTest::qWait(100);
    for (const Track &track : controller.document()->project().tracks) {
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id == secondCueId)
                secondText = cue.text;
        }
    }
    EXPECT_EQ(secondText, QStringLiteral("bbb"));
}

TEST(TimelineQml, InspectorMuteBindingSurvivesSelectionChanges)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"),
                                          5'000'000)
                     .isEmpty());
    const Track *audioTrack =
        controller.document()->trackById(QStringLiteral("track-a1"));
    ASSERT_NE(audioTrack, nullptr);
    ASSERT_EQ(audioTrack->clips.size(), 2);
    const QString clipA = audioTrack->clips.at(0).id;
    const QString clipB = audioTrack->clips.at(1).id;

    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for click delivery";

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);

    QQuickItem *mute = findItem(window, QStringLiteral("inspector-mute"));
    ASSERT_NE(mute, nullptr);

    controller.timeline()->selectClip(clipA);
    controller.timeline()->setClipVolume(clipA, 1500);
    QTest::qWait(120);
    EXPECT_TRUE(mute->isVisible());
    EXPECT_FALSE(mute->property("checked").toBool());
    clickItem(window, mute);
    QTest::qWait(100);
    EXPECT_EQ(findClip(controller.document()->project(), clipA)
                  ->audio.volumeMilli,
              0);
    EXPECT_TRUE(mute->property("checked").toBool());
    clickItem(window, mute);
    QTest::qWait(100);
    EXPECT_EQ(findClip(controller.document()->project(), clipA)
                  ->audio.volumeMilli,
              1500);
    EXPECT_FALSE(mute->property("checked").toBool());

    // Switching clips must not restore the previous clip's saved volume.
    controller.timeline()->selectClip(clipB);
    controller.timeline()->setClipVolume(clipB, 800);
    QTest::qWait(120);
    EXPECT_FALSE(mute->property("checked").toBool());
    clickItem(window, mute);
    QTest::qWait(100);
    EXPECT_EQ(findClip(controller.document()->project(), clipB)
                  ->audio.volumeMilli,
              0);
    clickItem(window, mute);
    QTest::qWait(100);
    EXPECT_EQ(findClip(controller.document()->project(), clipB)
                  ->audio.volumeMilli,
              800)
        << "unmute must restore this clip's volume, not the previous clip's";
}

TEST(TimelineQml, SpeedEntryAcceptsCustomMultiplier)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());
    const Track *track = controller.document()->trackById(
        QStringLiteral("track-v1"));
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->clips.size(), 1);
    const QString clipId = track->clips.first().id;
    controller.timeline()->selectClip(clipId);

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);

    QQuickItem *speedBox =
        findItem(window, QStringLiteral("inspector-speed"));
    ASSERT_NE(speedBox, nullptr);
    ASSERT_TRUE(speedBox->isEnabled());
    QQuickItem *speedEditor = qobject_cast<QQuickItem *>(
        speedBox->property("contentItem").value<QObject *>());
    ASSERT_NE(speedEditor, nullptr);
    speedEditor->forceActiveFocus();
    QTest::qWait(50);
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_2);
    QTest::keyClick(window, Qt::Key_X);
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(120);

    EXPECT_EQ(findClip(controller.document()->project(), clipId)->speedMilli,
              2000)
        << "typing 2x and pressing Enter must set the selected clip to 2x";
}

TEST(TimelineQml, QuestionMarkOpensShortcutHelp)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0)
                     .isEmpty());

    QQmlEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = showMainWindow(engine, controller, root);
    ASSERT_NE(window, nullptr);

    if (QQuickItem *focusSink =
            findItem(window, QStringLiteral("import-button")))
        focusSink->forceActiveFocus();
    QTest::qWait(50);
    QObject *help = window->findChild<QObject *>(
        QStringLiteral("shortcut-help-dialog"));
    ASSERT_NE(help, nullptr);
    EXPECT_FALSE(help->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Question);
    QTest::qWait(100);
    EXPECT_TRUE(help->property("opened").toBool())
        << "the ? key must open the shortcut-help dialog";
}
