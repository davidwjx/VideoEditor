#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <QFileInfo>
#include <QString>

#include <d3d11.h>
#include <wrl/client.h>

#include "ClipVideoSource.h"
#include "D3D11Context.h"
#include "MediaProbe.h"
#include "Project.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QStringLiteral(VIDEOEDITOR_FIXTURE_DIR "/") + name;
}

std::unique_ptr<D3D11Context> createContext(QString *error)
{
    // Prefer a video-capable device (hardware decode); fall back to a plain
    // device so software decoding still exercises the same normalization.
    auto context = D3D11Context::create(true, error);
    if (!context)
        context = D3D11Context::create(false, error);
    return context;
}

Asset assetFor(const QString &path)
{
    Asset asset;
    asset.id = QStringLiteral("asset");
    asset.path = path;
    const auto info = MediaProbe::probe(path);
    if (info)
        asset.info = *info;
    return asset;
}

// Reads back a regular (non-shared) texture; ClipVideoSource's normalized
// texture is not a shared/keyed resource.
std::vector<uint8_t> readbackTexture(D3D11Context &context,
                                     ID3D11Texture2D *texture, QString *error)
{
    if (!texture) {
        if (error)
            *error = QStringLiteral("纹理为空。");
        return {};
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(context.device()->CreateTexture2D(&desc, nullptr, &staging))) {
        if (error)
            *error = QStringLiteral("无法创建回读纹理。");
        return {};
    }
    context.context()->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0,
                                      &mapped))) {
        if (error)
            *error = QStringLiteral("无法映射回读纹理。");
        return {};
    }
    std::vector<uint8_t> result(size_t(desc.Width) * desc.Height * 4);
    for (UINT row = 0; row < desc.Height; ++row) {
        std::memcpy(result.data() + size_t(row) * desc.Width * 4,
                    static_cast<const uint8_t *>(mapped.pData)
                        + size_t(row) * mapped.RowPitch,
                    size_t(desc.Width) * 4);
    }
    context.context()->Unmap(staging.Get(), 0);
    return result;
}

bool hasVisiblePixels(const std::vector<uint8_t> &rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i] > 8 || rgba[i + 1] > 8 || rgba[i + 2] > 8)
            return true;
    }
    return false;
}

} // namespace

TEST(ClipVideoSource, NormalizesDecodedFrameToRgbaTexture)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const QString path = fixture(QStringLiteral("h264.mp4"));
    ASSERT_TRUE(QFileInfo::exists(path));
    const Asset asset = assetFor(path);
    ASSERT_TRUE(asset.info.supported);

    auto source = ClipVideoSource::open(asset, *context, QString(), &error);
    ASSERT_TRUE(source) << error.toStdString();
    EXPECT_GT(source->width(), 0);
    EXPECT_GT(source->height(), 0);

    const DecodedFrame *frame = source->frameAt(0, &error);
    ASSERT_NE(frame, nullptr) << error.toStdString();
    // Hardware NV12 and software RGBA both normalize to one RGBA texture.
    EXPECT_EQ(frame->kind, DecodedFrame::Kind::HardwareTexture);
    EXPECT_NE(frame->texture, nullptr);
    EXPECT_EQ(frame->subresource, 0u);
    EXPECT_EQ(frame->width, source->width());
    EXPECT_EQ(frame->height, source->height());

    const std::vector<uint8_t> rgba =
        readbackTexture(*context, frame->texture, &error);
    ASSERT_EQ(rgba.size(),
              size_t(source->width()) * size_t(source->height()) * 4)
        << error.toStdString();
    EXPECT_TRUE(hasVisiblePixels(rgba));
}

TEST(ClipVideoSource, ReusesFrameInsideItsDisplayWindow)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const Asset asset = assetFor(fixture(QStringLiteral("h264.mp4")));
    auto source = ClipVideoSource::open(asset, *context, QString(), &error);
    ASSERT_TRUE(source) << error.toStdString();

    const DecodedFrame *first = source->frameAt(0, &error);
    ASSERT_NE(first, nullptr) << error.toStdString();
    // 1 ms later is still inside the same ~33 ms frame window: no decode.
    const DecodedFrame *second = source->frameAt(1'000, &error);
    ASSERT_NE(second, nullptr) << error.toStdString();
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->ptsUs, second->ptsUs);
}

TEST(ClipVideoSource, ReturnsLastFrameCleanlyAtEndOfStream)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const Asset asset = assetFor(fixture(QStringLiteral("h264.mp4")));
    auto source = ClipVideoSource::open(asset, *context, QString(), &error);
    ASSERT_TRUE(source) << error.toStdString();

    const int64_t beyond = asset.info.durationUs + 2'000'000;
    const DecodedFrame *frame = source->frameAt(beyond, &error);
    ASSERT_NE(frame, nullptr) << error.toStdString();
    EXPECT_EQ(frame->width, source->width());
    EXPECT_EQ(frame->height, source->height());
}

TEST(ClipVideoSource, AppliesDisplayRotationToDecodedDimensions)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const Asset asset = assetFor(fixture(QStringLiteral("rotated.mp4")));
    if (!asset.info.supported || asset.info.rotationDegrees != 90)
        GTEST_SKIP() << "rotated fixture unavailable";

    auto source = ClipVideoSource::open(asset, *context, QString(), &error);
    ASSERT_TRUE(source) << error.toStdString();
    EXPECT_EQ(source->width(), asset.info.height);
    EXPECT_EQ(source->height(), asset.info.width);

    const DecodedFrame *frame = source->frameAt(0, &error);
    ASSERT_NE(frame, nullptr) << error.toStdString();
    EXPECT_EQ(frame->width, source->width());
    EXPECT_EQ(frame->height, source->height());
}

TEST(ClipVideoSource, UsesDecodedProxyDimensionsNotMediaInfo)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const QString proxyPath = fixture(QStringLiteral("h264.mp4"));
    const auto actual = MediaProbe::probe(proxyPath);
    ASSERT_TRUE(actual.has_value());
    ASSERT_TRUE(actual->supported);

    Asset asset;
    asset.id = QStringLiteral("proxy-asset");
    asset.path = fixture(QStringLiteral("missing-source.mp4"));
    asset.info = *actual;
    // Pretend the source is 4K rotated; the proxy is already upright 540p-ish.
    asset.info.width = 4096;
    asset.info.height = 2160;
    asset.info.rotationDegrees = 90;

    auto source =
        ClipVideoSource::open(asset, *context, proxyPath, &error);
    ASSERT_TRUE(source) << error.toStdString();
    // Dimensions come from the decoded proxy frame, with no rotation applied.
    EXPECT_EQ(source->width(), actual->width);
    EXPECT_EQ(source->height(), actual->height);
}

TEST(ClipVideoSource, UsesOriginalHighresSourceWhenProxyDisabled)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const QString highres = fixture(QStringLiteral("highres.mp4"));
    if (!QFileInfo::exists(highres))
        GTEST_SKIP() << "missing highres fixture";
    const Asset source = assetFor(highres);
    ASSERT_TRUE(source.info.supported);
    ASSERT_EQ(source.info.width, 2560);
    ASSERT_EQ(source.info.height, 1440);

    // Pretend a cached 640x360 proxy exists (h264.mp4 has those dimensions).
    Asset asset = source;
    asset.proxyPath = fixture(QStringLiteral("h264.mp4"));

    // Proxy enabled: decode the small proxy.
    EXPECT_EQ(activeProxyPath(asset, true), asset.proxyPath);
    auto proxy = ClipVideoSource::open(asset, *context,
                                       activeProxyPath(asset, true), &error);
    ASSERT_TRUE(proxy) << error.toStdString();
    EXPECT_EQ(proxy->width(), 640);
    EXPECT_EQ(proxy->height(), 360);

    // Proxy disabled: the render worker passes an empty path, so the original
    // 2560x1440 source is decoded instead of the cached proxy.
    EXPECT_TRUE(activeProxyPath(asset, false).isEmpty());
    auto original = ClipVideoSource::open(
        asset, *context, activeProxyPath(asset, false), &error);
    ASSERT_TRUE(original) << error.toStdString();
    EXPECT_EQ(original->width(), 2560);
    EXPECT_EQ(original->height(), 1440);
}

TEST(ClipVideoSource, AcceptsSharedConverters)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    VideoConverters converters;
    ASSERT_TRUE(converters.initialize(*context, &error)) << error.toStdString();
    EXPECT_TRUE(converters.isInitialized());

    const Asset first = assetFor(fixture(QStringLiteral("h264.mp4")));
    const Asset second = assetFor(fixture(QStringLiteral("mpeg4.mp4")));
    auto a = ClipVideoSource::open(first, *context, QString(), &converters,
                                   &error);
    ASSERT_TRUE(a) << error.toStdString();
    auto b = ClipVideoSource::open(second, *context, QString(), &converters,
                                   &error);
    ASSERT_TRUE(b) << error.toStdString();
    EXPECT_NE(a->frameAt(0, &error), nullptr);
    EXPECT_NE(b->frameAt(0, &error), nullptr);
}
