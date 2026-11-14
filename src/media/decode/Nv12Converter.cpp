#include "Nv12Converter.h"

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <d3dcompiler.h>

#include "D3D11Context.h"
#include "Shaders.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

Microsoft::WRL::ComPtr<ID3DBlob> compile(const char *source,
                                         const char *entry,
                                         const char *target,
                                         QString *error)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                                  entry, target, flags, 0, &blob, &diagnostics);
    if (FAILED(hr)) {
        const QString detail = diagnostics
            ? QString::fromUtf8(static_cast<const char *>(diagnostics->GetBufferPointer()),
                                int(diagnostics->GetBufferSize()))
            : QStringLiteral("unknown shader error");
        setError(error, QStringLiteral("着色器编译失败：%1").arg(detail));
    }
    return blob;
}

} // namespace

bool Nv12Converter::initialize(D3D11Context &context, QString *error)
{
    m_device = context.device();
    m_context = context.context();

    auto vsBlob = compile(shaders::kFullscreenVs, "main", "vs_5_0", error);
    auto psBlob = compile(shaders::kNv12Ps, "main", "ps_5_0", error);
    if (!vsBlob || !psBlob)
        return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(), nullptr, &m_vs))
        || FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                              psBlob->GetBufferSize(), nullptr, &m_ps))) {
        setError(error, QStringLiteral("无法创建着色器对象。"));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&samplerDesc, &m_sampler))) {
        setError(error, QStringLiteral("无法创建采样器。"));
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 16;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_params))) {
        setError(error, QStringLiteral("无法创建参数缓冲区。"));
        return false;
    }

    // The fullscreen triangle has clockwise winding; without CULL_NONE it can
    // be culled and the output stays black.
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&rasterizer, &m_rasterizer))) {
        setError(error, QStringLiteral("无法创建光栅化状态。"));
        return false;
    }

    return true;
}

bool Nv12Converter::convert(ID3D11Texture2D *nv12, UINT subresource,
                            int width, int height, ID3D11RenderTargetView *target,
                            float rotationRadians, QString *error)
{
    if (!nv12) {
        setError(error, QStringLiteral("视频纹理为空。"));
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    nv12->GetDesc(&sourceDesc);
    if (!(sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        // FFmpeg's D3D11VA pool textures carry only D3D11_BIND_DECODER. Copy
        // the decoded array slice into a texture that can be sampled.
        //
        // The pool is allocated at coded/alignment dimensions, which can be
        // larger than the visible frame (e.g. a 640x360 stream in a 640x368
        // surface). Copy only the visible box, never the whole subresource,
        // or the implied destination box can run past the destination texture.
        // NV12 textures must have even dimensions; round the destination up
        // for odd display sizes while still copying only the visible region.
        const UINT copyWidth = std::min<UINT>(UINT(std::max(width, 0)),
                                              sourceDesc.Width);
        const UINT copyHeight = std::min<UINT>(UINT(std::max(height, 0)),
                                               sourceDesc.Height);
        if (copyWidth == 0 || copyHeight == 0) {
            setError(error, QStringLiteral("视频帧尺寸无效。"));
            return false;
        }
        const UINT textureWidth = (copyWidth + 1u) & ~1u;
        const UINT textureHeight = (copyHeight + 1u) & ~1u;
        if (!m_sampleableTexture || m_sampleableWidth != int(textureWidth)
            || m_sampleableHeight != int(textureHeight)) {
            D3D11_TEXTURE2D_DESC copyDesc{};
            copyDesc.Width = textureWidth;
            copyDesc.Height = textureHeight;
            copyDesc.MipLevels = 1;
            copyDesc.ArraySize = 1;
            copyDesc.Format = DXGI_FORMAT_NV12;
            copyDesc.SampleDesc.Count = 1;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            m_sampleableTexture.Reset();
            if (FAILED(m_device->CreateTexture2D(&copyDesc, nullptr,
                                                 &m_sampleableTexture))) {
                setError(error, QStringLiteral("无法创建可采样的视频纹理。"));
                return false;
            }
            m_sampleableWidth = int(textureWidth);
            m_sampleableHeight = int(textureHeight);
        }
        const D3D11_BOX sourceBox{0, 0, 0, copyWidth, copyHeight, 1};
        m_context->CopySubresourceRegion(m_sampleableTexture.Get(), 0, 0, 0, 0,
                                         nv12, subresource, &sourceBox);
        nv12 = m_sampleableTexture.Get();
        subresource = 0;
    }

    // D3D11 selects the NV12 plane from the view format (R8 -> luma,
    // R8G8 -> chroma); there is no PlaneSlice member in D3D11 view descs.
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MostDetailedMip = 0;
    desc.Texture2DArray.MipLevels = 1;
    desc.Texture2DArray.FirstArraySlice = subresource;
    desc.Texture2DArray.ArraySize = 1;

    desc.Format = DXGI_FORMAT_R8_UNORM;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaSrv;
    if (FAILED(m_device->CreateShaderResourceView(nv12, &desc, &lumaSrv))) {
        setError(error, QStringLiteral("无法创建亮度平面视图。"));
        return false;
    }

    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaSrv;
    if (FAILED(m_device->CreateShaderResourceView(nv12, &desc, &chromaSrv))) {
        setError(error, QStringLiteral("无法创建色度平面视图。"));
        return false;
    }

    return draw(lumaSrv.Get(), chromaSrv.Get(), width, height, target,
                rotationRadians);
}

bool Nv12Converter::convert(ID3D11Texture2D *luma, ID3D11Texture2D *chroma,
                            int width, int height, ID3D11RenderTargetView *target,
                            float rotationRadians, QString *error)
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaSrv;
    if (FAILED(m_device->CreateShaderResourceView(luma, nullptr, &lumaSrv))
        || FAILED(m_device->CreateShaderResourceView(chroma, nullptr, &chromaSrv))) {
        setError(error, QStringLiteral("无法创建测试平面视图。"));
        return false;
    }
    return draw(lumaSrv.Get(), chromaSrv.Get(), width, height, target,
                rotationRadians);
}

bool Nv12Converter::draw(ID3D11ShaderResourceView *lumaSrv,
                         ID3D11ShaderResourceView *chromaSrv,
                         int width, int height, ID3D11RenderTargetView *target,
                         float rotationRadians)
{
    const float params[4] = {rotationRadians, 0.0f, 0.0f, 0.0f};
    m_context->UpdateSubresource(m_params.Get(), 0, nullptr, params, 0, 0);

    ID3D11RenderTargetView *targets[] = {target};
    m_context->OMSetRenderTargets(1, targets, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);
    m_context->RSSetState(m_rasterizer.Get());

    ID3D11ShaderResourceView *srvs[] = {lumaSrv, chromaSrv};
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);

    // Do not leave temporary SRVs or the render target bound on the shared
    // immediate context: stale bindings keep decoder frames alive and leak
    // state into whatever draws next.
    ID3D11ShaderResourceView *nullSrvs[2] = {nullptr, nullptr};
    m_context->PSSetShaderResources(0, 2, nullSrvs);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

} // namespace VideoEditor

#endif
