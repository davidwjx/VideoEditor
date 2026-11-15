#include "LayerCompositor.h"

#ifdef _WIN32

#include <algorithm>
#include <cstring>

#include <d3dcompiler.h>

#include "D3D11Context.h"
#include "IVideoDecoder.h"

namespace VideoEditor {

namespace {

// Vertex shader builds a quad from SV_VertexID using the destination rect in
// normalized output space. Vertex 0..3 map to (0,0) (1,0) (0,1) (1,1) UV.
constexpr const char *kLayerVs = R"(
cbuffer LayerParams : register(b0)
{
    float4 rect;
    float opacity;
    float3 padding;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 uv = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    VSOut o;
    o.uv = uv;
    o.pos = float4(rect.x * 2.0 - 1.0 + uv.x * rect.z * 2.0,
                   1.0 - rect.y * 2.0 - uv.y * rect.w * 2.0,
                   0.0, 1.0);
    return o;
}
)";

// Sources are straight-alpha RGBA. Opacity multiplies coverage; the fixed
// function blend state does src.rgb * src.a + dst * (1 - src.a).
constexpr const char *kLayerPs = R"(
Texture2D<float4> src  : register(t0);
SamplerState      samp : register(s0);

cbuffer LayerParams : register(b0)
{
    float4 rect;
    float opacity;
    float3 padding;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut i) : SV_Target
{
    float4 c = src.Sample(samp, i.uv);
    return float4(c.rgb, c.a * opacity);
}
)";

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

Microsoft::WRL::ComPtr<ID3DBlob> compile(const char *source, const char *entry,
                                         const char *target, QString *error)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr,
                                  nullptr, entry, target,
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob,
                                  &diagnostics);
    if (FAILED(hr)) {
        const QString detail =
            diagnostics
                ? QString::fromUtf8(
                      static_cast<const char *>(diagnostics->GetBufferPointer()),
                      int(diagnostics->GetBufferSize()))
                : QStringLiteral("未知着色器错误");
        setError(error, QStringLiteral("合成着色器编译失败：%1").arg(detail));
    }
    return blob;
}

} // namespace

bool LayerCompositor::initialize(D3D11Context &context, int width, int height,
                                 QString *error)
{
    m_device = context.device();
    m_context = context.context();
    m_width = width;
    m_height = height;
    if (!m_device || !m_context) {
        setError(error, QStringLiteral("D3D11 设备不可用。"));
        return false;
    }

    auto vsBlob = compile(kLayerVs, "main", "vs_5_0", error);
    auto psBlob = compile(kLayerPs, "main", "ps_5_0", error);
    if (!vsBlob || !psBlob)
        return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(), nullptr,
                                            &m_vertexShader))
        || FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                              psBlob->GetBufferSize(), nullptr,
                                              &m_pixelShader))) {
        setError(error, QStringLiteral("无法创建合成着色器对象。"));
        return false;
    }

    D3D11_BUFFER_DESC paramsDesc{};
    paramsDesc.ByteWidth = 32; // float4 rect + float opacity + float3 padding
    paramsDesc.Usage = D3D11_USAGE_DYNAMIC;
    paramsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    paramsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&paramsDesc, nullptr, &m_params))) {
        setError(error, QStringLiteral("无法创建合成参数缓冲区。"));
        return false;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&blendDesc, &m_blend))) {
        setError(error, QStringLiteral("无法创建合成混合状态。"));
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
        setError(error, QStringLiteral("无法创建合成采样器。"));
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&rasterizerDesc, &m_rasterizer))) {
        setError(error, QStringLiteral("无法创建合成光栅化状态。"));
        return false;
    }

    return true;
}

void LayerCompositor::applyDrawState()
{
    // Every draw re-establishes the full pipeline state because frame
    // converters used between draws retarget the render target, viewport,
    // shaders, sampler and constant buffers on the shared immediate context.
    if (!m_context || !m_target || !m_params)
        return;
    m_context->OMSetRenderTargets(1, &m_target, nullptr);
    m_context->RSSetViewports(1, &m_viewport);
    m_context->OMSetBlendState(m_blend.Get(), nullptr, 0xffffffff);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
}

void LayerCompositor::begin(ID3D11RenderTargetView *target, bool clearBlack)
{
    if (!m_context || !target)
        return;

    m_target = target;

    int viewportWidth = m_width;
    int viewportHeight = m_height;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    if (resource) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            viewportWidth = int(desc.Width);
            viewportHeight = int(desc.Height);
        }
    }

    m_viewport = {};
    m_viewport.Width = float(viewportWidth);
    m_viewport.Height = float(viewportHeight);
    m_viewport.MinDepth = 0.0f;
    m_viewport.MaxDepth = 1.0f;

    m_context->OMSetRenderTargets(1, &m_target, nullptr);
    m_context->RSSetViewports(1, &m_viewport);
    if (clearBlack) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_context->ClearRenderTargetView(target, black);
    }
    m_context->OMSetBlendState(m_blend.Get(), nullptr, 0xffffffff);
}

void LayerCompositor::end()
{
    if (!m_context)
        return;
    ID3D11ShaderResourceView *nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    m_context->VSSetShader(nullptr, nullptr, 0);
    m_context->PSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer *nullBuffer = nullptr;
    m_context->VSSetConstantBuffers(0, 1, &nullBuffer);
    m_context->PSSetConstantBuffers(0, 1, &nullBuffer);
    m_context->PSSetSamplers(0, 0, nullptr);
    m_context->RSSetState(nullptr);
    m_context->IASetInputLayout(nullptr);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    m_target = nullptr;
}

void LayerCompositor::forgetTexture(ID3D11Texture2D *texture)
{
    if (!texture)
        return;
    m_textureSrvs.erase(
        std::remove_if(m_textureSrvs.begin(), m_textureSrvs.end(),
                       [texture](const CachedSrv &entry) {
                           return entry.texture == texture;
                       }),
        m_textureSrvs.end());
}

void LayerCompositor::clearTextureCache()
{
    m_textureSrvs.clear();
}

bool LayerCompositor::drawTexture(ID3D11ShaderResourceView *srv,
                                  const LayerRect &rect, float opacity,
                                  QString *error)
{
    if (!m_context || !m_params || !srv) {
        setError(error, QStringLiteral("合成纹理不可用。"));
        return false;
    }

    const float params[8] = {rect.x, rect.y, rect.w, rect.h, opacity,
                             0.0f,   0.0f,   0.0f};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped))) {
        setError(error, QStringLiteral("无法更新合成参数。"));
        return false;
    }
    std::memcpy(mapped.pData, params, sizeof(params));
    m_context->Unmap(m_params.Get(), 0);

    applyDrawState();
    m_context->PSSetShaderResources(0, 1, &srv);
    m_context->Draw(4, 0);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    return true;
}

LayerCompositor::ImageTexture *
LayerCompositor::imageTextureFor(int width, int height, QString *error)
{
    const quint64 key = (quint64(quint32(width)) << 32) | quint32(height);
    auto existing = m_imageTextures.find(key);
    if (existing != m_imageTextures.end())
        return &existing->second;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ImageTexture entry;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &entry.texture))) {
        setError(error, QStringLiteral("无法创建图层纹理。"));
        return nullptr;
    }
    if (FAILED(m_device->CreateShaderResourceView(entry.texture.Get(), nullptr,
                                                  &entry.srv))) {
        setError(error, QStringLiteral("无法创建图层纹理视图。"));
        return nullptr;
    }

    auto inserted = m_imageTextures.emplace(key, std::move(entry));
    return &inserted.first->second;
}

bool LayerCompositor::drawImage(const QImage &image, const LayerRect &rect,
                                float opacity, QString *error)
{
    return drawImageCached(QString(), image, rect, opacity, error);
}

bool LayerCompositor::drawImageCached(const QString &cacheKey,
                                      const QImage &image,
                                      const LayerRect &rect, float opacity,
                                      QString *error)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        setError(error, QStringLiteral("图层图像为空。"));
        return false;
    }

    ImageTexture *target = imageTextureFor(image.width(), image.height(), error);
    if (!target)
        return false;

    const bool unchanged =
        !cacheKey.isEmpty() && cacheKey == m_cachedImageKey
        && m_cachedImageWidth == image.width()
        && m_cachedImageHeight == image.height();
    if (!unchanged) {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        m_context->UpdateSubresource(target->texture.Get(), 0, nullptr,
                                     rgba.constBits(),
                                     UINT(rgba.bytesPerLine()), 0);
        m_cachedImageKey = cacheKey;
        m_cachedImageWidth = image.width();
        m_cachedImageHeight = image.height();
    }
    return drawTexture(target->srv.Get(), rect, opacity, error);
}

ID3D11ShaderResourceView *
LayerCompositor::textureSrv(ID3D11Texture2D *texture, UINT subresource,
                            QString *error)
{
    if (!texture) {
        setError(error, QStringLiteral("视频帧纹理为空。"));
        return nullptr;
    }
    for (const CachedSrv &entry : m_textureSrvs) {
        if (entry.texture == texture && entry.subresource == subresource)
            return entry.srv.Get();
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = S_OK;
    if (desc.ArraySize > 1) {
        // A decoder pool frame is an array slice; D3D11 has no PlaneSlice for
        // view descs, the subresource selects both slice and plane.
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = subresource;
        srvDesc.Texture2DArray.ArraySize = 1;
        hr = m_device->CreateShaderResourceView(texture, &srvDesc, &srv);
    } else {
        hr = m_device->CreateShaderResourceView(texture, nullptr, &srv);
    }
    if (FAILED(hr) || !srv) {
        setError(error, QStringLiteral("无法创建视频帧纹理视图。"));
        return nullptr;
    }

    // Hold a reference so a later texture allocation can never reuse the same
    // pointer while the cached view is still alive. The cache is bounded by
    // the number of simultaneously active sources; clear it if it grows.
    if (m_textureSrvs.size() >= 8)
        m_textureSrvs.erase(m_textureSrvs.begin());
    CachedSrv entry;
    entry.texture = texture;
    entry.subresource = subresource;
    entry.keepAlive = texture;
    entry.srv = srv;
    m_textureSrvs.push_back(std::move(entry));
    return m_textureSrvs.back().srv.Get();
}

bool LayerCompositor::drawFrame(const DecodedFrame &frame,
                                const LayerRect &rect, float opacity,
                                QString *error)
{
    if (frame.kind == DecodedFrame::Kind::CpuRgba) {
        if (frame.width <= 0 || frame.height <= 0) {
            setError(error, QStringLiteral("视频帧尺寸无效。"));
            return false;
        }
        const size_t needed = size_t(frame.width) * size_t(frame.height) * 4;
        if (frame.rgba.size() < needed) {
            setError(error, QStringLiteral("视频帧像素数据不完整。"));
            return false;
        }
        const QImage image(frame.rgba.data(), frame.width, frame.height,
                           frame.width * 4, QImage::Format_RGBA8888);
        return drawImage(image, rect, opacity, error);
    }

    ID3D11ShaderResourceView *srv =
        textureSrv(frame.texture, frame.subresource, error);
    if (!srv)
        return false;
    return drawTexture(srv, rect, opacity, error);
}

} // namespace VideoEditor

#endif
