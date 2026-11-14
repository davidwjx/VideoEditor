#include "D3D11Context.h"

#ifdef _WIN32

#include <d3d10.h>
#include <wrl/client.h>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

std::unique_ptr<D3D11Context> D3D11Context::create(bool enableVideo, QString *error)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (enableVideo)
        flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actualLevel{};

    auto context = std::unique_ptr<D3D11Context>(new D3D11Context);
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, 2, D3D11_SDK_VERSION,
                                   &context->m_device, &actualLevel,
                                   &context->m_context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, &levels[1], 1, D3D11_SDK_VERSION,
                               &context->m_device, &actualLevel,
                               &context->m_context);
    }
    if (FAILED(hr) && !enableVideo) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               flags, levels, 2, D3D11_SDK_VERSION,
                               &context->m_device, &actualLevel,
                               &context->m_context);
    }
    if (FAILED(hr)) {
        setError(error, QStringLiteral("无法创建 D3D11 设备（0x%1）。")
                            .arg(quint32(hr), 8, 16, QLatin1Char('0')));
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context->m_context.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    if (enableVideo)
        context->m_device.As(&context->m_videoDevice);

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(context->m_device.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
                context->m_adapterName = QString::fromWCharArray(desc.Description);
        }
    }

    return context;
}

} // namespace VideoEditor

#endif
