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
    QTest::qWait(100);
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

    // The track add/reorder menu and header controls are reachable from QML.
    EXPECT_NE(findItem(window, QStringLiteral("track-menu-button")), nullptr);
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
