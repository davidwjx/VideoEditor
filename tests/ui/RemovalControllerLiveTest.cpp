#include <gtest/gtest.h>

#include <atomic>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "AiFeatureGate.h"
#include "D3D11Context.h"
#include "LiveRepairService.h"
#include "MaskCodec.h"
#include "MediaProbe.h"
#include "ModelRegistry.h"
#include "PreviewController.h"
#include "ProjectDocument.h"
#include "RemovalCache.h"
#include "RemovalController.h"
#include "RemovalTestProject.h"
#include "TimelineEvaluator.h"

using namespace VideoEditor;

namespace {

ModelManifest liveManifest()
{
    ModelManifest manifest;
    manifest.commercialApproved = true;
    manifest.legalSignOffBy = QStringLiteral("legal");
    manifest.legalSignOffDate = QStringLiteral("2026-11-15");
    manifest.legalScope = QStringLiteral("commercial");
    manifest.models.append(AiModelRecord{
        QStringLiteral("sam2.1-hiera-tiny"), QStringLiteral("tracking"),
        QStringLiteral("1.0.0"), QStringLiteral("Apache-2.0"),
        QStringLiteral("Apache-2.0"), {}, true,
        {{QStringLiteral("image_encoder"),
          AiModelFile{QStringLiteral("tracking.onnx"), QString(64, 'a')}}},
        {}});
    manifest.models.append(AiModelRecord{
        QStringLiteral("migan-512"), QStringLiteral("inpaint"),
        QStringLiteral("1.0.0"), QStringLiteral("MIT"), QStringLiteral("MIT"),
        {}, true,
        {{QStringLiteral("model"),
          AiModelFile{QStringLiteral("migan.onnx"), QString(64, 'b')}}},
        {}});
    return manifest;
}

Project projectWithLiveLayer()
{
    Project project = projectWithClip();
    RemovalLayer layer = sampleLayer();
    project.removalLayers.append(layer);
    project.tracks[0].clips[0].removalLayerId = layer.id;
    return project;
}

class RecordingProvider : public LiveRepairProvider {
public:
    QString layerId;
    mutable std::atomic<int> queries{0};

    bool patchFor(const QString &id, int64_t,
                  LiveRepairPatch *patch) const override
    {
        ++queries;
        if (id != layerId || !patch)
            return false;
        QImage image(64, 64, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        patch->image = image;
        patch->crop = QRect(0, 0, 64, 64);
        return true;
    }
};

void writeTrackingMask(const QString &cacheDir)
{
    const QString directory =
        QDir(cacheDir).filePath(QStringLiteral("tracking/masks"));
    ASSERT_TRUE(QDir().mkpath(directory));
    BinaryMask mask;
    mask.width = 32;
    mask.height = 18;
    mask.bits.assign(32 * 18, 0);
    mask.set(16, 9, true);
    QFile file(QDir(directory).filePath(QStringLiteral("frame_000000.rle")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray encoded = MaskCodec::encode(mask);
    ASSERT_FALSE(encoded.isEmpty());
    ASSERT_EQ(file.write(encoded), encoded.size());
}

} // namespace

TEST(RemovalControllerLive, BuildsSpecAndTogglesService)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    ProjectDocument document;
    document.setProject(projectWithLiveLayer());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(document, sidecars);

    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    gate.setRegistryForTest(ModelRegistry::forTest(liveManifest(), dir.path()));
    RemovalCache cache(dir.path());
    controller.setFeatureGate(&gate);
    controller.setCache(&cache);
    controller.setSelectedClipProvider([] { return QStringLiteral("clip-1"); });

    const RemovalLayer &layer = document.project().removalLayers.first();
    writeTrackingMask(cache.cacheDir(layer.cacheKey));

    LiveRepairSpec captured;
    bool started = false;
    int stopped = 0;
    controller.setLivePreviewStarter(
        [&](const LiveRepairSpec &spec, QString *) {
            captured = spec;
            started = true;
            return true;
        });
    controller.setLivePreviewStopper([&] { ++stopped; });

    EXPECT_TRUE(controller.livePreviewAvailable());
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("实时预览未开启"));

    controller.setLivePreview(true);
    EXPECT_TRUE(controller.livePreview());
    EXPECT_TRUE(started);
    // A freshly started paused preview has not repaired its target yet.
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("正在修复当前帧…"));
    EXPECT_EQ(captured.layerId, layer.id);
    EXPECT_EQ(captured.assetPath, document.project().assets.first().path);
    EXPECT_EQ(captured.cacheDir, cache.cacheDir(layer.cacheKey));
    EXPECT_EQ(captured.sourceInUs, layer.sourceInUs);
    EXPECT_EQ(captured.sourceOutUs, layer.sourceOutUs);
    EXPECT_EQ(captured.displaySize, QSize(1920, 1080));
    EXPECT_EQ(captured.fps, (Rational{30, 1}));
    EXPECT_EQ(captured.featherPx, 4) << "nearest keyframe feather";
    EXPECT_EQ(captured.mode, RepairMode::Fast);
    EXPECT_TRUE(captured.inpainterModelPath.endsWith(QStringLiteral(
        "migan.onnx")))
        << captured.inpainterModelPath.toStdString();

    controller.setLivePreview(false);
    EXPECT_FALSE(controller.livePreview());
    EXPECT_EQ(stopped, 1);
    EXPECT_EQ(controller.livePreviewStatus(), QStringLiteral("实时预览已关闭"));
}

TEST(RemovalControllerLive, PreviewForwardsProviderAcrossRenderThread)
{
    QString deviceError;
    if (!D3D11Context::create(true, &deviceError)
        && !D3D11Context::create(false, &deviceError)) {
        GTEST_SKIP() << deviceError.toStdString();
    }
    const QString red =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("red.mp4"));
    if (!QFileInfo::exists(red))
        GTEST_SKIP() << "missing red.mp4";
    const auto info = MediaProbe::probe(red);
    if (!info)
        GTEST_SKIP() << "red.mp4 could not be probed";

    Project project;
    project.timeline.width = 64;
    project.timeline.height = 64;
    project.timeline.fps = Rational{30, 1};
    Asset asset;
    asset.id = QStringLiteral("asset-1");
    asset.path = red;
    asset.info = *info;
    project.assets.append(asset);
    Track track;
    track.id = QStringLiteral("track-v");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = asset.id;
    clip.sourceInUs = 0;
    clip.sourceOutUs = info->durationUs;
    clip.timelineStartUs = 0;
    clip.removalLayerId = QStringLiteral("rl-live");
    track.clips.append(clip);
    project.tracks.append(track);
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-live");
    layer.clipId = clip.id;
    layer.sourceInUs = 0;
    layer.sourceOutUs = info->durationUs;
    // Missing cache: only the live provider can produce a patch.
    layer.cacheKey = QString(64, QLatin1Char('f'));
    layer.enabled = true;
    project.removalLayers.append(layer);

    auto provider = std::make_shared<RecordingProvider>();
    provider->layerId = layer.id;

    PreviewController preview;
    preview.setLiveRepairProvider(provider);
    preview.openTimeline(project, info->durationUs, false);
    QTRY_VERIFY_WITH_TIMEOUT(provider->queries > 0, 10000);
    EXPECT_GT(provider->queries.load(), 0)
        << "the queued shared provider never reached the render worker";
}

TEST(RemovalControllerLive, RequiresTrackingMasksBeforeStarting)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    ProjectDocument document;
    document.setProject(projectWithLiveLayer());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(document, sidecars);

    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    gate.setRegistryForTest(ModelRegistry::forTest(liveManifest(), dir.path()));
    RemovalCache cache(dir.path());
    controller.setFeatureGate(&gate);
    controller.setCache(&cache);
    controller.setSelectedClipProvider([] { return QStringLiteral("clip-1"); });

    bool started = false;
    controller.setLivePreviewStarter(
        [&](const LiveRepairSpec &, QString *) {
            started = true;
            return true;
        });

    EXPECT_FALSE(controller.livePreviewAvailable());
    controller.setLivePreview(true);
    EXPECT_FALSE(controller.livePreview());
    EXPECT_FALSE(started) << "a missing tracking run must not start a worker";
    EXPECT_TRUE(controller.livePreviewStatus().contains(
        QStringLiteral("跟踪")))
        << controller.livePreviewStatus().toStdString();
    EXPECT_TRUE(controller.error().contains(QStringLiteral("请先运行 AI 跟踪")))
        << controller.error().toStdString();
}

TEST(RemovalControllerLive, StatusIsTargetSpecificAndShowsBackend)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectDocument document;
    document.setProject(projectWithLiveLayer());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(document, sidecars);

    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    gate.setRegistryForTest(ModelRegistry::forTest(liveManifest(), dir.path()));
    RemovalCache cache(dir.path());
    controller.setFeatureGate(&gate);
    controller.setCache(&cache);
    controller.setSelectedClipProvider([] { return QStringLiteral("clip-1"); });
    writeTrackingMask(cache.cacheDir(
        document.project().removalLayers.first().cacheKey));

    bool playing = false;
    controller.setPlaybackStateProvider([&] { return playing; });
    bool started = false;
    controller.setLivePreviewStarter(
        [&](const LiveRepairSpec &, QString *) {
            started = true;
            return true;
        });
    controller.setLivePreviewStopper([] {});

    ASSERT_TRUE(controller.livePreviewAvailable());
    controller.setLivePreview(true);
    ASSERT_TRUE(controller.livePreview());
    ASSERT_TRUE(started);

    // Paused start: no frame is ready yet.
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("正在修复当前帧…"));
    // A playhead change resets readiness even if an old frame was ready.
    controller.notifyLivePreviewTargetReady();
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("实时预览就绪（当前帧）"));
    controller.notifyLivePreviewTargetChanged();
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("正在修复当前帧…"));

    // Backend is reported only after the model session exists.
    controller.setLivePreviewBackend(QStringLiteral("cpu"));
    EXPECT_EQ(controller.livePreviewBackend(), QStringLiteral("CPU，较慢"));
    controller.notifyLivePreviewTargetReady();
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("实时预览就绪（当前帧，CPU，较慢）"));

    // Playback status wins over per-frame readiness so the label does not
    // flicker between "ready" and "running".
    playing = true;
    controller.notifyLivePreviewTargetChanged();
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("实时预览运行中（未就绪帧显示原片/缓存；CPU，较慢）"));
    controller.notifyLivePreviewTargetReady();
    EXPECT_EQ(controller.livePreviewStatus(),
              QStringLiteral("实时预览运行中（未就绪帧显示原片/缓存；CPU，较慢）"));

    controller.setLivePreviewBackend(QStringLiteral("dml"));
    EXPECT_EQ(controller.livePreviewBackend(), QStringLiteral("DML"));
    EXPECT_TRUE(controller.livePreviewStatus().contains(QStringLiteral("DML")));

    controller.setLivePreview(false);
    EXPECT_FALSE(controller.livePreview());
}

TEST(RemovalControllerLive, PartialTrackingCannotStartLivePreview)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    Project project = projectWithLiveLayer();
    project.removalLayers.first().trackingState =
        RemovalTrackingState::Partial;
    ProjectDocument document;
    document.setProject(project);
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(document, sidecars);

    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    gate.setRegistryForTest(ModelRegistry::forTest(liveManifest(), dir.path()));
    RemovalCache cache(dir.path());
    controller.setFeatureGate(&gate);
    controller.setCache(&cache);
    controller.setSelectedClipProvider([] { return QStringLiteral("clip-1"); });
    writeTrackingMask(cache.cacheDir(
        document.project().removalLayers.first().cacheKey));

    bool started = false;
    controller.setLivePreviewStarter(
        [&](const LiveRepairSpec &, QString *) {
            started = true;
            return true;
        });

    EXPECT_EQ(controller.trackingStateText(), QStringLiteral("部分"));
    EXPECT_FALSE(controller.livePreviewAvailable())
        << "partial tracking masks are not trustworthy enough for live preview";
    controller.setLivePreview(true);
    EXPECT_FALSE(controller.livePreview());
    EXPECT_FALSE(started);
    EXPECT_TRUE(controller.error().contains(QStringLiteral("跟踪尚未完成")))
        << controller.error().toStdString();
}
