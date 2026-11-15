#pragma once

#ifdef _WIN32

#include <cstdint>
#include <unordered_map>
#include <vector>

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
    // Same as drawImage, but an unchanged non-empty cacheKey reuses the RGBA
    // texture already uploaded for that key: the subtitle image is converted
    // and uploaded once per cue/style change, not once per frame.
    bool drawImageCached(const QString &cacheKey, const QImage &image,
                         const LayerRect &rect, float opacity,
                         QString *error = nullptr);

    // Restricts subsequent draws to a rectangle in output pixels. nullptr
    // restores the full target. begin() always resets to the full target, so
    // a caller that forgets to clear it cannot leak into the next frame.
    void setScissorRect(const RECT *rect);

    // Unbinds the render target and shader resources from the shared context.
    void end();

    // Drop cached views over textures that are about to be released (evicted
    // clip sources or a project switch).
    void forgetTexture(ID3D11Texture2D *texture);
    void clearTextureCache();

private:
    struct ImageTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    bool drawTexture(ID3D11ShaderResourceView *srv, const LayerRect &rect,
                     float opacity, QString *error);
    ImageTexture *imageTextureFor(int width, int height, QString *error);
    ID3D11ShaderResourceView *textureSrv(ID3D11Texture2D *texture,
                                         UINT subresource, QString *error);
    void applyDrawState();

    struct CachedSrv {
        ID3D11Texture2D *texture = nullptr;
        UINT subresource = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> keepAlive;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_params;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    std::unordered_map<quint64, ImageTexture> m_imageTextures;
    QString m_cachedImageKey;
    int m_cachedImageWidth = 0;
    int m_cachedImageHeight = 0;
    std::vector<CachedSrv> m_textureSrvs;
    // Converters inside the worker retarget the immediate context; the
    // compositor caches its pass state and reapplies it before every draw.
    ID3D11RenderTargetView *m_target = nullptr;
    D3D11_VIEWPORT m_viewport{};
    D3D11_RECT m_scissor{};
    int m_width = 0;
    int m_height = 0;
};

} // namespace VideoEditor

#endif
