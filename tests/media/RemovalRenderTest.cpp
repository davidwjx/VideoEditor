#include <gtest/gtest.h>

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
#include "MediaProbe.h"
#include "Project.h"
#include "RemovalCache.h"
#include "TimelineCompositor.h"

using namespace VideoEditor;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

Project redProject(const QString &fixtureDir)
{
    const QString path =
        QDir(fixtureDir).filePath(QStringLiteral("red.mp4"));
    Project project;
    project.timeline.width = kWidth;
    project.timeline.height = kHeight;
    project.timeline.fps = Rational{30, 1};
    const auto probed = MediaProbe::probe(path);
    if (!probed)
        return project; // no assets: callers skip or fail explicitly
    Asset asset;
    asset.id = QStringLiteral("asset-1");
    asset.path = path;
    asset.info = *probed;
    project.assets.append(asset);
    Track track;
    track.id = QStringLiteral("track-v");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("c1");
    clip.assetId = asset.id;
    clip.sourceInUs = 0;
    clip.sourceOutUs = asset.info.durationUs;
    clip.timelineStartUs = 0;
    track.clips.append(clip);
    project.tracks.append(track);
    return project;
}

// Adds a repair layer covering [sourceInUs, sourceOutUs) and writes one patch
// at the requested global frame index.
void addRepairLayer(Project *project, const QString &cacheRoot,
                    int64_t sourceInUs, int64_t sourceOutUs, int frame,
                    int totalFrames, const QColor &color,
                    const QRect &crop = QRect(0, 0, 64, 64))
{
    const QString key(64, QLatin1Char('a'));
    RemovalCache cache(cacheRoot);
    RemovalChunkIndex index;
    QImage patch(crop.size(), QImage::Format_ARGB32);
    patch.fill(color);
    QString error;
    EXPECT_TRUE(cache.writeChunkFrames(key, frame / 30, {frame}, {patch},
                                       {crop}, &index, &error))
        << error.toStdString();

    RemovalCacheManifest manifest;
    manifest.schema = QStringLiteral("m4.removal.cache.1");
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("c1");
    manifest.sourceInUs = sourceInUs;
    manifest.sourceOutUs = sourceOutUs;
    manifest.width = kWidth;
    manifest.height = kHeight;
    manifest.fps = Rational{30, 1};
    manifest.status = QStringLiteral("complete");
    manifest.totalFrames = totalFrames;
    manifest.chunks.append(
        RemovalCacheChunk{frame / 30, frame / 30 * 30, totalFrames,
                          index.treeSha256});
    EXPECT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();

    RemovalLayer layer;
    layer.id = QStringLiteral("rl-1");
    layer.clipId = QStringLiteral("c1");
    layer.sourceInUs = sourceInUs;
    layer.sourceOutUs = sourceOutUs;
    layer.cacheKey = key;
    layer.trackingState = RemovalTrackingState::Complete;
    project->removalLayers.append(layer);
    project->tracks[0].clips[0].removalLayerId = layer.id;
}

std::vector<uint8_t> compose(TimelineCompositor &compositor, int64_t timelineUs,
                             QString *error, int *drawn = nullptr)
{
    D3D11Context *context = compositor.context();
    if (!context) {
        if (error)
            *error = QStringLiteral("合成器没有 D3D11 设备。");
        return {};
    }
    ID3D11Device *device = context->device();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target))
        || FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv))) {
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

    const int layers = compositor.drawFrame(timelineUs, rtv.Get(), true, error);
    if (drawn)
        *drawn = layers;
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

std::array<int, 3> pixel(const std::vector<uint8_t> &rgba, int x, int y)
{
    const size_t i = size_t(y * kWidth + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2]};
}

bool isBlue(const std::array<int, 3> &p)
{
    return p[2] > 150 && p[0] < 100 && p[1] < 100;
}

bool isRed(const std::array<int, 3> &p)
{
    return p[0] > 150 && p[2] < 100 && p[1] < 100;
}

} // namespace

class RemovalRenderTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        QString error;
        if (!D3D11Context::create(true, &error)
            && !D3D11Context::create(false, &error)) {
            GTEST_SKIP() << error.toStdString();
        }
        const QString red =
            QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                .filePath(QStringLiteral("red.mp4"));
        if (!QFileInfo::exists(red))
            GTEST_SKIP() << "missing red.mp4";
        if (!MediaProbe::probe(red))
            GTEST_SKIP() << "red.mp4 could not be probed";
    }
};

TEST_F(RemovalRenderTest, DrawsRepairOverOriginalAndCompareModesDiffer)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    // A solid blue patch covering the whole frame makes the comparison mode
    // unambiguous against the red source.
    addRepairLayer(&project, dir.path(), 0, 1'000'000, 0, 30, QColor(0, 0, 255));

    TimelineCompositor compositor;
    compositor.setRemovalCacheRoot(dir.path());
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    compositor.setRemovalCompareMode(RemovalCompareMode::Repair);
    int drawn = 0;
    const auto repair = compose(compositor, 0, &error, &drawn);
    ASSERT_FALSE(repair.empty()) << error.toStdString();
    EXPECT_TRUE(isBlue(pixel(repair, 32, 32)));
    EXPECT_EQ(drawn, 2); // original + patch
    EXPECT_EQ(compositor.removalLayerCountForTest(), 1);

    compositor.setRemovalCompareMode(RemovalCompareMode::Original);
    const auto original = compose(compositor, 0, &error, &drawn);
    ASSERT_FALSE(original.empty()) << error.toStdString();
    EXPECT_TRUE(isRed(pixel(original, 32, 32)));
    EXPECT_EQ(drawn, 1);

    // Split wipe repair draws only into the left fraction; the right half is
    // the untouched original.
    compositor.setRemovalCompareMode(RemovalCompareMode::SplitWipe, 500);
    const auto split = compose(compositor, 0, &error, &drawn);
    ASSERT_FALSE(split.empty()) << error.toStdString();
    EXPECT_TRUE(isBlue(pixel(split, 16, 32)))
        << "left half must show the repair patch";
    EXPECT_TRUE(isRed(pixel(split, 48, 32)))
        << "right half must show the original";
}

TEST_F(RemovalRenderTest, SplitWipeHonoursThePermilleFraction)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    addRepairLayer(&project, dir.path(), 0, 1'000'000, 0, 30, QColor(0, 0, 255));

    TimelineCompositor compositor;
    compositor.setRemovalCacheRoot(dir.path());
    compositor.setRemovalCompareMode(RemovalCompareMode::SplitWipe, 250);
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    const auto frame = compose(compositor, 0, &error);
    ASSERT_FALSE(frame.empty()) << error.toStdString();
    EXPECT_TRUE(isBlue(pixel(frame, 8, 32)));
    EXPECT_TRUE(isRed(pixel(frame, 32, 32)));
}

TEST_F(RemovalRenderTest, OutsideLayerRangeDrawsOriginal)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    // Layer covers only 0.5 s..1.0 s of the source.
    addRepairLayer(&project, dir.path(), 500'000, 1'000'000, 7, 15,
                   QColor(0, 0, 255));

    TimelineCompositor compositor;
    compositor.setRemovalCacheRoot(dir.path());
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    const auto before = compose(compositor, 250'000, &error);
    ASSERT_FALSE(before.empty()) << error.toStdString();
    EXPECT_TRUE(isRed(pixel(before, 32, 32)));

    const auto inside = compose(compositor, 750'000, &error);
    ASSERT_FALSE(inside.empty()) << error.toStdString();
    EXPECT_TRUE(isBlue(pixel(inside, 32, 32)));

    // Within the layer range but a frame with no hole (sparse cache) keeps the
    // original rather than failing or stretching the neighbouring patch.
    const auto sparse = compose(compositor, 900'000, &error);
    ASSERT_FALSE(sparse.empty()) << error.toStdString();
    EXPECT_TRUE(isRed(pixel(sparse, 32, 32)));
}

TEST_F(RemovalRenderTest, MissingCacheFallsBackToOriginalAndWarnsOnce)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-missing");
    layer.clipId = QStringLiteral("c1");
    layer.sourceInUs = 0;
    layer.sourceOutUs = 1'000'000;
    layer.cacheKey = QString(64, QLatin1Char('f')); // nothing on disk
    layer.trackingState = RemovalTrackingState::Complete;
    project.removalLayers.append(layer);
    project.tracks[0].clips[0].removalLayerId = layer.id;

    TimelineCompositor compositor;
    compositor.setRemovalCacheRoot(dir.path());
    int warnings = 0;
    compositor.setWarningCallback(
        [&warnings](const QString &) { ++warnings; });
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    const auto first = compose(compositor, 0, &error);
    ASSERT_FALSE(first.empty()) << error.toStdString();
    EXPECT_TRUE(isRed(pixel(first, 32, 32)));
    const auto second = compose(compositor, 0, &error);
    ASSERT_FALSE(second.empty()) << error.toStdString();
    EXPECT_EQ(warnings, 1) << "the missing cache must warn once per layer";
    // The failed key is cached: playback must not re-parse the manifest or
    // every chunk index on each frame.
    EXPECT_EQ(compositor.failedRepairKeyCountForTest(), 1);
}

TEST_F(RemovalRenderTest, EmptyCacheRootWarnsOnceAndCachesTheFailure)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-no-root");
    layer.clipId = QStringLiteral("c1");
    layer.sourceInUs = 0;
    layer.sourceOutUs = 1'000'000;
    layer.cacheKey = QString(64, QLatin1Char('a'));
    layer.trackingState = RemovalTrackingState::Complete;
    project.removalLayers.append(layer);
    project.tracks[0].clips[0].removalLayerId = layer.id;

    TimelineCompositor compositor;
    int warnings = 0;
    compositor.setWarningCallback(
        [&warnings](const QString &) { ++warnings; });
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    ASSERT_FALSE(compose(compositor, 0, &error).empty());
    ASSERT_FALSE(compose(compositor, 0, &error).empty());
    EXPECT_EQ(warnings, 1);
    EXPECT_EQ(compositor.failedRepairKeyCountForTest(), 1);
}

TEST_F(RemovalRenderTest, SplitWipeExtremesShowOnlyOneSide)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    addRepairLayer(&project, dir.path(), 0, 1'000'000, 0, 30, QColor(0, 0, 255));

    TimelineCompositor compositor;
    compositor.setRemovalCacheRoot(dir.path());
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    compositor.setRemovalCompareMode(RemovalCompareMode::SplitWipe, 0);
    const auto none = compose(compositor, 0, &error);
    ASSERT_FALSE(none.empty()) << error.toStdString();
    EXPECT_TRUE(isRed(pixel(none, 2, 32)));
    EXPECT_TRUE(isRed(pixel(none, 32, 32)));

    compositor.setRemovalCompareMode(RemovalCompareMode::SplitWipe, 1000);
    const auto all = compose(compositor, 0, &error);
    ASSERT_FALSE(all.empty()) << error.toStdString();
    EXPECT_TRUE(isBlue(pixel(all, 2, 32)));
    EXPECT_TRUE(isBlue(pixel(all, 32, 32)));
}

TEST_F(RemovalRenderTest, FailOnMissingRemovalReturnsError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Project project = redProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ASSERT_EQ(project.assets.size(), 1) << "red.mp4 probe failed";
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-missing");
    layer.clipId = QStringLiteral("c1");
    layer.sourceInUs = 0;
    layer.sourceOutUs = 1'000'000;
    layer.cacheKey = QString(64, QLatin1Char('f'));
    layer.trackingState = RemovalTrackingState::Complete;
    project.removalLayers.append(layer);
    project.tracks[0].clips[0].removalLayerId = layer.id;

    TimelineCompositor compositor;
    compositor.setRemovalCacheRoot(dir.path());
    compositor.setFailOnMissingRemoval(true);
    QString error;
    ASSERT_TRUE(compositor.open(project, kWidth, kHeight, false, &error))
        << error.toStdString();

    const auto frame = compose(compositor, 0, &error);
    EXPECT_TRUE(frame.empty());
    error.clear();
    // compose() cannot read a failed frame; call drawFrame directly with a
    // real target to assert the -1 + error contract.
    D3D11Context *context = compositor.context();
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
    ASSERT_TRUE(SUCCEEDED(context->device()->CreateTexture2D(&desc, nullptr,
                                                             &target)));
    ASSERT_TRUE(SUCCEEDED(
        context->device()->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    EXPECT_EQ(compositor.drawFrame(0, rtv.Get(), true, &error), -1);
    EXPECT_TRUE(error.contains(QStringLiteral("缓存")))
        << error.toStdString();
}
