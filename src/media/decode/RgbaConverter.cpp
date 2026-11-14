#include "RgbaConverter.h"

#ifdef _WIN32

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

Microsoft::WRL::ComPtr<ID3DBlob> compileShader(const char *source,
                                               const char *entry,
                                               const char *target,
                                               QString *error)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                                  entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  &blob, &diagnostics);
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

bool RgbaConverter::initialize(D3D11Context &context, QString *error)
{
    m_device = context.device();
    m_context = context.context();

    auto vsBlob = compileShader(shaders::kFullscreenVs, "main", "vs_5_0", error);
    auto psBlob = compileShader(shaders::kRgbaPs, "main", "ps_5_0", error);
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

    // Match the NV12 path: the fullscreen triangle must not be back-face culled.
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

bool RgbaConverter::convert(ID3D11Texture2D *source, int width, int height,
                            ID3D11RenderTargetView *target, float rotationRadians,
                            QString *error)
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(m_device->CreateShaderResourceView(source, nullptr, &srv))) {
        setError(error, QStringLiteral("无法创建 RGBA 纹理视图。"));
        return false;
    }

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

    m_context->PSSetShaderResources(0, 1, srv.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);

    // Do not leave the temporary SRV or the render target bound on the shared
    // immediate context: stale bindings keep source frames alive and leak
    // state into whatever draws next.
    ID3D11ShaderResourceView *nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

bool RgbaConverter::convertRgba(const std::vector<uint8_t> &rgba, int width,
                                int height, ID3D11RenderTargetView *target,
                                float rotationRadians, QString *error)
{
    if (rgba.size() < size_t(width) * size_t(height) * 4) {
        setError(error, QStringLiteral("RGBA 缓冲区尺寸不正确。"));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!m_uploadTexture || m_uploadWidth != width || m_uploadHeight != height) {
        m_uploadTexture.Reset();
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_uploadTexture))) {
            setError(error, QStringLiteral("无法创建 CPU 上传纹理。"));
            return false;
        }
        m_uploadWidth = width;
        m_uploadHeight = height;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_uploadTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                              0, &mapped))) {
        setError(error, QStringLiteral("无法映射 CPU 上传纹理。"));
        return false;
    }
    for (int row = 0; row < height; ++row) {
        std::memcpy(static_cast<uint8_t *>(mapped.pData)
                        + size_t(row) * mapped.RowPitch,
                    rgba.data() + size_t(row) * size_t(width) * 4,
                    size_t(width) * 4);
    }
    m_context->Unmap(m_uploadTexture.Get(), 0);

    return convert(m_uploadTexture.Get(), width, height, target, rotationRadians,
                   error);
}

} // namespace VideoEditor

#endif
