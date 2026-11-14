#include "SharedTextureRing.h"

#ifdef _WIN32

#include <d3d11.h>

#include "D3D11Context.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

SharedTextureRing::SharedTextureRing(D3D11Context &context)
    : m_context(context)
{
}

bool SharedTextureRing::create(int width, int height, int count, QString *error)
{
    if (count <= 0 || count > kSlotCount) {
        setError(error, QStringLiteral("共享纹理数量无效：%1").arg(count));
        return false;
    }

    for (int i = 0; i < count; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(width);
        desc.Height = UINT(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // D3D11 treats D3D11_RESOURCE_MISC_SHARED and
        // D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX as mutually exclusive; use the
        // keyed-mutex variant only (GetSharedHandle still works).
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = m_context.device()->CreateTexture2D(
            &desc, nullptr, &m_slots[i].texture);
        if (FAILED(hr)) {
            setError(error, QStringLiteral("无法创建共享纹理（0x%1）。")
                                .arg(quint32(hr), 8, 16, QLatin1Char('0')));
            return false;
        }

        if (FAILED(m_context.device()->CreateRenderTargetView(
                m_slots[i].texture.Get(), nullptr, &m_slots[i].rtv))) {
            setError(error, QStringLiteral("无法创建渲染目标视图。"));
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        if (FAILED(m_slots[i].texture.As(&resource))
            || FAILED(resource->GetSharedHandle(&m_slots[i].handle))) {
            setError(error, QStringLiteral("无法获取共享纹理句柄。"));
            return false;
        }

        if (FAILED(m_slots[i].texture.As(&m_slots[i].mutex))) {
            setError(error, QStringLiteral("无法获取键控互斥量。"));
            return false;
        }
    }

    m_count = count;
    m_width = width;
    m_height = height;
    m_nextWrite = 0;
    m_valid = true;
    return true;
}

ID3D11Texture2D *SharedTextureRing::texture(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].texture.Get() : nullptr;
}

ID3D11RenderTargetView *SharedTextureRing::rtv(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].rtv.Get() : nullptr;
}

IDXGIKeyedMutex *SharedTextureRing::mutex(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].mutex.Get() : nullptr;
}

HANDLE SharedTextureRing::sharedHandle(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].handle : nullptr;
}

} // namespace VideoEditor

#endif
