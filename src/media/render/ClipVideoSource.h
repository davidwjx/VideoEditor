#pragma once

#ifdef _WIN32

#include <cstdint>
#include <memory>

#include <d3d11.h>
#include <wrl/client.h>

#include <QSize>
#include <QString>

#include "IVideoDecoder.h"
#include "Project.h"

namespace VideoEditor {

class D3D11Context;
class Nv12Converter;
class RgbaConverter;

// Chooses the file a timeline preview should decode. Toggling "use proxy" off
// must hand an empty path to ClipVideoSource so the original source is used
// even when a proxy file exists on disk.
inline QString activeProxyPath(const Asset &asset, bool useProxy)
{
    return useProxy ? asset.proxyPath : QString();
}

// One NV12->RGBA and one software-RGBA converter, compiled once and shared by
// all ClipVideoSource instances of a TimelineRenderWorker.
class VideoConverters {
public:
    VideoConverters();
    ~VideoConverters();

    VideoConverters(const VideoConverters &) = delete;
    VideoConverters &operator=(const VideoConverters &) = delete;

    bool initialize(D3D11Context &context, QString *error = nullptr);
    bool isInitialized() const { return m_initialized; }

    Nv12Converter *nv12() const { return m_nv12.get(); }
    RgbaConverter *rgba() const { return m_rgba.get(); }

private:
    std::unique_ptr<Nv12Converter> m_nv12;
    std::unique_ptr<RgbaConverter> m_rgba;
    bool m_initialized = false;
};

// Owns one decoder for one clip and normalizes every decoded frame into a
// single RGBA texture, honouring the source display rotation. The conversion
// target uses the decoder's actual (possibly proxy-sized) frame dimensions, so
// 540p proxies are not upscaled to the source display resolution first.
class ClipVideoSource {
public:
    static std::unique_ptr<ClipVideoSource> open(const Asset &asset,
                                                 D3D11Context &context,
                                                 const QString &proxyPath,
                                                 QString *error = nullptr);
    // Shared-converter overload used by TimelineRenderWorker: the worker owns
    // one VideoConverters for every clip, so shaders compile once per worker.
    static std::unique_ptr<ClipVideoSource> open(const Asset &asset,
                                                 D3D11Context &context,
                                                 const QString &proxyPath,
                                                 VideoConverters *converters,
                                                 QString *error = nullptr);
    ~ClipVideoSource();

    // Returns a frame covering sourceUs, or the last good frame when the
    // decoder reaches EOF. Returns nullptr on seek/decode/convert failure.
    const DecodedFrame *frameAt(int64_t sourceUs, QString *error = nullptr);

    // Display dimensions of the decoded frames (proxy size when a proxy is
    // used), with 90/270 rotation applied.
    int width() const { return m_displaySize.width(); }
    int height() const { return m_displaySize.height(); }

    // Normalized RGBA texture handed to the compositor (owned by this source).
    ID3D11Texture2D *texture() const { return m_converted.Get(); }

private:
    ClipVideoSource() = default;

    bool createConvertedTexture(QString *error);
    bool convertToRgba(DecodedFrame &frame, QString *error);

    std::unique_ptr<IVideoDecoder> m_decoder;
    std::unique_ptr<VideoConverters> m_ownedConverters;
    VideoConverters *m_converters = nullptr;
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
