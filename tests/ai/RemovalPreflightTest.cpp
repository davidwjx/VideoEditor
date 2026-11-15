#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
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
    QImage patch(16, 16, QImage::Format_ARGB32);
    patch.fill(QColor(10, 200, 20, 255));
    RemovalChunkIndex index;
    QString error;
    ASSERT_TRUE(cache.writeChunkFrames(key, 0, {0}, {patch},
                                       {QRect(0, 0, 16, 16)}, &index, &error))
        << error.toStdString();

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
    manifest.totalFrames = 300;
    manifest.chunks.append(
        RemovalCacheChunk{0, 0, manifest.totalFrames, index.treeSha256});
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
