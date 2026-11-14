#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context {
public:
    static std::unique_ptr<D3D11Context> create(bool enableVideo,
                                                QString *error = nullptr);

    ID3D11Device *device() const { return m_device.Get(); }
    ID3D11DeviceContext *context() const { return m_context.Get(); }
    ID3D11VideoDevice *videoDevice() const { return m_videoDevice.Get(); }
    bool supportsVideoDecoding() const { return m_videoDevice != nullptr; }
    QString adapterName() const { return m_adapterName; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    QString m_adapterName;
};

} // namespace VideoEditor

#endif
