#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <d3d11.h>
#include <wrl/client.h>

#include "D3D11Context.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "ExportTestProject.h"
#include "MediaProbe.h"
#include "OfflineFrameRenderer.h"
#include "RemovalCache.h"
#include "TimelineCompositor.h"

using namespace VideoEditor;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

Project redProject(const QString &fixtureDir)
{
    Project project;
    project.timeline.width = kWidth;
    project.timeline.height = kHeight;
    project.timeline.fps = Rational{30, 1};
    Asset asset;
    asset.id = QStringLiteral("red");
    asset.path =
        QDir(fixtureDir).filePath(QStringLiteral("red.mp4"));
    asset.info = *MediaProbe::probe(asset.path);
    project.assets.append(asset);

    Track track;
    track.id = QStringLiteral("track-v");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("c1");
    clip.assetId = asset.id;
    clip.sourceInUs = 0;
    clip.sourceOutUs = 1'000'000;
    clip.timelineStartUs = 0;
    track.clips.append(clip);
    project.tracks.append(track);
    return project;
}

QString writeRepairCache(const QString &root, int totalFrames,
                         const QList<int> &frames)
{
    const QString key(64, QLatin1Char('a'));
    RemovalCache cache(root);
    QList<QImage> patches;
    QList<QRect> crops;
    for (int i = 0; i < frames.size(); ++i) {
        QImage patch(32, 32, QImage::Format_ARGB32);
        patch.fill(QColor(0, 0, 255, 255));
        patches.append(patch);
        crops.append(QRect(16, 16, 32, 32));
    }
    RemovalChunkIndex index;
    QString error;
    EXPECT_TRUE(cache.writeChunkFrames(key, 0, frames, patches, crops, &index,
                                       &error))
        << error.toStdString();

    RemovalCacheManifest manifest;
    manifest.schema = QStringLiteral("m4.removal.cache.1");
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("c1");
    manifest.sourceInUs = 0;
    manifest.sourceOutUs = 1'000'000;
    manifest.width = kWidth;
    manifest.height = kHeight;
    manifest.fps = Rational{30, 1};
    manifest.status = QStringLiteral("complete");
    manifest.totalFrames = totalFrames;
    manifest.chunks.append(
        RemovalCacheChunk{0, 0, totalFrames, index.treeSha256});
    EXPECT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();
    return key;
}

void attachRepairLayer(Project *project, const QString &key)
{
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-1");
    layer.clipId = QStringLiteral("c1");
    layer.sourceInUs = 0;
    layer.sourceOutUs = 1'000'000;
    layer.cacheKey = key;
    layer.trackingState = RemovalTrackingState::Complete;
    project->removalLayers.append(layer);
    project->tracks[0].clips[0].removalLayerId = layer.id;
}

ExportSettings settingsFor(const QString &output)
{
    ExportSettings settings;
    settings.codec = ExportVideoCodec::Mpeg4Part2;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 2000;
    settings.outputPath = output;
    settings.segmentTargetUs = 100'000;
    return settings;
}

std::vector<uint8_t> composePreview(TimelineCompositor &compositor,
                                    int64_t timelineUs, QString *error)
{
    D3D11Context *context = compositor.context();
    ID3D11Device *device = context->device();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target))
        || FAILED(
            device->CreateRenderTargetView(target.Get(), nullptr, &rtv))) {
        if (error)
            *error = QStringLiteral("无法创建合成目标。");
        return {};
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        if (error)
            *error = QStringLiteral("无法创建回读纹理。");
        return {};
    }

    compositor.drawFrame(timelineUs, rtv.Get(), true, error);
    if (error && !error->isEmpty())
        return {};
    context->context()->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0,
                                       &mapped))) {
        if (error)
            *error = QStringLiteral("无法映射回读纹理。");
        return {};
    }
    std::vector<uint8_t> rgba(size_t(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        std::memcpy(rgba.data() + size_t(y) * kWidth * 4,
                    static_cast<const uint8_t *>(mapped.pData)
                        + size_t(y) * mapped.RowPitch,
                    size_t(kWidth) * 4);
    }
    context->context()->Unmap(staging.Get(), 0);
    return rgba;
}

} // namespace

TEST(RemovalExport, FailsBeforeWritingOutputWhenCacheMissing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    attachRepairLayer(&project, QString(64, QLatin1Char('f')));

    const QString output = dir.filePath(QStringLiteral("missing.mp4"));
    ExportPipeline::Hooks hooks;
    hooks.removalPreflight = [](const Project &) {
        RemovalPreflightResult result;
        result.ok = false;
        result.message = QStringLiteral(
            "片段“c1”的 AI 修复结果缺失或已过期。请先生成修复，再导出。");
        return result;
    };
    const ExportResult result = runExportForTest(
        project, settingsFor(output), EncoderCatalog::withoutHardwareForTest(),
        false, hooks);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("缺失或已过期")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(output));
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(output)));
}

TEST(RemovalExport, PreviewAndExportMatchWithFixedCache)
{
    QString error;
    if (!d3d11Available(&error))
        GTEST_SKIP() << error.toStdString();
    const QString red =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("red.mp4"));
    if (!QFileInfo::exists(red))
        GTEST_SKIP() << "missing red.mp4";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    attachRepairLayer(&project, writeRepairCache(dir.path(), 30, {0}));

    // Preview path: the same TimelineCompositor the render worker uses,
    // drawing into a test-owned target.
    TimelineCompositor preview;
    preview.setRemovalCacheRoot(dir.path());
    ASSERT_TRUE(preview.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();
    const std::vector<uint8_t> previewFrame = composePreview(preview, 0, &error);
    ASSERT_FALSE(previewFrame.empty()) << error.toStdString();

    // Export path: OfflineFrameRenderer reads back the same composition.
    OfflineFrameRenderer renderer;
    renderer.setRemovalCacheRoot(dir.path());
    ASSERT_TRUE(renderer.open(project, kWidth, kHeight, &error))
        << error.toStdString();
    std::vector<uint8_t> exportFrame;
    ASSERT_TRUE(renderer.renderFrame(0, &exportFrame, &error))
        << error.toStdString();
    ASSERT_EQ(exportFrame.size(), previewFrame.size());

    int maxDelta = 0;
    for (size_t i = 0; i < previewFrame.size(); ++i) {
        maxDelta = std::max(
            maxDelta, std::abs(int(previewFrame[i]) - int(exportFrame[i])));
    }
    EXPECT_LE(maxDelta, 1) << "preview and export must compose the same pixels";
    const size_t centre = size_t(32 * kWidth + 32) * 4;
    EXPECT_GT(exportFrame[centre + 2], 150); // blue repair patch
}

TEST(RemovalExport, MissingCacheMidExportAbortsWithoutOriginalPixels)
{
    QString error;
    if (!d3d11Available(&error))
        GTEST_SKIP() << error.toStdString();
    const QString red =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("red.mp4"));
    if (!QFileInfo::exists(red))
        GTEST_SKIP() << "missing red.mp4";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    QList<int> allFrames;
    for (int i = 0; i < 30; ++i)
        allFrames.append(i);
    // Keep the cache in its own subtree: the export parts directory must
    // survive the simulated cache deletion.
    const QString cacheRoot = dir.filePath(QStringLiteral("cache"));
    ASSERT_TRUE(QDir().mkpath(cacheRoot));
    const QString key = writeRepairCache(cacheRoot, 30, allFrames);
    attachRepairLayer(&project, key);

    const QString output = dir.filePath(QStringLiteral("abort.mp4"));
    bool deleted = false;
    ExportPipeline::Hooks hooks;
    hooks.removalCacheRoot = cacheRoot;
    hooks.onProgress = [&](const ExportProgress &progress) {
        if (deleted || progress.stage != ExportStage::Rendering
            || progress.completedFrames < 1) {
            return;
        }
        // Simulate the cache disappearing after the first segment: the next
        // frame must fail instead of silently encoding the original.
        deleted = QDir(cacheRoot).removeRecursively();
    };
    const ExportResult result = runExportForTest(
        project, settingsFor(output),
        detectEncodersForTest(kWidth, kHeight, Rational{30, 1}), false, hooks);

    EXPECT_TRUE(deleted) << "test never reached the rendering stage";
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("AI 修复缓存")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(output))
        << "a failed export must never assemble original pixels";
    // Completed work stays resumable.
    const QString partsDir = partsDirectoryFor(output);
    EXPECT_TRUE(QFileInfo::exists(
        QDir(partsDir).filePath(QStringLiteral("checkpoint.json"))));
}
