#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include "AiJobManager.h"
#include "AiTransport.h"
#include "AppController.h"
#include "AudioSink.h"
#include "ExportController.h"
#include "ExportEncoderCatalog.h"
#include "MaskSidecarStore.h"
#include "ModelRegistry.h"
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
    // The mask-mode project starts with a keyframe whose mask is all zero, so
    // the first-run hint and the add-keyframe guard are exercised. Painting a
    // stroke replaces it with a real mask.
    BinaryMask emptyMask;
    emptyMask.width = 640;
    emptyMask.height = 360;
    emptyMask.bits.assign(size_t(640) * size_t(360), 0);
    QString emptyMaskHash;
    ASSERT_TRUE(store.writeMask(emptyMask, &emptyMaskHash, nullptr));
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
        removalProject(video, emptyMaskHash, trackingHash));
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
             QStringLiteral("removal-erase-button"),
             QStringLiteral("removal-erase-tool-button"),
             QStringLiteral("removal-status-text"),
             QStringLiteral("removal-progress-line"),
             QStringLiteral("removal-explainer"),
             QStringLiteral("removal-compare-toggle"),
             QStringLiteral("removal-advanced-toggle"),
             QStringLiteral("removal-advanced-panel"),
             QStringLiteral("removal-low-confidence-count"),
             QStringLiteral("removal-redo-button"),
             QStringLiteral("removal-live-preview-toggle"),
             QStringLiteral("removal-live-preview-status"),
             QStringLiteral("removal-first-run-hint"),
             QStringLiteral("removal-action-row"),
             QStringLiteral("removal-action-disabled-reason"),
             QStringLiteral("removal-add-keyframe"),
             QStringLiteral("removal-tracking-state"),
             QStringLiteral("removal-layer-enabled"),
             QStringLiteral("removal-layer-disabled-chip"),
             QStringLiteral("removal-brush-value"),
             QStringLiteral("removal-feather-value"),
             QStringLiteral("removal-split-value"),
             QStringLiteral("removal-advanced-invalidation-warning"),
             QStringLiteral("confidence-band-hit-area"),
             QStringLiteral("job-progress-paused")}) {
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
    EXPECT_EQ(liveToggle->property("text").toString(),
              QStringLiteral("实时预览"))
        << "the label must not promise GPU inference";
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

    // First-run guidance is visible while the working mask is empty, and the
    // add-keyframe action is disabled until the user paints.
    QQuickItem *addKeyframe =
        findItem(window, QStringLiteral("removal-add-keyframe"));
    QQuickItem *firstRunHint =
        findItem(window, QStringLiteral("removal-first-run-hint"));
    QQuickItem *actionReason =
        findItem(window, QStringLiteral("removal-action-disabled-reason"));
    QQuickItem *trackingState =
        findItem(window, QStringLiteral("removal-tracking-state"));
    ASSERT_NE(addKeyframe, nullptr);
    ASSERT_NE(firstRunHint, nullptr);
    ASSERT_NE(actionReason, nullptr);
    ASSERT_NE(trackingState, nullptr);
    EXPECT_TRUE(controller.removal()->currentMaskEmpty());
    EXPECT_FALSE(addKeyframe->isEnabled());
    EXPECT_TRUE(firstRunHint->isVisible());
    // 开始消除 stays disabled until the user paints something.
    QQuickItem *eraseBeforePaint =
        findItem(window, QStringLiteral("removal-erase-button"));
    ASSERT_NE(eraseBeforePaint, nullptr);
    EXPECT_FALSE(eraseBeforePaint->isEnabled());
    // The disabled-reason label is only needed when an action is blocked (gate
    // closed); in the technical-preview run there are keyframes and no reason.
    EXPECT_EQ(actionReason->isVisible(), !controller.aiFeatureEnabled());
    EXPECT_EQ(trackingState->property("text").toString(),
              QStringLiteral("跟踪：完成"));

    // Mask mode disarms destructive timeline shortcuts.
    for (const QString &name : {
             QStringLiteral("shortcut-split"),
             QStringLiteral("shortcut-delete"),
             QStringLiteral("shortcut-ripple-delete"),
             QStringLiteral("shortcut-paste")}) {
        QObject *shortcut = window->findChild<QObject *>(name);
        ASSERT_NE(shortcut, nullptr) << name.toStdString();
        EXPECT_FALSE(shortcut->property("enabled").toBool())
            << name.toStdString();
    }

    // The first stroke creates a keyframe; the hint disappears and the add
    // action becomes available for extra keyframes.
    controller.removal()->beginStroke(20, false);
    controller.removal()->continueStroke(QPointF(0.4, 0.4),
                                         QPointF(0.6, 0.6));
    controller.removal()->endStroke();
    QTest::qWait(120);
    EXPECT_FALSE(controller.removal()->currentMaskEmpty());
    EXPECT_TRUE(addKeyframe->isEnabled());
    EXPECT_FALSE(firstRunHint->isVisible());
    // With the release gate closed the inline reason still explains why the
    // actions are unavailable; only hide it when the gate is actually on.
    if (controller.aiFeatureEnabled())
        EXPECT_FALSE(actionReason->isVisible());

    // Numeric slider feedback and the enlarged confidence hit target.
    QQuickItem *brushValue =
        findItem(window, QStringLiteral("removal-brush-value"));
    QQuickItem *featherValue =
        findItem(window, QStringLiteral("removal-feather-value"));
    QQuickItem *hitArea =
        findItem(window, QStringLiteral("confidence-band-hit-area"));
    ASSERT_NE(brushValue, nullptr);
    ASSERT_NE(featherValue, nullptr);
    ASSERT_NE(hitArea, nullptr);
    EXPECT_TRUE(brushValue->property("text").toString().endsWith("px"));
    EXPECT_TRUE(featherValue->property("text").toString().endsWith("px"));
    EXPECT_GE(hitArea->height(), 20.0);

    QQuickItem *pausedLabel =
        findItem(window, QStringLiteral("job-progress-paused"));
    ASSERT_NE(pausedLabel, nullptr);
    EXPECT_EQ(pausedLabel->property("text").toString(),
              QStringLiteral("已暂停"));
    QObject *toastTimer =
        window->findChild<QObject *>(QStringLiteral("error-toast-timer"));
    ASSERT_NE(toastTimer, nullptr);
    EXPECT_EQ(toastTimer->property("interval").toInt(), 8000);

    // The power-user track/generate buttons now live inside AI 高级设置 and
    // stay hidden while the advanced panel is collapsed.
    QQuickItem *trackButton =
        findItem(window, QStringLiteral("removal-track-button"));
    QQuickItem *generateButton =
        findItem(window, QStringLiteral("removal-generate-button"));
    ASSERT_NE(trackButton, nullptr);
    ASSERT_NE(generateButton, nullptr);
    EXPECT_FALSE(trackButton->isVisible());
    EXPECT_FALSE(generateButton->isVisible());

    // The one-button action row is the only visible erase entry point. Its
    // enabled state follows the gate + painted mask, and its label reflects
    // the cache state.
    QQuickItem *eraseButton =
        findItem(window, QStringLiteral("removal-erase-button"));
    ASSERT_NE(eraseButton, nullptr);
    EXPECT_TRUE(eraseButton->isVisible());
    EXPECT_EQ(eraseButton->property("text").toString(),
              QStringLiteral("开始消除"));
    EXPECT_EQ(eraseButton->isEnabled(),
              controller.aiFeatureEnabled()
                  && controller.removal()->maskHasPixels());
    QQuickItem *explainer =
        findItem(window, QStringLiteral("removal-explainer"));
    ASSERT_NE(explainer, nullptr);
    EXPECT_TRUE(explainer->property("text").toString()
                    .contains(QStringLiteral("开始消除")));

    // Confidence band reads the sidecar: 2 low/missing frames out of 4.
    QQuickItem *lowCount =
        findItem(window, QStringLiteral("removal-low-confidence-count"));
    ASSERT_NE(lowCount, nullptr);
    EXPECT_EQ(lowCount->property("text").toString(),
              QStringLiteral("有 2 帧低置信"));
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
    // Invoke the button's clicked signal: the inspector may scroll this control
    // below the fold now that the mask actions live at the top of the section.
    ASSERT_TRUE(QMetaObject::invokeMethod(advancedToggle, "clicked"));
    QTest::qWait(80);
    EXPECT_TRUE(advancedPanel->isVisible());
    // The demoted single-stage commands are reachable here for power users.
    EXPECT_TRUE(trackButton->isVisible());
    EXPECT_TRUE(generateButton->isVisible());
    QQuickItem *advancedMode =
        findItem(window, QStringLiteral("removal-advanced-mode"));
    ASSERT_NE(advancedMode, nullptr);
    EXPECT_TRUE(advancedMode->isEnabled());
    QQuickItem *advancedWarning = findItem(
        window, QStringLiteral("removal-advanced-invalidation-warning"));
    ASSERT_NE(advancedWarning, nullptr);
    EXPECT_TRUE(advancedWarning->isVisible());
    EXPECT_TRUE(advancedWarning->property("text").toString()
                    .contains(QStringLiteral("失效")));

    // Leaving mask mode hides the generation controls but keeps the
    // confidence band visible because the selected clip still has a layer.
    controller.removal()->endMask();
    QTest::qWait(120);
    EXPECT_FALSE(trackButton->isVisible());
    EXPECT_FALSE(generateButton->isVisible());
    EXPECT_TRUE(band->isVisible());
    // Compare/split and the layer switch stay available for review once the
    // selected clip has a tracked layer, even outside mask mode.
    EXPECT_TRUE(compare->isVisible());
    QQuickItem *layerToggle =
        findItem(window, QStringLiteral("removal-layer-enabled"));
    ASSERT_NE(layerToggle, nullptr);
    EXPECT_TRUE(layerToggle->isVisible());
    EXPECT_TRUE(layerToggle->isEnabled());

    // Disabling the layer outside mask mode shows the chip and flips the
    // switch; re-enabling keeps the later export-preflight check meaningful.
    controller.removal()->setLayerEnabled(false);
    QTest::qWait(120);
    QQuickItem *disabledChip =
        findItem(window, QStringLiteral("removal-layer-disabled-chip"));
    ASSERT_NE(disabledChip, nullptr);
    EXPECT_TRUE(disabledChip->isVisible());
    EXPECT_FALSE(layerToggle->property("checked").toBool());
    controller.removal()->setLayerEnabled(true);
    QTest::qWait(120);
    EXPECT_FALSE(disabledChip->isVisible());

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
    // The prompt owns a progress/stage/ETA area and a real generation cancel
    // action; both exist even while no generation is running.
    for (const QString &name : {
             QStringLiteral("export-removal-progress"),
             QStringLiteral("export-removal-progress-bar"),
             QStringLiteral("export-removal-stage"),
             QStringLiteral("export-removal-eta"),
             QStringLiteral("export-removal-cancel-generation")}) {
        EXPECT_NE(findItem(window, name), nullptr) << name.toStdString();
    }

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
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
    QTest::qWait(150);

    // Clicking 开始消除 must call Removal.erase(): deleting the layer makes the
    // controller reject the click immediately with its Chinese explanation,
    // so the wiring is proven without starting a real inference job.
    // deleteLayer() acts on the mask-editor working layer, so re-enter mask
    // mode before removing it.
    controller.removal()->beginMask(QStringLiteral("clip-1"));
    QTest::qWait(80);
    controller.removal()->deleteLayer();
    QTest::qWait(80);
    ASSERT_NE(findItem(window, QStringLiteral("removal-erase-button")),
              nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(eraseButton, "clicked"));
    QTest::qWait(80);
    EXPECT_EQ(controller.removal()->error(),
              QStringLiteral("请先绘制消除遮罩。"));
}

namespace {

// Regression harness for the field report "点击取消后任务一直跑到自然结束":
// a tracking job must actually stop when the job panel's 取消 button is
// clicked. The fake transport below never finishes on its own, so generating
// can only flip back to false if the click really reached Removal.cancel().
class HangingJobTransport : public AiTransport {
public:
    bool start(const AiJobSpec &spec, QString *) override
    {
        m_spec = spec;
        QTimer::singleShot(0, this, [this] {
            emit message(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("progress")},
                {QStringLiteral("stage"), QStringLiteral("tracking")},
                {QStringLiteral("frame"), 5},
                {QStringLiteral("totalFrames"), 60},
                {QStringLiteral("percent"), 8.0},
                {QStringLiteral("etaMs"), 4000.0}});
        });
        return true;
    }

    void cancel() override
    {
        // A real engine still emits progress for a few frames while it winds
        // down; the test drives that window explicitly so the "正在取消…"
        // feedback can be observed before the job finishes.
        m_cancelCalled = true;
    }

    void finishNow()
    {
        emit message(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("cancelled")},
            {QStringLiteral("completedFrames"), 5},
            {QStringLiteral("trackingState"), QStringLiteral("partial")}});
        emit finished(0, QProcess::NormalExit);
    }

    void sendCommand(const QJsonObject &) override {}

    bool cancelCalled() const { return m_cancelCalled; }

private:
    AiJobSpec m_spec;
    bool m_cancelCalled = false;
};

// Writes a minimal but real model directory: dummy .onnx payloads plus a
// manifest whose sha256 entries match, so AiFeatureGate::enableTechnicalPreview
// (VIDEOEDITOR_M4_TECHNICAL=1) verifies and enables the feature exactly as the
// app does for internal runs. This keeps the test on production code paths
// instead of gate test hooks.
bool writeFakeModelsDir(const QString &root, QString *error)
{
    const auto writeFile = [&root](const QString &name,
                                   const QByteArray &bytes) {
        QFile file(QDir(root).filePath(name));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(bytes) == bytes.size();
    };
    const QByteArray payload(2048, Qt::Uninitialized);
    const QStringList names{
        QStringLiteral("fake_encoder.onnx"),
        QStringLiteral("fake_prompt.onnx"),
        QStringLiteral("fake_decoder.onnx"),
        QStringLiteral("fake_migan.onnx"),
        QStringLiteral("fake_lama.onnx")};
    for (const QString &name : names) {
        if (!writeFile(name, payload)) {
            if (error)
                *error = QStringLiteral("无法写入测试模型文件：%1").arg(name);
            return false;
        }
    }
    const auto sha = [&root](const QString &name) {
        QFile file(QDir(root).filePath(name));
        file.open(QIODevice::ReadOnly);
        return QString::fromLatin1(
            QCryptographicHash::hash(file.readAll(),
                                     QCryptographicHash::Sha256).toHex());
    };

    const auto fileEntry = [&sha](const QString &name) {
        return QJsonObject{{QStringLiteral("path"), name},
                           {QStringLiteral("sha256"), sha(name)}};
    };
    const QJsonArray models{
        QJsonObject{
            {QStringLiteral("id"),
             QStringLiteral("efficienttam-ti-512-fp32")},
            {QStringLiteral("family"), QStringLiteral("tracking")},
            {QStringLiteral("version"), QStringLiteral("0.8.0-fp32")},
            {QStringLiteral("files"),
             QJsonObject{
                 {QStringLiteral("image_encoder"),
                  fileEntry(QStringLiteral("fake_encoder.onnx"))},
                 {QStringLiteral("prompt_encoder"),
                  fileEntry(QStringLiteral("fake_prompt.onnx"))},
                 {QStringLiteral("mask_decoder"),
                  fileEntry(QStringLiteral("fake_decoder.onnx"))}}}},
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("migan-512")},
            {QStringLiteral("family"), QStringLiteral("inpainting")},
            {QStringLiteral("version"), QStringLiteral("2.0.0")},
            {QStringLiteral("files"),
             QJsonObject{{QStringLiteral("pipeline"),
                          fileEntry(QStringLiteral("fake_migan.onnx"))}}}},
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("lama-512-int8-allowzero0")},
            {QStringLiteral("family"), QStringLiteral("inpainting")},
            {QStringLiteral("version"), QStringLiteral("1.0.0-int8-allowzero0")},
            {QStringLiteral("files"),
             QJsonObject{{QStringLiteral("model"),
                          fileEntry(QStringLiteral("fake_lama.onnx"))}}}}};
    const QJsonObject manifest{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("pipelineVersion"), QStringLiteral("m4.0")},
        {QStringLiteral("models"), models}};
    QFile manifestFile(QDir(root).filePath(QStringLiteral(
        "models.manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || manifestFile.write(QJsonDocument(manifest).toJson()) <= 0) {
        if (error)
            *error = QStringLiteral("无法写入测试模型清单。");
        return false;
    }
    return true;
}

} // namespace

TEST(RemovalQml, CancelButtonDuringRunningJobActuallyCancels)
{
    const QString video = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(video))
        GTEST_SKIP() << "missing h264.mp4";
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for click delivery";

    registerPreviewType();

    QTemporaryDir sidecarDir;
    ASSERT_TRUE(sidecarDir.isValid());

    // The AI gate is fail-closed, so the app (and its AiJobManager) must see a
    // real technical-preview environment: a manifest whose files hash clean.
    // The transport factory below keeps the actual inference out of the test.
    QTemporaryDir modelsDir;
    ASSERT_TRUE(modelsDir.isValid());
    QString modelsError;
    ASSERT_TRUE(writeFakeModelsDir(modelsDir.path(), &modelsError))
        << modelsError.toStdString();
    qputenv("VIDEOEDITOR_M4_MODELS_DIR", modelsDir.path().toUtf8());
    qputenv("VIDEOEDITOR_M4_TECHNICAL", "1");

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
        QJsonObject{{QStringLiteral("timeUs"), 2'000'000},
                    {QStringLiteral("state"), QStringLiteral("good")}}};
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

    // The environment only mattered while AppController loaded its gate;
    // clear it so sibling tests in this binary stay unaffected.
    qunsetenv("VIDEOEDITOR_M4_TECHNICAL");
    qunsetenv("VIDEOEDITOR_M4_MODELS_DIR");

    HangingJobTransport *fake = nullptr;
    controller.aiJobs()->setTransportFactoryForTest([&fake] {
        auto transport = std::make_unique<HangingJobTransport>();
        fake = transport.get();
        return std::unique_ptr<AiTransport>(transport.release());
    });

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

    ASSERT_TRUE(controller.removal()->track())
        << controller.removal()->property("error").toString().toStdString();
    ASSERT_TRUE(controller.removal()->generating());
    QTest::qWait(300);

    QQuickItem *cancelButton = findItem(window, QStringLiteral("job-cancel-button"));
    ASSERT_NE(cancelButton, nullptr);
    EXPECT_TRUE(cancelButton->isVisible());
    const QPointF buttonCenter =
        cancelButton->mapToScene(QPointF(cancelButton->width() / 2.0,
                                         cancelButton->height() / 2.0));
    qWarning("cancel button scene pos=%.0f,%.0f visible=%d enabled=%d",
             buttonCenter.x(), buttonCenter.y(), int(cancelButton->isVisible()),
             int(cancelButton->isEnabled()));

    clickItem(window, cancelButton);

    // The click must reach the controller immediately, and the UI must say so
    // while the engine winds down: a click that only changes after the engine
    // exits reads as "ignored".
    QTRY_VERIFY_WITH_TIMEOUT(fake->cancelCalled(), 2000);
    EXPECT_TRUE(controller.removal()->property("cancelling").toBool());
    QQuickItem *stage = findItem(window, QStringLiteral("job-progress-stage"));
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(stage->property("text").toString(), QStringLiteral("正在取消…"));
    QQuickItem *inlineCancel =
        findItem(window, QStringLiteral("removal-cancel-button"));
    ASSERT_NE(inlineCancel, nullptr);
    EXPECT_TRUE(inlineCancel->isVisible());
    EXPECT_FALSE(inlineCancel->isEnabled());
    EXPECT_FALSE(cancelButton->isEnabled());

    fake->finishNow();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.removal()->generating(), 3000);
    EXPECT_FALSE(controller.removal()->property("cancelling").toBool());
}
