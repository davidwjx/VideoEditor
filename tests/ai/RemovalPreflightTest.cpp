#include <gtest/gtest.h>

#include <algorithm>

#include <QColor>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "RemovalPreflight.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

namespace {

ModelManifest approvedManifest()
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
          AiModelFile{QStringLiteral("unused.onnx"),
                      QString(64, QLatin1Char('a'))}}},
        {}});
    manifest.models.append(AiModelRecord{
        QStringLiteral("migan-512"), QStringLiteral("inpaint"),
        QStringLiteral("1.0.0"), QStringLiteral("MIT"),
        QStringLiteral("MIT"), {}, true,
        {{QStringLiteral("model"),
          AiModelFile{QStringLiteral("unused.onnx"),
                      QString(64, QLatin1Char('b'))}}},
        {}});
    // Real manifest ordering: the fp16-only record appears before the int8
    // allowzero graph. Quality must resolve the int8 id, not this one.
    manifest.models.append(AiModelRecord{
        QStringLiteral("lama-512"), QStringLiteral("inpaint"),
        QStringLiteral("1.0.0"), QStringLiteral("Apache-2.0"),
        QStringLiteral("Apache-2.0"), {}, true,
        {{QStringLiteral("model"),
          AiModelFile{QStringLiteral("lama_512/lama_512_fp16.onnx"),
                      QString(64, QLatin1Char('d'))}}},
        {}});
    manifest.models.append(AiModelRecord{
        QStringLiteral("lama-512-int8-allowzero0"), QStringLiteral("inpaint"),
        QStringLiteral("1.0.0"), QStringLiteral("Apache-2.0"),
        QStringLiteral("Apache-2.0"), {}, true,
        {{QStringLiteral("model"),
          AiModelFile{QStringLiteral("unused-lama.onnx"),
                      QString(64, QLatin1Char('c'))}}},
        {}});
    return manifest;
}

// Builds a project whose clip references the sample layer and returns the
// cache key the preflight expects.
QString projectWithRemovalLayer(const ModelRegistry &registry, Project *project,
                                RemovalLayer *layer)
{
    *project = projectWithClip();
    *layer = sampleLayer();
    project->removalLayers.append(*layer);
    project->tracks[0].clips[0].removalLayerId = layer->id;
    return computeRemovalCacheKey(RemovalPreflight::keyInputFor(
        *project, project->tracks[0].clips[0], *layer, registry));
}

void writeCompleteCache(const QString &root, const QString &key,
                        const RemovalKeyInput &input)
{
    RemovalCache cache(root);
    constexpr int kTotalFrames = 300;
    RemovalCacheManifest manifest;
    manifest.schema = QStringLiteral("m4.removal.cache.1");
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("clip-1");
    manifest.sourceInUs = input.sourceInUs;
    manifest.sourceOutUs = input.sourceOutUs;
    manifest.width = input.width;
    manifest.height = input.height;
    manifest.fps = input.fps;
    manifest.trackingModelId = input.trackingModelId;
    manifest.inpaintModelId = input.inpaintModelId;
    manifest.pipelineVersion = input.pipelineVersion;
    manifest.status = QStringLiteral("complete");
    manifest.totalFrames = kTotalFrames;

    QString error;
    const int chunkCount = (kTotalFrames + 29) / 30;
    for (int chunk = 0; chunk < chunkCount; ++chunk) {
        QList<int> frames;
        QList<QImage> patches;
        QList<QRect> crops;
        if (chunk == 0) {
            QImage patch(16, 16, QImage::Format_ARGB32);
            patch.fill(QColor(10, 200, 20, 255));
            frames.append(0);
            patches.append(patch);
            crops.append(QRect(0, 0, 16, 16));
        }
        RemovalChunkIndex index;
        ASSERT_TRUE(cache.writeChunkFrames(key, chunk, frames, patches, crops,
                                           &index, &error))
            << error.toStdString();
        const int firstFrame = chunk * 30;
        manifest.chunks.append(RemovalCacheChunk{
            chunk, firstFrame, std::min(30, kTotalFrames - firstFrame),
            index.treeSha256});
    }
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();
    ASSERT_TRUE(cache.verify(key, &error)) << error.toStdString();
}

} // namespace

TEST(RemovalPreflight, MissingAndStaleCacheAreReportedBeforeExport)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const ModelRegistry registry = ModelRegistry::forTest(approvedManifest());

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);
    // sampleLayer carries a made-up cache key; nothing exists on disk.
    RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_EQ(result.issues.first().clipId, QStringLiteral("clip-1"));
    EXPECT_TRUE(result.message.contains(QStringLiteral("缺失或已过期")))
        << result.message.toStdString();
}

TEST(RemovalPreflight, AcceptsCompleteCacheAndRejectsKeyMismatch)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const ModelRegistry registry = ModelRegistry::forTest(approvedManifest());

    Project project;
    RemovalLayer layer;
    QString key = projectWithRemovalLayer(registry, &project, &layer);
    ASSERT_EQ(key.size(), 64);
    const RemovalKeyInput input = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], layer, registry);
    writeCompleteCache(dir.path(), key, input);

    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(result.issues.isEmpty());
    EXPECT_TRUE(result.message.isEmpty());

    // A different key means the generated pixels no longer match the project.
    project.removalLayers[0].cacheKey = QString(64, QLatin1Char('e'));
    result = RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_TRUE(result.issues.first().reason.contains(
        QStringLiteral("已过期"))) << result.issues.first().reason.toStdString();
    EXPECT_TRUE(result.message.contains(QStringLiteral("已过期")))
        << result.message.toStdString();
}

TEST(RemovalPreflight, GateDisabledReportsTheGateReasonNotAnExpiredCache)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());

    ModelManifest manifest = approvedManifest();
    manifest.commercialApproved = false;
    const ModelRegistry registry = ModelRegistry::forTest(manifest);

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);
    const RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_TRUE(result.issues.first().reason.contains(
        QStringLiteral("未启用"))) << result.issues.first().reason.toStdString();
    EXPECT_TRUE(result.message.contains(
        QStringLiteral("AI 消除功能未启用")))
        << result.message.toStdString();
    EXPECT_FALSE(result.message.contains(QStringLiteral("已过期")))
        << result.message.toStdString();
}

TEST(RemovalPreflight, SkipsDisabledTracksClipsAndNonOverlappingLayers)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const ModelRegistry registry = ModelRegistry::forTest(approvedManifest());

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);

    // A disabled track makes the layer unreachable in the evaluator, so an
    // empty cache must not block export.
    project.tracks[0].enabled = false;
    EXPECT_TRUE(RemovalPreflight::check(project, cache, registry).ok);
    EXPECT_TRUE(RemovalPreflight::checkWithoutModels(
                    project, QStringLiteral("未启用"))
                    .ok);

    project.tracks[0].enabled = true;
    project.tracks[0].clips[0].enabled = false;
    EXPECT_TRUE(RemovalPreflight::check(project, cache, registry).ok);
    EXPECT_TRUE(RemovalPreflight::checkWithoutModels(
                    project, QStringLiteral("未启用"))
                    .ok);

    // Enabled again, but the clip was trimmed outside the generated range.
    project.tracks[0].clips[0].enabled = true;
    project.tracks[0].clips[0].sourceInUs = 20'000'000;
    project.tracks[0].clips[0].sourceOutUs = 30'000'000;
    EXPECT_TRUE(RemovalPreflight::check(project, cache, registry).ok)
        << "a layer outside the clip's source range cannot be active";
}

TEST(RemovalPreflight, NullRegistryHonoursDisabledTrackAndClip)
{
    ModelManifest manifest = approvedManifest();
    const ModelRegistry registry = ModelRegistry::forTest(manifest);

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);

    project.tracks[0].enabled = false;
    EXPECT_TRUE(RemovalPreflight::checkWithoutModels(
                    project, QStringLiteral("AI 消除功能未启用。"))
                    .ok);
    project.tracks[0].enabled = true;
    project.tracks[0].clips[0].enabled = false;
    EXPECT_TRUE(RemovalPreflight::checkWithoutModels(
                    project, QStringLiteral("AI 消除功能未启用。"))
                    .ok);
}

TEST(RemovalPreflight, CacheKeyUsesTheModeSpecificInpaintModel)
{
    const ModelRegistry registry = ModelRegistry::forTest(approvedManifest());
    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);

    RemovalLayer fast = layer;
    fast.parameters.mode = RepairMode::Fast;
    const RemovalKeyInput fastInput = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], fast, registry);
    EXPECT_EQ(fastInput.inpaintModelId, QStringLiteral("migan-512"));

    RemovalLayer quality = layer;
    quality.parameters.mode = RepairMode::Quality;
    const RemovalKeyInput qualityInput = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], quality, registry);
    EXPECT_EQ(qualityInput.inpaintModelId,
              QStringLiteral("lama-512-int8-allowzero0"));
    EXPECT_NE(qualityInput.inpaintModelHash, fastInput.inpaintModelHash);
    EXPECT_NE(computeRemovalCacheKey(fastInput),
              computeRemovalCacheKey(qualityInput));
}

TEST(RemovalPreflight, TruncatedCompleteManifestIsReportedAsDamaged)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const ModelRegistry registry = ModelRegistry::forTest(approvedManifest());

    Project project;
    RemovalLayer layer;
    const QString key = projectWithRemovalLayer(registry, &project, &layer);
    const RemovalKeyInput input = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], layer, registry);

    // 300 frames need 10 chunks; list only chunk 0 and call it complete.
    QImage patch(16, 16, QImage::Format_ARGB32);
    patch.fill(QColor(1, 2, 3, 255));
    RemovalChunkIndex index;
    QString error;
    ASSERT_TRUE(cache.writeChunkFrames(key, 0, {0}, {patch},
                                       {QRect(0, 0, 16, 16)}, &index, &error))
        << error.toStdString();
    RemovalCacheManifest manifest;
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("clip-1");
    manifest.sourceInUs = input.sourceInUs;
    manifest.sourceOutUs = input.sourceOutUs;
    manifest.width = input.width;
    manifest.height = input.height;
    manifest.fps = input.fps;
    manifest.status = QStringLiteral("complete");
    manifest.totalFrames = 300;
    manifest.chunks.append(RemovalCacheChunk{0, 0, 30, index.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();
    EXPECT_EQ(cache.state(key, &error), RemovalCacheState::Corrupt);
    EXPECT_FALSE(cache.verify(key, &error));

    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    const RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_TRUE(result.issues.first().reason.contains(
        QStringLiteral("缺失或已损坏")))
        << result.issues.first().reason.toStdString();
}

TEST(RemovalPreflight, QualityWithoutLamaFailsInsteadOfUsingMigan)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());

    ModelManifest manifest = approvedManifest();
    // Remove the int8 allowzero LaMa; only MI-GAN and the fp16 LaMa remain.
    for (int i = manifest.models.size() - 1; i >= 0; --i) {
        if (manifest.models[i].id
            == QStringLiteral("lama-512-int8-allowzero0")) {
            manifest.models.removeAt(i);
        }
    }
    const ModelRegistry registry = ModelRegistry::forTest(manifest);

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);
    project.removalLayers[0].parameters.mode = RepairMode::Quality;

    const RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_TRUE(result.issues.first().reason.contains(
        QStringLiteral("没有可用的修复模型")))
        << result.issues.first().reason.toStdString();
    EXPECT_TRUE(result.message.contains(QStringLiteral("没有可用的修复模型")))
        << result.message.toStdString();
}

TEST(RemovalPreflight, ModelsAlreadyVerifiedSkipsModelFileHashing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString manifestPath =
        dir.filePath(QStringLiteral("models.manifest.json"));
    const auto fileObject = [](const QString &path, QChar hashChar) {
        return QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("sha256"), QString(64, hashChar)}};
    };
    const QJsonObject manifestJson{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("commercialApproved"), true},
        {QStringLiteral("pipelineVersion"), QStringLiteral("m4.0")},
        {QStringLiteral("legalSignOff"),
         QJsonObject{{QStringLiteral("approvedBy"), QStringLiteral("legal")},
                     {QStringLiteral("date"), QStringLiteral("2026-11-15")},
                     {QStringLiteral("scope"), QStringLiteral("commercial")}}},
        {QStringLiteral("models"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("tracker")},
                         {QStringLiteral("family"), QStringLiteral("tracking")},
                         {QStringLiteral("commercialApproved"), true},
                         {QStringLiteral("files"),
                          QJsonObject{{QStringLiteral("image_encoder"),
                                       fileObject(QStringLiteral(
                                                      "missing-tracking.onnx"),
                                                  QLatin1Char('a'))}}}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("migan-512")},
                         {QStringLiteral("family"), QStringLiteral("inpaint")},
                         {QStringLiteral("commercialApproved"), true},
                         {QStringLiteral("files"),
                          QJsonObject{{QStringLiteral("model"),
                                       fileObject(QStringLiteral(
                                                      "missing-migan.onnx"),
                                                  QLatin1Char('b'))}}}}}}};
    QFile manifestFile(manifestPath);
    ASSERT_TRUE(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(
        QJsonDocument(manifestJson).toJson(QJsonDocument::Compact));
    manifestFile.close();

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(*registry, &project, &layer);
    RemovalCache cache(dir.path());

    // Without the flag, the gate re-hashes the missing files and fails before
    // the cache check.
    const RemovalPreflightResult unverified =
        RemovalPreflight::check(project, cache, *registry, false);
    EXPECT_FALSE(unverified.ok);
    EXPECT_FALSE(unverified.issues.isEmpty());
    EXPECT_TRUE(unverified.issues.first().reason.contains(
        QStringLiteral("未启用")))
        << unverified.issues.first().reason.toStdString();

    // With the flag (AiFeatureGate already verified), the cache verdict is
    // reached without touching the model files; sampleLayer has a fake key.
    const RemovalPreflightResult verified =
        RemovalPreflight::check(project, cache, *registry, true);
    EXPECT_FALSE(verified.ok);
    EXPECT_FALSE(verified.issues.isEmpty());
    EXPECT_TRUE(verified.issues.first().reason.contains(
        QStringLiteral("已过期")))
        << verified.issues.first().reason.toStdString();
}

TEST(RemovalPreflight, TechnicalPreviewBypassesApprovalButStillChecksCache)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());

    ModelManifest manifest = approvedManifest();
    manifest.commercialApproved = false;
    manifest.legalSignOffBy.clear();
    const ModelRegistry registry = ModelRegistry::forTest(manifest);

    Project project;
    RemovalLayer layer;
    const QString key = projectWithRemovalLayer(registry, &project, &layer);
    const RemovalKeyInput input = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], layer, registry);
    writeCompleteCache(dir.path(), key, input);
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;

    const RemovalPreflightResult normal =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(normal.ok);
    ASSERT_FALSE(normal.issues.isEmpty());
    EXPECT_TRUE(normal.issues.first().reason.contains(
        QStringLiteral("许可证")))
        << normal.issues.first().reason.toStdString();

    const RemovalPreflightResult technical =
        RemovalPreflight::check(project, cache, registry, true, true);
    EXPECT_TRUE(technical.ok) << technical.message.toStdString();
    EXPECT_TRUE(technical.issues.isEmpty());
}

TEST(RemovalPreflight, TechnicalPreviewVerifiesBundleWhenNotPreviouslyVerified)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());

    ModelManifest manifest = approvedManifest();
    manifest.commercialApproved = false;
    manifest.legalSignOffBy.clear();
    const ModelRegistry registry = ModelRegistry::forTest(manifest);

    Project project;
    RemovalLayer layer;
    const QString key = projectWithRemovalLayer(registry, &project, &layer);
    const RemovalKeyInput input = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], layer, registry);
    writeCompleteCache(dir.path(), key, input);
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;

    // modelsAlreadyVerified=false forces the technical-preview branch to call
    // verifyFiles(); forTest uses in-memory records whose files verify as ok.
    const RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry, false, true);
    EXPECT_TRUE(result.ok) << result.message.toStdString();
}

TEST(RemovalPreflight, TechnicalPreviewStillRequiresTheModeInpaintModel)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());

    ModelManifest manifest = approvedManifest();
    manifest.commercialApproved = false;
    for (int i = manifest.models.size() - 1; i >= 0; --i) {
        if (manifest.models[i].id
            == QStringLiteral("lama-512-int8-allowzero0")) {
            manifest.models.removeAt(i);
        }
    }
    const ModelRegistry registry = ModelRegistry::forTest(manifest);

    Project project;
    RemovalLayer layer;
    projectWithRemovalLayer(registry, &project, &layer);
    project.removalLayers[0].parameters.mode = RepairMode::Quality;

    const RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry, true, true);
    EXPECT_FALSE(result.ok);
    ASSERT_FALSE(result.issues.isEmpty());
    EXPECT_TRUE(result.issues.first().reason.contains(
        QStringLiteral("没有可用的修复模型")))
        << result.issues.first().reason.toStdString();
}

TEST(RemovalPreflight, IncompleteTrackingBlocksButLowConfidenceOnlyWarns)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const ModelRegistry registry = ModelRegistry::forTest(approvedManifest());

    Project project;
    RemovalLayer layer;
    QString key = projectWithRemovalLayer(registry, &project, &layer);
    const RemovalKeyInput input = RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], layer, registry);
    writeCompleteCache(dir.path(), key, input);
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;

    project.removalLayers[0].trackingState = RemovalTrackingState::Tracking;
    RemovalPreflightResult result =
        RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_TRUE(result.issues.first().reason.contains(
        QStringLiteral("跟踪尚未完成")))
        << result.issues.first().reason.toStdString();

    project.removalLayers[0].trackingState = RemovalTrackingState::LowConfidence;
    result = RemovalPreflight::check(project, cache, registry);
    EXPECT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(result.issues.isEmpty());
    EXPECT_TRUE(result.message.contains(QStringLiteral("置信度较低")))
        << result.message.toStdString();
}
