#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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
    window->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
    QTest::qWait(300);

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
