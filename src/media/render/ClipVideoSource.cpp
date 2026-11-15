#include "ClipVideoSource.h"

#ifdef _WIN32

#include <algorithm>

#include <QFileInfo>

#include "D3D11Context.h"
#include "FrameSelector.h"
#include "Nv12Converter.h"
#include "Project.h"
#include "RgbaConverter.h"
#include "TimeBase.h"
#include "VideoDecoderFactory.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

constexpr float kPi = 3.14159265358979323846f;

int normalizedRotation(int degrees)
{
    return ((degrees % 360) + 360) % 360;
}

} // namespace

VideoConverters::VideoConverters()
    : m_nv12(std::make_unique<Nv12Converter>())
    , m_rgba(std::make_unique<RgbaConverter>())
{
}

VideoConverters::~VideoConverters() = default;

bool VideoConverters::initialize(D3D11Context &context, QString *error)
{
    if (m_initialized)
        return true;
    if (!m_nv12->initialize(context, error))
        return false;
    if (!m_rgba->initialize(context, error))
        return false;
    m_initialized = true;
    return true;
}

ClipVideoSource::~ClipVideoSource() = default;

bool ClipVideoSource::createConvertedTexture(QString *error)
{
    if (!m_device || m_displaySize.isEmpty()) {
        setError(error, QStringLiteral("视频源尺寸无效。"));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(m_displaySize.width());
    desc.Height = UINT(m_displaySize.height());
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_converted))) {
        setError(error, QStringLiteral("无法创建 RGBA 转换纹理。"));
        return false;
    }
    if (FAILED(m_device->CreateRenderTargetView(m_converted.Get(), nullptr,
                                                &m_convertedRtv))) {
        setError(error, QStringLiteral("无法创建 RGBA 转换目标。"));
        return false;
    }
    return true;
}

std::unique_ptr<ClipVideoSource>
ClipVideoSource::open(const Asset &asset, D3D11Context &context,
                      const QString &proxyPath, QString *error)
{
    auto converters = std::make_unique<VideoConverters>();
    if (!converters->initialize(context, error))
        return nullptr;
    auto source = open(asset, context, proxyPath, converters.get(), error);
    if (source)
        source->m_ownedConverters = std::move(converters);
    return source;
}

std::unique_ptr<ClipVideoSource>
ClipVideoSource::open(const Asset &asset, D3D11Context &context,
                      const QString &proxyPath, VideoConverters *converters,
                      QString *error)
{
    if (!converters || !converters->isInitialized()) {
        setError(error, QStringLiteral("视频帧转换器尚未初始化。"));
        return nullptr;
    }

    auto source = std::unique_ptr<ClipVideoSource>(new ClipVideoSource);
    source->m_device = context.device();
    source->m_context = context.context();
    if (!source->m_device || !source->m_context) {
        setError(error, QStringLiteral("D3D11 设备不可用。"));
        return nullptr;
    }
    source->m_converters = converters;

    // ffmpeg applies the display matrix when it generates a proxy, so proxy
    // frames are already upright and their decoded size is the proxy size.
    const bool usingProxy = !proxyPath.isEmpty() && QFileInfo::exists(proxyPath);
    const QString path = usingProxy ? proxyPath : asset.path;
    const int rotation = normalizedRotation(asset.info.rotationDegrees);

    const Rational fps =
        asset.info.frameRate.isValid() ? asset.info.frameRate : Rational{30, 1};
    source->m_frameDurationUs =
        std::max<int64_t>(1, framesToMicros(1, fps));

    DecoderCreationResult created =
        createVideoDecoder(path, context, asset.info);
    if (!created.decoder) {
        setError(error, created.error.isEmpty()
                            ? QStringLiteral("无法打开视频片段。")
                            : created.error);
        return nullptr;
    }
    source->m_decoder = std::move(created.decoder);

    // The decoder's first frame supplies the real (visible/proxy) dimensions.
    // Allocating the RGBA target from media info would upscale 540p proxies to
    // the source resolution and could mis-size rotated material.
    DecodedFrame first;
    if (!source->m_decoder->decodeNext(first, error)) {
        if (!error || error->isEmpty())
            setError(error, QStringLiteral("视频片段没有可解码的帧。"));
        return nullptr;
    }
    if (first.width <= 0 || first.height <= 0) {
        setError(error, QStringLiteral("视频片段帧尺寸无效。"));
        return nullptr;
    }

    if (usingProxy) {
        source->m_displaySize = QSize(first.width, first.height);
        source->m_rotationRadians = 0.0f;
    } else {
        source->m_displaySize = (rotation == 90 || rotation == 270)
                                    ? QSize(first.height, first.width)
                                    : QSize(first.width, first.height);
        source->m_rotationRadians =
            float(-asset.info.rotationDegrees) * kPi / 180.0f;
    }

    if (!source->createConvertedTexture(error))
        return nullptr;
    if (!source->convertToRgba(first, error))
        return nullptr;

    source->m_lastPtsUs = first.ptsUs;
    source->m_hasFrame = true;
    return source;
}

bool ClipVideoSource::convertToRgba(DecodedFrame &frame, QString *error)
{
    if (!m_converters || !m_convertedRtv) {
        setError(error, QStringLiteral("RGBA 转换目标不可用。"));
        return false;
    }

    bool ok = false;
    if (frame.kind == DecodedFrame::Kind::HardwareTexture) {
        ok = m_converters->nv12()->convert(
            frame.texture, frame.subresource, frame.width, frame.height,
            m_convertedRtv.Get(), m_rotationRadians, error);
    } else {
        if (frame.rgba.empty()) {
            setError(error, QStringLiteral("软件解码帧没有像素数据。"));
            return false;
        }
        ok = m_converters->rgba()->convertRgba(
            frame.rgba, frame.width, frame.height, m_convertedRtv.Get(),
            m_rotationRadians, error);
    }
    if (!ok)
        return false;

    // The compositor only sees an RGBA texture. The decoded source frame (and
    // its CPU buffer) can be released as soon as the conversion is submitted.
    m_frame = DecodedFrame{};
    m_frame.kind = DecodedFrame::Kind::HardwareTexture;
    m_frame.texture = m_converted.Get();
    m_frame.subresource = 0;
    m_frame.width = m_displaySize.width();
    m_frame.height = m_displaySize.height();
    m_frame.ptsUs = frame.ptsUs;

    frame.rgba.clear();
    frame.rgba.shrink_to_fit();
    return true;
}

const DecodedFrame *ClipVideoSource::frameAt(int64_t sourceUs, QString *error)
{
    if (!m_decoder) {
        setError(error, QStringLiteral("视频片段尚未打开。"));
        return nullptr;
    }

    const FrameFetchPlan plan =
        planFrameFetch(m_hasFrame ? m_lastPtsUs : -1, sourceUs,
                       m_frameDurationUs);
    if (plan.action == FrameFetchAction::Reuse && m_hasFrame)
        return &m_frame;

    if (plan.action == FrameFetchAction::Seek) {
        if (!m_decoder->seek(sourceUs, error))
            return nullptr;
        // Keep the previous frame as the EOF fallback: seeking beyond the end
        // must show the last picture, not a black gap.
    }

    int drops =
        plan.action == FrameFetchAction::Decode ? plan.maxFramesToDrop : 0;
    while (true) {
        DecodedFrame frame;
        if (!m_decoder->decodeNext(frame, error)) {
            // A real decode error is propagated; only a clean EOF falls back
            // to the last decoded frame.
            if (error && !error->isEmpty())
                return nullptr;
            return m_hasFrame ? &m_frame : nullptr;
        }
        if (!convertToRgba(frame, error))
            return nullptr;

        m_lastPtsUs = frame.ptsUs;
        m_hasFrame = true;
        if (m_lastPtsUs + m_frameDurationUs > sourceUs || drops-- <= 0)
            break;
    }
    return &m_frame;
}

} // namespace VideoEditor

#endif
