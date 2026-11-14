#pragma once

#ifdef _WIN32

#include <array>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context;

class SharedTextureRing {
public:
    static constexpr int kSlotCount = 3;

    explicit SharedTextureRing(D3D11Context &context);

    bool create(int width, int height, int count = kSlotCount,
                QString *error = nullptr);
    bool isValid() const { return m_valid; }
    int count() const { return m_count; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    ID3D11Texture2D *texture(int index) const;
    ID3D11RenderTargetView *rtv(int index) const;
    IDXGIKeyedMutex *mutex(int index) const;
    HANDLE sharedHandle(int index) const;

    int nextWriteIndex() const { return m_nextWrite; }
    void advanceWriteIndex() { m_nextWrite = (m_nextWrite + 1) % m_count; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
        HANDLE handle = nullptr;
    };

    D3D11Context &m_context;
    std::array<Slot, kSlotCount> m_slots;
    int m_count = 0;
    int m_width = 0;
    int m_height = 0;
    int m_nextWrite = 0;
    bool m_valid = false;
};

} // namespace VideoEditor

#endif
