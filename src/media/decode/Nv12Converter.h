#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context;

class Nv12Converter {
public:
    bool initialize(D3D11Context &context, QString *error = nullptr);
    bool convert(ID3D11Texture2D *nv12, UINT subresource, int width, int height,
                 ID3D11RenderTargetView *target, float rotationRadians,
                 QString *error = nullptr);
    bool convert(ID3D11Texture2D *luma, ID3D11Texture2D *chroma,
                 int width, int height, ID3D11RenderTargetView *target,
                 float rotationRadians, QString *error = nullptr);

private:
    bool draw(ID3D11ShaderResourceView *lumaSrv,
              ID3D11ShaderResourceView *chromaSrv,
              int width, int height, ID3D11RenderTargetView *target,
              float rotationRadians);

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_params;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
};

} // namespace VideoEditor

#endif
