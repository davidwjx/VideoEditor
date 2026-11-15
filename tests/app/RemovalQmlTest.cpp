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

QQuickItem *findItem(QQuickWindow *window, const QString &name)
{
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
    QJsonArray frames{
        QJsonObject{{QStringLiteral("timeUs"), 0},
                    {QStringLiteral("state"), QStringLiteral("good")}},
        QJsonObject{{QStringLiteral("timeUs"), 500'000},
                    {QStringLiteral("state"), QStringLiteral("low")}},
        QJsonObject{{QStringLiteral("timeUs"), 1'000'000},
                    {QStringLiteral("state"), QStringLiteral("fair")}},
        QJsonObject{{QStringLiteral("timeUs"), 1'500'000},
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
             QStringLiteral("removal-low-confidence-count")}) {
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

    // Clicking the confidence strip seeks to the clicked frame and pauses.
    const QPointF bandPoint = band->mapToScene(
        QPointF(band->width() * 0.8, band->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      bandPoint.toPoint());
    QTest::qWait(120);
    EXPECT_NEAR(double(controller.timeline()->positionUs()), 1'500'000.0,
                50'000.0);
    EXPECT_FALSE(controller.preview()->playing());

    // Advanced panel opens through the toggle.
    QQuickItem *advancedToggle =
        findItem(window, QStringLiteral("removal-advanced-toggle"));
    QQuickItem *advancedPanel =
        findItem(window, QStringLiteral("removal-advanced-panel"));
    ASSERT_NE(advancedToggle, nullptr);
    ASSERT_NE(advancedPanel, nullptr);
    EXPECT_FALSE(advancedPanel->isVisible());
    clickItem(window, advancedToggle);
    EXPECT_TRUE(advancedPanel->isVisible());

    // Leaving mask mode hides the generation controls.
    controller.removal()->endMask();
    QTest::qWait(120);
    EXPECT_FALSE(trackButton->isVisible());
    EXPECT_FALSE(generateButton->isVisible());

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
    EXPECT_TRUE(findItem(window, QStringLiteral("export-removal-message"))
                    ->isVisible());
}
