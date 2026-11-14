#include "FrameReadback.h"

#ifdef _WIN32

#include <cstring>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "D3D11Context.h"

namespace VideoEditor {

std::vector<uint8_t> readbackSharedTexture(D3D11Context &context,
                                           ID3D11Texture2D *texture,
                                           QString *error)
{
    if (!texture) {
        if (error)
            *error = QStringLiteral("纹理为空。");
        return {};
    }

    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&mutex)))
        || !mutex || mutex->AcquireSync(1, 5000) != S_OK) {
        if (error)
            *error = QStringLiteral("无法获取共享纹理互斥量。");
        return {};
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = context.device()->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) {
        mutex->ReleaseSync(0);
        if (error)
            *error = QStringLiteral("无法创建回读纹理。");
        return {};
    }

    context.context()->CopyResource(staging.Get(), texture);
    mutex->ReleaseSync(0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
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

} // namespace VideoEditor

#endif
