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

    // Dragging one fade slider must preserve the other fade, even after
    // switching selection between clips with different fade values.
    const QString clip2Id = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 5'000'000);
    ASSERT_FALSE(clip2Id.isEmpty());
    controller.timeline()->setClipFades(clipId, 300'000, 400'000);
    controller.timeline()->setClipFades(clip2Id, 100'000, 900'000);
    controller.timeline()->selectClip(clip2Id);
    QTest::qWait(80);
    controller.timeline()->selectClip(clipId);
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
        findClip(controller.document()->project(), clipId);
    ASSERT_NE(afterFadeIn, nullptr);
    const qint64 fadeInAfterDrag = afterFadeIn->audio.fadeInUs;
    EXPECT_NE(fadeInAfterDrag, 300'000);
    EXPECT_EQ(afterFadeIn->audio.fadeOutUs, 400'000)
        << "dragging fade-in must preserve the sibling fade-out";

    dragSlider(fadeOut, 0.8, 0.3);
    const Clip *afterFadeOut =
        findClip(controller.document()->project(), clipId);
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
