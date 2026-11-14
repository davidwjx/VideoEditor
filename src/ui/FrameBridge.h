#pragma once

#ifdef _WIN32

#include <array>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <QQuickWindow>
#include <QSGTexture>
#include <QString>

namespace VideoEditor {

class FrameBridge {
public:
    bool initialize(QQuickWindow *window,
                    const std::array<quintptr, 3> &handles,
                    int width, int height, QString *error = nullptr);
    QSGTexture *present(int slot, QQuickWindow *window);
    bool isValid() const { return m_valid; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> shared;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> owned;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    };

    std::array<Slot, 3> m_slots;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    ID3D11DeviceContext *m_context = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_valid = false;
};

} // namespace VideoEditor

#endif
