#include <gtest/gtest.h>

#include <array>
#include <vector>

#include <QImage>

#include "ClipVideoSource.h"
#include "D3D11Context.h"
#include "FrameReadback.h"
#include "LayerCompositor.h"
#include "MediaProbe.h"
#include "Nv12Converter.h"
#include "Project.h"
#include "SharedTextureRing.h"

using namespace VideoEditor;
using Microsoft::WRL::ComPtr;

namespace {

// Convincing NV12 planes (gray luma, neutral chroma) are enough: the test only
// cares that the converter actually renders and changes the immediate context
// state between compositor begin() and drawImage().
bool makeNv12Planes(ID3D11Device *device, ID3D11Texture2D **luma,
                    ID3D11Texture2D **chroma)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<uint8_t> yPlane(64 * 64, 63);
    D3D11_SUBRESOURCE_DATA yData{};
    yData.pSysMem = yPlane.data();
    yData.SysMemPitch = 64;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(device->CreateTexture2D(&desc, &yData, luma)))
        return false;

    std::vector<uint8_t> uvPlane(64 * 64 * 2, 0);
    for (size_t i = 0; i < uvPlane.size(); i += 2) {
        uvPlane[i] = 102;
        uvPlane[i + 1] = 240;
    }
    D3D11_SUBRESOURCE_DATA uvData{};
    uvData.pSysMem = uvPlane.data();
    uvData.SysMemPitch = 128;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    return SUCCEEDED(device->CreateTexture2D(&desc, &uvData, chroma));
}

} // namespace

TEST(Compositor, BlendsTwoImagesAndReadsBack)
{
    QString error;
    auto context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    LayerCompositor compositor;
    ASSERT_TRUE(compositor.initialize(*context, 64, 64, &error)) << error.toStdString();

    QImage red(64, 64, QImage::Format_ARGB32_Premultiplied);
    red.fill(Qt::red);
    QImage blue(64, 64, QImage::Format_ARGB32_Premultiplied);
    blue.fill(QColor(0, 0, 255, 128));

    // Keyed mutex initial state is key 0 (producer's turn). The producer must
    // release with key 1 before a consumer (readback) can acquire the slot,
    // exactly like TimelineRenderWorker does around the compositor calls.
    IDXGIKeyedMutex *mutex = ring.mutex(0);
    ASSERT_NE(mutex, nullptr);
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);

    compositor.begin(ring.rtv(0), true);
    ASSERT_TRUE(compositor.drawImage(red, LayerRect{0, 0, 1, 1}, 1.0f, &error))
        << error.toStdString();
    ASSERT_TRUE(compositor.drawImage(blue, LayerRect{0, 0, 0.5f, 1}, 0.5f, &error))
        << error.toStdString();
    compositor.end();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> pixels =
        readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(pixels.size(), size_t(64 * 64 * 4)) << error.toStdString();
    const auto at = [&pixels](int x, int y) {
        const size_t index = (size_t(y) * 64 + x) * 4;
        return std::array<int, 3>{pixels[index], pixels[index + 1], pixels[index + 2]};
    };
    const auto left = at(8, 32);
    const auto right = at(56, 32);
    EXPECT_GT(left[0], 100);       // red + blue blend
    EXPECT_GT(left[2], 40);
    EXPECT_GT(right[0], 200);      // untouched red
    EXPECT_LT(right[2], 60);
}

// Regression: Nv12Converter/RgbaConverter unbind the render target and change
// the viewport/shader state. A conversion that happens between begin() and
// drawImage() must not make the compositor drop its own draw.
TEST(Compositor, DrawsAfterConverterChangesContextState)
{
    QString error;
    auto context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    ID3D11Device *device = context->device();
    ComPtr<ID3D11Texture2D> luma;
    ComPtr<ID3D11Texture2D> chroma;
    ASSERT_TRUE(makeNv12Planes(device, &luma, &chroma));

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    LayerCompositor compositor;
    ASSERT_TRUE(compositor.initialize(*context, 64, 64, &error))
        << error.toStdString();

    // A separate conversion target, like ClipVideoSource's normalized texture.
    D3D11_TEXTURE2D_DESC targetDesc{};
    targetDesc.Width = 64;
    targetDesc.Height = 64;
    targetDesc.MipLevels = 1;
    targetDesc.ArraySize = 1;
    targetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    targetDesc.SampleDesc.Count = 1;
    targetDesc.Usage = D3D11_USAGE_DEFAULT;
    targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET |
                           D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> converted;
    ASSERT_TRUE(SUCCEEDED(
        device->CreateTexture2D(&targetDesc, nullptr, &converted)));
    ComPtr<ID3D11RenderTargetView> convertedRtv;
    ASSERT_TRUE(SUCCEEDED(
        device->CreateRenderTargetView(converted.Get(), nullptr, &convertedRtv)));

    Nv12Converter converter;
    ASSERT_TRUE(converter.initialize(*context, &error)) << error.toStdString();

    IDXGIKeyedMutex *mutex = ring.mutex(0);
    ASSERT_NE(mutex, nullptr);
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);

    compositor.begin(ring.rtv(0), true);
    // Real conversion in the middle of the compositor pass: this rebinds the
    // render target to convertedRtv and then leaves no target bound.
    ASSERT_TRUE(converter.convert(luma.Get(), chroma.Get(), 64, 64,
                                  convertedRtv.Get(), 0.0f, &error))
        << error.toStdString();

    QImage red(64, 64, QImage::Format_ARGB32_Premultiplied);
    red.fill(Qt::red);
    ASSERT_TRUE(compositor.drawImage(red, LayerRect{0, 0, 1, 1}, 1.0f, &error))
        << error.toStdString();
    compositor.end();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> pixels =
        readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(pixels.size(), size_t(64 * 64 * 4)) << error.toStdString();
    const size_t center = (size_t(32) * 64 + 32) * 4;
    EXPECT_GT(pixels[center], 200);     // red survived the conversion
    EXPECT_LT(pixels[center + 1], 60);
    EXPECT_LT(pixels[center + 2], 60);
}

TEST(Compositor, ReusesUploadedImageUntilCacheKeyChanges)
{
    QString error;
    auto context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    LayerCompositor compositor;
    ASSERT_TRUE(compositor.initialize(*context, 64, 64, &error))
        << error.toStdString();

    QImage red(64, 64, QImage::Format_ARGB32_Premultiplied);
    red.fill(Qt::red);
    QImage blue(64, 64, QImage::Format_ARGB32_Premultiplied);
    blue.fill(Qt::blue);

    const auto drawAndRead = [&](const QString &key, const QImage &image,
                                 std::array<int, 3> *rgb) {
        IDXGIKeyedMutex *mutex = ring.mutex(0);
        EXPECT_NE(mutex, nullptr);
        EXPECT_EQ(mutex->AcquireSync(0, 5000), S_OK);
        compositor.begin(ring.rtv(0), true);
        EXPECT_TRUE(compositor.drawImageCached(key, image,
                                               LayerRect{0, 0, 1, 1}, 1.0f,
                                               &error))
            << error.toStdString();
        compositor.end();
        mutex->ReleaseSync(1);

        const std::vector<uint8_t> pixels =
            readbackSharedTexture(*context, ring.texture(0), &error);
        EXPECT_EQ(pixels.size(), size_t(64 * 64 * 4)) << error.toStdString();
        if (pixels.size() == size_t(64 * 64 * 4)) {
            const size_t center = (size_t(32) * 64 + 32) * 4;
            *rgb = {pixels[center], pixels[center + 1], pixels[center + 2]};
        }
    };

    std::array<int, 3> rgb{};
    drawAndRead(QStringLiteral("subtitle-a"), red, &rgb);
    EXPECT_GT(rgb[0], 200);

    // Same key: the blue image must be ignored and the red texture reused.
    drawAndRead(QStringLiteral("subtitle-a"), blue, &rgb);
    EXPECT_GT(rgb[0], 200);
    EXPECT_LT(rgb[2], 60);

    // New key: the changed image is uploaded.
    drawAndRead(QStringLiteral("subtitle-b"), blue, &rgb);
    EXPECT_GT(rgb[2], 200);
    EXPECT_LT(rgb[0], 60);
}

// End-to-end version of the converter-state regression: decode and normalize a
// real clip frame inside the compositor pass, then draw it and read the ring.
TEST(Compositor, DrawsNormalizedClipFrameConvertedInsidePass)
{
    QString error;
    auto context = D3D11Context::create(true, &error);
    if (!context)
        context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    const QString path =
        QStringLiteral(VIDEOEDITOR_FIXTURE_DIR "/h264.mp4");
    const auto info = MediaProbe::probe(path);
    ASSERT_TRUE(info.has_value());
    ASSERT_TRUE(info->supported);

    Asset asset;
    asset.id = QStringLiteral("clip");
    asset.path = path;
    asset.info = *info;

    auto source = ClipVideoSource::open(asset, *context, QString(), &error);
    ASSERT_TRUE(source) << error.toStdString();
    ASSERT_GT(source->width(), 0);
    ASSERT_GT(source->height(), 0);

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(source->width(), source->height(), 3, &error))
        << error.toStdString();
    LayerCompositor compositor;
    ASSERT_TRUE(compositor.initialize(*context, source->width(),
                                      source->height(), &error))
        << error.toStdString();

    IDXGIKeyedMutex *mutex = ring.mutex(0);
    ASSERT_NE(mutex, nullptr);
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);

    compositor.begin(ring.rtv(0), true);
    // One second in: the source seeks, decodes and converts inside the pass.
    const DecodedFrame *frame = source->frameAt(1'000'000, &error);
    ASSERT_NE(frame, nullptr) << error.toStdString();
    ASSERT_TRUE(compositor.drawFrame(*frame, LayerRect{0, 0, 1, 1}, 1.0f,
                                     &error))
        << error.toStdString();
    compositor.end();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> pixels =
        readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(pixels.size(),
              size_t(source->width()) * size_t(source->height()) * 4)
        << error.toStdString();
    bool visible = false;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i] > 8 || pixels[i + 1] > 8 || pixels[i + 2] > 8) {
            visible = true;
            break;
        }
    }
    EXPECT_TRUE(visible);
}

TEST(LayerGeometry, FitsAndOffsetsTransform)
{
    const LayerRect fit = layerRectFor(Transform{}, 1920, 1080, 1920, 1080);
    EXPECT_FLOAT_EQ(fit.x, 0.0f);
    EXPECT_FLOAT_EQ(fit.w, 1.0f);

    Transform zoom;
    zoom.scaleMilli = 2000;
    zoom.offsetXPermille = 250;
    const LayerRect zoomed = layerRectFor(zoom, 1920, 1080, 1920, 1080);
    EXPECT_NEAR(zoomed.w, 2.0f, 1e-4f);
    // Centered 2x layer starts at -0.5; a +0.25 frame-width offset moves it to -0.25.
    EXPECT_NEAR(zoomed.x, -0.25f, 1e-4f);
}
