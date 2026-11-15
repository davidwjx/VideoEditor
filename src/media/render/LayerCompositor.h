#pragma once

#ifdef _WIN32

#include <cstdint>
#include <unordered_map>

#include <d3d11.h>
#include <wrl/client.h>

#include <QImage>
#include <QString>

#include "LayerGeometry.h"

namespace VideoEditor {

class D3D11Context;
struct DecodedFrame;

// Sequential D3D11 compositor: every layer is drawn into the same render
// target, bottom to top, so preview (TimelineRenderWorker) and M3 export share
// one composition path. Sources are RGBA only; ClipVideoSource normalizes
// hardware NV12 and software frames before they reach this class.
class LayerCompositor {
public:
    bool initialize(D3D11Context &context, int width, int height,
                    QString *error = nullptr);

    // Binds target as the output and (optionally) clears it to opaque black.
    void begin(ID3D11RenderTargetView *target, bool clearBlack);

    bool drawFrame(const DecodedFrame &frame, const LayerRect &rect,
                   float opacity, QString *error = nullptr);
    bool drawImage(const QImage &image, const LayerRect &rect, float opacity,
                   QString *error = nullptr);

    // Unbinds the render target and shader resources from the shared context.
    void end();

private:
    struct ImageTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    bool drawTexture(ID3D11ShaderResourceView *srv, const LayerRect &rect,
                     float opacity, QString *error);
    ImageTexture *imageTextureFor(int width, int height, QString *error);

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_params;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    std::unordered_map<quint64, ImageTexture> m_imageTextures;
    int m_width = 0;
    int m_height = 0;
};

} // namespace VideoEditor

#endif
