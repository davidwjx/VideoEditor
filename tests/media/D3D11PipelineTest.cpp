#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <QString>

#include "D3D11Context.h"
#include "FrameReadback.h"
#include "Nv12Converter.h"
#include "RgbaConverter.h"
#include "SharedTextureRing.h"

using namespace VideoEditor;
using Microsoft::WRL::ComPtr;

namespace {

// GTEST_SKIP() cannot be used from a factory returning a value, so the tests
// create the context themselves and skip on failure with the error message.
std::unique_ptr<D3D11Context> createContext(QString *error)
{
    return D3D11Context::create(false, error);
}

// 创建两张 R8 / R8G8 纹理，模拟 NV12 的两个平面。
bool makeNv12Planes(ID3D11Device *device, int width, int height,
                    uint8_t y, uint8_t u, uint8_t v,
                    ID3D11Texture2D **luma, ID3D11Texture2D **chroma)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Format = DXGI_FORMAT_R8_UNORM;

    std::vector<uint8_t> yPlane(size_t(width) * height, y);
    D3D11_SUBRESOURCE_DATA yData{};
    yData.pSysMem = yPlane.data();
    yData.SysMemPitch = UINT(width);
    if (FAILED(device->CreateTexture2D(&desc, &yData, luma)))
        return false;

    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    std::vector<uint8_t> uvPlane(size_t(width) * height * 2, 0);
    for (int i = 0; i < width * height; ++i) {
        uvPlane[size_t(i) * 2] = u;
        uvPlane[size_t(i) * 2 + 1] = v;
    }
    D3D11_SUBRESOURCE_DATA uvData{};
    uvData.pSysMem = uvPlane.data();
    uvData.SysMemPitch = UINT(width * 2);
    return SUCCEEDED(device->CreateTexture2D(&desc, &uvData, chroma));
}

} // namespace

TEST(D3D11Pipeline, CreatesSharedTextureRing)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    EXPECT_NE(ring.sharedHandle(0), nullptr);
    EXPECT_NE(ring.sharedHandle(1), nullptr);
    EXPECT_NE(ring.sharedHandle(2), nullptr);
}

TEST(D3D11Pipeline, ConvertsNv12RedToRgba)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    ID3D11Device *device = context->device();
    ID3D11DeviceContext *deviceContext = context->context();

    ID3D11Texture2D *luma = nullptr;
    ID3D11Texture2D *chroma = nullptr;
    ASSERT_TRUE(makeNv12Planes(device, 64, 64, 63, 102, 240, &luma, &chroma));

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 1, &error)) << error.toStdString();

    Nv12Converter converter;
    ASSERT_TRUE(converter.initialize(*context, &error)) << error.toStdString();

    ComPtr<IDXGIKeyedMutex> mutex;
    ring.texture(0)->QueryInterface(IID_PPV_ARGS(&mutex));
    ASSERT_NE(mutex, nullptr);
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);
    ASSERT_TRUE(converter.convert(luma, chroma, 64, 64, ring.rtv(0), 0.0f, &error))
        << error.toStdString();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(64 * 64 * 4)) << error.toStdString();
    EXPECT_GT(rgba[0], 200);
    EXPECT_LT(rgba[1], 60);
    EXPECT_LT(rgba[2], 60);
    EXPECT_EQ(rgba[3], 255);

    luma->Release();
    chroma->Release();
    deviceContext->Flush();
}

TEST(D3D11Pipeline, ConvertsRgbaBuffer)
{
    QString error;
    auto context = createContext(&error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    ID3D11Device *device = context->device();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32;
    desc.Height = 32;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<uint8_t> pixels(32 * 32 * 4, 255);
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = 32 * 4;

    ComPtr<ID3D11Texture2D> source;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &source)));

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(32, 32, 1, &error)) << error.toStdString();

    RgbaConverter converter;
    ASSERT_TRUE(converter.initialize(*context, &error)) << error.toStdString();

    ComPtr<IDXGIKeyedMutex> mutex;
    ring.texture(0)->QueryInterface(IID_PPV_ARGS(&mutex));
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);
    ASSERT_TRUE(converter.convert(source.Get(), 32, 32, ring.rtv(0), 0.0f, &error))
        << error.toStdString();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(32 * 32 * 4));
    EXPECT_GT(rgba[0], 200);
    EXPECT_GT(rgba[1], 200);
    EXPECT_GT(rgba[2], 200);
}
