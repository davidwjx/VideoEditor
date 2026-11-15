#pragma once

#ifdef _WIN32

#include <cstdint>
#include <memory>

#include <d3d11.h>
#include <wrl/client.h>

#include <QSize>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;
class Nv12Converter;
class RgbaConverter;
struct Asset;

// Owns one decoder for one clip and normalizes every decoded frame into a
// single RGBA texture, honouring the source display rotation. The compositor
// therefore only ever samples RGBA, whether the decoder produced a hardware
// NV12 texture or a software RGBA buffer.
class ClipVideoSource {
public:
    static std::unique_ptr<ClipVideoSource> open(const Asset &asset,
                                                 D3D11Context &context,
                                                 const QString &proxyPath,
                                                 QString *error = nullptr);
    ~ClipVideoSource();

    // Returns a frame covering sourceUs, or the last good frame when the
    // decoder reaches EOF. Returns nullptr on seek/convert failure.
    const DecodedFrame *frameAt(int64_t sourceUs, QString *error = nullptr);

    int width() const { return m_displaySize.width(); }
    int height() const { return m_displaySize.height(); }

private:
    ClipVideoSource() = default;

    bool createConvertedTexture(QString *error);
    bool convertToRgba(DecodedFrame &frame, QString *error);

    std::unique_ptr<IVideoDecoder> m_decoder;
    std::unique_ptr<Nv12Converter> m_nv12;
    std::unique_ptr<RgbaConverter> m_rgba;
    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_converted;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_convertedRtv;
    DecodedFrame m_frame;
    QSize m_displaySize;
    int64_t m_frameDurationUs = 33'333;
    int64_t m_lastPtsUs = 0;
    float m_rotationRadians = 0.0f;
    bool m_hasFrame = false;
};

} // namespace VideoEditor

#endif
