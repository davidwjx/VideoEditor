#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context;

class RgbaConverter {
public:
    bool initialize(D3D11Context &context, QString *error = nullptr);
    bool convert(ID3D11Texture2D *source, int width, int height,
                 ID3D11RenderTargetView *target, float rotationRadians,
                 QString *error = nullptr);
    bool convertRgba(const std::vector<uint8_t> &rgba, int width, int height,
                     ID3D11RenderTargetView *target, float rotationRadians,
                     QString *error = nullptr);

private:
    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_params;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_uploadTexture;
    int m_uploadWidth = 0;
    int m_uploadHeight = 0;
};

} // namespace VideoEditor

#endif
