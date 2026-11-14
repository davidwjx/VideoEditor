#include "FrameBridge.h"

#ifdef _WIN32

#include <qnativeinterface.h>
#include <QSGRendererInterface>

namespace VideoEditor {

bool FrameBridge::initialize(QQuickWindow *window,
                             const std::array<quintptr, 3> &handles,
                             int width, int height, QString *error)
{
    if (!window) {
        if (error)
            *error = QStringLiteral("预览窗口为空。");
        return false;
    }

    // getResource() is an instance method on QSGRendererInterface in Qt 6.8;
    // the window only hands it out on the scene-graph render thread, which is
    // where updatePaintNode() calls initialize().
    QSGRendererInterface *renderer = window->rendererInterface();
    if (!renderer) {
        if (error)
            *error = QStringLiteral("Qt 场景图尚未使用 D3D11 后端。");
        return false;
    }

    auto *device = static_cast<ID3D11Device *>(
        renderer->getResource(window, QSGRendererInterface::DeviceResource));
    auto *context = static_cast<ID3D11DeviceContext *>(
        renderer->getResource(window, QSGRendererInterface::DeviceContextResource));
    if (!device || !context) {
        if (error)
            *error = QStringLiteral("Qt 场景图尚未使用 D3D11 后端。");
        return false;
    }

    m_device = device;
    m_context = context;
    m_width = width;
    m_height = height;

    for (int i = 0; i < 3; ++i) {
        if (!handles[i]) {
            if (error)
                *error = QStringLiteral("共享纹理句柄为空。");
            return false;
        }

        if (FAILED(m_device->OpenSharedResource(
                reinterpret_cast<HANDLE>(handles[i]),
                IID_PPV_ARGS(&m_slots[i].shared)))) {
            if (error)
                *error = QStringLiteral("无法打开共享纹理。");
            return false;
        }

        if (FAILED(m_slots[i].shared.As(&m_slots[i].mutex))) {
            if (error)
                *error = QStringLiteral("无法获取共享纹理互斥量。");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        m_slots[i].shared->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_slots[i].owned))) {
            if (error)
                *error = QStringLiteral("无法创建 Qt 侧纹理。");
            return false;
        }
    }

    m_valid = true;
    return true;
}

QSGTexture *FrameBridge::present(int slot, QQuickWindow *window)
{
    if (!m_valid || slot < 0 || slot >= 3 || !window)
        return nullptr;

    Slot &target = m_slots[slot];
    if (!target.mutex)
        return nullptr;
    // The producer releases key 1 before it announces the frame, so a miss is
    // transient. Never block the render thread waiting for a producer; a short
    // grace absorbs GPU submission latency and the caller retries on the next
    // frame when it still misses.
    const HRESULT acquire = target.mutex->AcquireSync(1, 2);
    if (acquire != S_OK)
        return nullptr;

    m_context->CopyResource(target.owned.Get(), target.shared.Get());
    target.mutex->ReleaseSync(0);

    QSGTexture *texture = QNativeInterface::QSGD3D11Texture::fromNative(
        target.owned.Get(), window, QSize(m_width, m_height), {});
    return texture;
}

void FrameBridge::discard(int slot)
{
    if (!m_valid || slot < 0 || slot >= 3)
        return;
    Slot &slotData = m_slots[slot];
    if (slotData.mutex && slotData.mutex->AcquireSync(1, 0) == S_OK)
        slotData.mutex->ReleaseSync(0);
}

} // namespace VideoEditor

#endif
