#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "AppController.h"
#include "AudioSink.h"
#include "ExportController.h"
#include "ExportEncoderCatalog.h"
#include "MaskSidecarStore.h"
#include "PreviewController.h"
#include "RemovalController.h"
#include "TimelineController.h"
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
    for (QQuickItem *child : parent->childItems()) {
        if (QQuickItem *found = findVisualItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *findItem(QQuickWindow *window, const QString &name)
{
    if (QQuickItem *found = findVisualItem(window->contentItem(), name))
        return found;
    return window->findChild<QQuickItem *>(name);
}

void clickItem(QQuickWindow *window, QQuickItem *item)
{
    const QPointF point = item->mapToScene(
        QPointF(item->width() / 2.0, item->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      point.toPoint());
    QTest::qWait(120);
}

Project removalProject(const QString &assetPath, const QString &maskHash,
                       const QString &trackingHash)
{
    MediaInfo info;
    info.path = assetPath;
    info.container = QStringLiteral("mp4");
    info.videoCodec = QStringLiteral("h264");
    info.width = 640;
    info.height = 360;
    info.frameRate = Rational{30, 1};
    info.durationUs = 2'000'000;
    info.hasVideo = true;
    info.supported = true;
    info.kind = MediaKind::Video;

    Project project;
    project.assets.append(
        Asset{QStringLiteral("asset-1"), assetPath, info, {}});
    Track track;
    track.id = QStringLiteral("track-v1");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = QStringLiteral("asset-1");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 2'000'000;
    track.clips.append(clip);

    RemovalLayer layer;
    layer.id = QStringLiteral("rl-1");
    layer.clipId = clip.id;
    layer.sourceInUs = 0;
    layer.sourceOutUs = 2'000'000;
    layer.maskKeyframes.append(
        MaskKeyframe{QStringLiteral("mk-1"), 0, maskHash, 4, 1});
    layer.trackingState = RemovalTrackingState::Complete;
    layer.trackingConfidenceHash = trackingHash;
    layer.parameters.processingWidth = 640;
    layer.parameters.processingHeight = 360;
    track.clips[0].removalLayerId = layer.id;
    project.removalLayers.append(layer);
    project.tracks.append(track);
    project.ensureSubtitleStyles();
    return project;
}

} // namespace

TEST(RemovalQml, PanelProgressConfidenceAdvancedAndExportPrompt)
{
    const QString video = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(video))
        GTEST_SKIP() << "missing h264.mp4";
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for click delivery";

    registerPreviewType();

    QTemporaryDir sidecarDir;
    ASSERT_TRUE(sidecarDir.isValid());

    // Mask and tracking sidecars live in the same fallback root the app uses.
    MaskSidecarStore store;
    store.setProjectPath(QString(), sidecarDir.path());
    BinaryMask mask;
    mask.width = 640;
    mask.height = 360;
    mask.bits.assign(size_t(640) * size_t(360), 0);
    mask.set(300, 160, true);
    QString maskHash;
    ASSERT_TRUE(store.writeMask(mask, &maskHash, nullptr));
    // Non-uniform spacing exercises the time-proportional segment mapping.
    QJsonArray frames{
        QJsonObject{{QStringLiteral("timeUs"), 0},
                    {QStringLiteral("state"), QStringLiteral("good")}},
        QJsonObject{{QStringLiteral("timeUs"), 500'000},
                    {QStringLiteral("state"), QStringLiteral("low")}},
        QJsonObject{{QStringLiteral("timeUs"), 1'500'000},
                    {QStringLiteral("state"), QStringLiteral("fair")}},
        QJsonObject{{QStringLiteral("timeUs"), 2'000'000},
                    {QStringLiteral("state"), QStringLiteral("missing")}}};
    QString trackingHash;
    ASSERT_TRUE(store.writeTracking(QJsonObject{{QStringLiteral("frames"),
                                                 frames}},
                                    &trackingHash, nullptr));

    AppController controller(nullptr, sidecarDir.filePath(QStringLiteral(
                                            "recovery.json")),
                             nullSinkFactory(), sidecarDir.path());
    controller.document()->setProject(
        removalProject(video, maskHash, trackingHash));
    controller.timeline()->selectClip(QStringLiteral("clip-1"));
    controller.timeline()->seekUs(0);
    controller.removal()->beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(controller.removal()->maskMode());

    QQmlEngine engine;
    engine.addImageProvider(
        QStringLiteral("waveform"),
        new WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Removal"),
                                             controller.removal());
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
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000))
        GTEST_SKIP() << "window was not exposed (no desktop/D3D session)";
    QTest::qWait(300);

    // Mask-mode objectNames must resolve from Main.qml before the modal export
    // dialog is opened.
    for (const QString &name : {
             QStringLiteral("confidence-band"),
             QStringLiteral("confidence-low-jump"),
             QStringLiteral("job-progress-panel"),
             QStringLiteral("job-progress-bar"),
             QStringLiteral("job-cancel-button"),
             QStringLiteral("job-pause-button"),
             QStringLiteral("removal-track-button"),
             QStringLiteral("removal-generate-button"),
             QStringLiteral("removal-compare-toggle"),
             QStringLiteral("removal-advanced-toggle"),
             QStringLiteral("removal-advanced-panel"),
             QStringLiteral("removal-low-confidence-count"),
             QStringLiteral("removal-redo-button"),
             QStringLiteral("removal-live-preview-toggle"),
             QStringLiteral("removal-live-preview-status")}) {
        EXPECT_NE(findItem(window, name), nullptr) << name.toStdString();
    }

    // Gate reason is visible while the release gate is closed. The disclaimer
    // text is always present so the internal run can reveal it verbatim.
    QQuickItem *gateReason =
        findItem(window, QStringLiteral("removal-gate-reason"));
    QQuickItem *disclaimer =
        findItem(window, QStringLiteral("removal-technical-preview"));
    ASSERT_NE(gateReason, nullptr);
    ASSERT_NE(disclaimer, nullptr);
    EXPECT_EQ(disclaimer->property("text").toString(),
              QStringLiteral(
                  "内部技术预览：模型未取得商业许可，仅用于内部技术验证，禁止分发。"));
    if (!controller.aiFeatureEnabled()) {
        EXPECT_TRUE(gateReason->isVisible());
        EXPECT_FALSE(gateReason->property("text").toString().isEmpty());
    }

    // The live preview toggle is present but gated (no tracked layer here).
    QQuickItem *liveToggle =
        findItem(window, QStringLiteral("removal-live-preview-toggle"));
    QQuickItem *liveStatus =
        findItem(window, QStringLiteral("removal-live-preview-status"));
    ASSERT_NE(liveToggle, nullptr);
    ASSERT_NE(liveStatus, nullptr);
    EXPECT_FALSE(liveToggle->isEnabled())
        << "live preview needs a gate and a tracked layer";
    EXPECT_EQ(liveStatus->property("text").toString(),
              QStringLiteral("实时预览未开启"));
    // Force the control enabled so the programmatic toggle exercises the
    // QML -> controller path; the controller still rejects the start (no
    // tracking cache) with a Chinese explanation and updates the status.
    const QString statusBefore = liveStatus->property("text").toString();
    liveToggle->setProperty("enabled", true);
    liveToggle->setProperty("checked", true);
    // Emit the control's own toggled() signal, exactly what a user click
    // produces, so the QML onToggled -> controller path is exercised.
    ASSERT_TRUE(QMetaObject::invokeMethod(liveToggle, "toggled"));
    QTest::qWait(120);
    EXPECT_FALSE(controller.removal()->livePreview());
    EXPECT_FALSE(controller.removal()->livePreviewStatus().isEmpty());
    EXPECT_NE(controller.removal()->livePreviewStatus(),
              QStringLiteral("实时预览未开启"));
    EXPECT_NE(liveStatus->property("text").toString(), statusBefore);

    // Mask-mode-only controls disappear after leaving mask mode.
    QQuickItem *trackButton =
        findItem(window, QStringLiteral("removal-track-button"));
    QQuickItem *generateButton =
        findItem(window, QStringLiteral("removal-generate-button"));
    ASSERT_NE(trackButton, nullptr);
    ASSERT_NE(generateButton, nullptr);
    EXPECT_TRUE(trackButton->isVisible());
    EXPECT_TRUE(generateButton->isVisible());

    // Confidence band reads the sidecar: 2 low/missing frames out of 4.
    QQuickItem *lowCount =
        findItem(window, QStringLiteral("removal-low-confidence-count"));
    ASSERT_NE(lowCount, nullptr);
    EXPECT_EQ(lowCount->property("text").toString(),
              QStringLiteral("低置信 2 帧"));
    QQuickItem *band = findItem(window, QStringLiteral("confidence-band"));
    ASSERT_NE(band, nullptr);
    EXPECT_TRUE(band->isVisible());
    QQuickItem *lowJump =
        findItem(window, QStringLiteral("confidence-low-jump"));
    ASSERT_NE(lowJump, nullptr);
    EXPECT_TRUE(lowJump->isVisible());

    // Time-proportional widths: 0..0.5 s, 0.5..1.5 s, 1.5..2.0 s, 2.0..2.5 s.
    QQuickItem *seg0 =
        findItem(window, QStringLiteral("confidence-segment-0"));
    QQuickItem *seg1 =
        findItem(window, QStringLiteral("confidence-segment-1"));
    ASSERT_NE(seg0, nullptr);
    ASSERT_NE(seg1, nullptr);
    EXPECT_GT(seg0->width(), 1.0) << "first segment must not be 1px";
    EXPECT_GT(seg1->width(), seg0->width())
        << "the 1 s segment must be wider than the 0.5 s segment";

    // Click at 70% (1.75 s) seeks to the 1.5 s segment, not a uniform index.
    const QPointF bandPoint = band->mapToScene(
        QPointF(band->width() * 0.7, band->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      bandPoint.toPoint());
    QTest::qWait(120);
    EXPECT_NEAR(double(controller.timeline()->positionUs()), 1'500'000.0,
                50'000.0);
    EXPECT_FALSE(controller.preview()->playing());

    // The compare control must reach the preview renderer.
    QQuickItem *compare =
        findItem(window, QStringLiteral("removal-compare-toggle"));
    ASSERT_NE(compare, nullptr);
    compare->setProperty("currentIndex", 2);
    QMetaObject::invokeMethod(compare, "activated", Q_ARG(int, 2));
    QTest::qWait(80);
    EXPECT_EQ(controller.preview()->removalCompareMode(),
              QStringLiteral("split"));
    controller.removal()->setSplitPermille(250);
    EXPECT_EQ(controller.preview()->removalSplitPermille(), 250);

    // Advanced panel opens through the toggle; controls are enabled when idle.
    QQuickItem *advancedToggle =
        findItem(window, QStringLiteral("removal-advanced-toggle"));
    QQuickItem *advancedPanel =
        findItem(window, QStringLiteral("removal-advanced-panel"));
    ASSERT_NE(advancedToggle, nullptr);
    ASSERT_NE(advancedPanel, nullptr);
    EXPECT_FALSE(advancedPanel->isVisible());
    clickItem(window, advancedToggle);
    EXPECT_TRUE(advancedPanel->isVisible());
    QQuickItem *advancedMode =
        findItem(window, QStringLiteral("removal-advanced-mode"));
    ASSERT_NE(advancedMode, nullptr);
    EXPECT_TRUE(advancedMode->isEnabled());

    // Leaving mask mode hides the generation controls but keeps the
    // confidence band visible because the selected clip still has a layer.
    controller.removal()->endMask();
    QTest::qWait(120);
    EXPECT_FALSE(trackButton->isVisible());
    EXPECT_FALSE(generateButton->isVisible());
    EXPECT_TRUE(band->isVisible());

    // Export dialog content is created lazily; open it with a deterministic
    // encoder catalog and check the missing-cache prompt actions.
    controller.exportController()->setCatalogForTest(
        EncoderCatalog::withoutHardwareForTest());
    controller.exportController()->refreshCapabilities();
    QObject *dialog =
        window->findChild<QObject *>(QStringLiteral("exportDialog"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openDialog"));
    QTest::qWait(300);
    ASSERT_TRUE(dialog->property("opened").toBool());

    QQuickItem *generateExport =
        findItem(window, QStringLiteral("export-generate-removal"));
    QQuickItem *cancelExport =
        findItem(window, QStringLiteral("export-cancel-removal"));
    ASSERT_NE(generateExport, nullptr);
    ASSERT_NE(cancelExport, nullptr);
    EXPECT_TRUE(controller.exportController()->removalRequired());
    EXPECT_TRUE(generateExport->isVisible());
    EXPECT_TRUE(cancelExport->isVisible());
    EXPECT_TRUE(generateExport->property("text").toString()
                    .contains(QStringLiteral("先生成")));
    QQuickItem *promptMessage =
        findItem(window, QStringLiteral("export-removal-message"));
    ASSERT_NE(promptMessage, nullptr);
    EXPECT_TRUE(promptMessage->isVisible());

    // Dismissing the prompt hides it; reopening the dialog shows it again.
    clickItem(window, cancelExport);
    QTest::qWait(100);
    EXPECT_FALSE(promptMessage->isVisible());
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
    QTest::qWait(150);
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openDialog"));
    QTest::qWait(300);
    ASSERT_TRUE(dialog->property("opened").toBool());
    EXPECT_TRUE(promptMessage->isVisible());
}
