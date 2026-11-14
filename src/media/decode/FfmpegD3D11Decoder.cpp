#include "FfmpegD3D11Decoder.h"

#ifdef _WIN32

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include "D3D11Context.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString ffmpegError(int value)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

AVPixelFormat selectD3D11Format(AVCodecContext *, const AVPixelFormat *formats)
{
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_D3D11)
            return *format;
    }
    return AV_PIX_FMT_NONE;
}

// The hardware conversion shader only consumes 8-bit NV12. Reject anything
// deeper up front so the factory can fall back to software decoding.
bool isEightBitFormat(AVPixelFormat format)
{
    if (format == AV_PIX_FMT_NONE)
        return true; // unknown: let the decoder and frames check decide
    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor)
        return true;
    return descriptor->comp[0].depth <= 8;
}

// Some codecs (for example the MJPEG used by preview proxies) have no D3D11VA
// hardware decoder at all. Opening one would succeed but the first packet
// would fail with EINVAL from get_format, so reject it here and let the
// factory fall back to software decoding.
bool hasD3D11Decoder(const AVCodec *codec)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config)
            return false;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            && config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
            return true;
        }
    }
}

} // namespace

struct FfmpegD3D11Decoder::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVBufferRef *hardwareDevice = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    // Set by seek(): the first decoded frame at or after the requested time,
    // handed out by the next decodeNext() call.
    AVFrame *pendingFrame = nullptr;
    bool drainSent = false;
    int videoStream = -1;
    MediaInfo info;

    ~Impl()
    {
        if (pendingFrame)
            av_frame_free(&pendingFrame);
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (codec)
            avcodec_free_context(&codec);
        if (hardwareDevice)
            av_buffer_unref(&hardwareDevice);
        if (format)
            avformat_close_input(&format);
    }
};

FfmpegD3D11Decoder::FfmpegD3D11Decoder()
    : m_impl(std::make_unique<Impl>())
{
}

FfmpegD3D11Decoder::~FfmpegD3D11Decoder() = default;

std::unique_ptr<FfmpegD3D11Decoder> FfmpegD3D11Decoder::open(
    const QString &path, D3D11Context &context, const MediaInfo &info,
    QString *error)
{
    auto decoder = std::unique_ptr<FfmpegD3D11Decoder>(new FfmpegD3D11Decoder);
    Impl &impl = *decoder->m_impl;
    impl.info = info;

    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&impl.format, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("硬件解码打开文件失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }
    ret = avformat_find_stream_info(impl.format, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("硬件解码读取流信息失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.videoStream = av_find_best_stream(impl.format, AVMEDIA_TYPE_VIDEO,
                                           -1, -1, nullptr, 0);
    if (impl.videoStream < 0) {
        setError(error, QStringLiteral("文件不包含视频流。"));
        return nullptr;
    }

    const AVCodec *codec = avcodec_find_decoder(
        impl.format->streams[impl.videoStream]->codecpar->codec_id);
    if (!codec) {
        setError(error, QStringLiteral("FFmpeg 不支持该视频编码。"));
        return nullptr;
    }
    if (!hasD3D11Decoder(codec)) {
        setError(error, QStringLiteral("该编码没有 D3D11 硬件解码器，已回退软件解码。"));
        return nullptr;
    }

    // Reject 10-bit sources before touching the GPU: P010 is not representable
    // by the NV12 shader path.
    const auto streamFormat = static_cast<AVPixelFormat>(
        impl.format->streams[impl.videoStream]->codecpar->format);
    if (!isEightBitFormat(streamFormat)) {
        setError(error, QStringLiteral("硬件路径暂只支持 NV12 8-bit，已回退软件解码。"));
        return nullptr;
    }

    impl.hardwareDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!impl.hardwareDevice) {
        setError(error, QStringLiteral("无法分配 D3D11VA 设备。"));
        return nullptr;
    }

    // AVBufferRef::data points at AVHWDeviceContext; the type-specific
    // AVD3D11VADeviceContext lives in its hwctx field.
    auto *hwDeviceContext =
        reinterpret_cast<AVHWDeviceContext *>(impl.hardwareDevice->data);
    auto *deviceContext =
        reinterpret_cast<AVD3D11VADeviceContext *>(hwDeviceContext->hwctx);
    deviceContext->device = context.device();
    deviceContext->device->AddRef();
    deviceContext->device_context = context.context();
    deviceContext->device_context->AddRef();

    ret = av_hwdevice_ctx_init(impl.hardwareDevice);
    if (ret < 0) {
        setError(error, QStringLiteral("初始化 D3D11VA 设备失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.codec = avcodec_alloc_context3(codec);
    if (!impl.codec) {
        setError(error, QStringLiteral("无法分配解码上下文。"));
        return nullptr;
    }

    ret = avcodec_parameters_to_context(
        impl.codec, impl.format->streams[impl.videoStream]->codecpar);
    if (ret < 0) {
        setError(error, QStringLiteral("复制解码参数失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.codec->pkt_timebase = impl.format->streams[impl.videoStream]->time_base;
    impl.codec->hw_device_ctx = av_buffer_ref(impl.hardwareDevice);
    impl.codec->get_format = selectD3D11Format;
    impl.codec->thread_count = 1;

    ret = avcodec_open2(impl.codec, codec, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("打开硬件解码器失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    if (impl.codec->hw_frames_ctx) {
        const auto *frames = reinterpret_cast<AVHWFramesContext *>(
            impl.codec->hw_frames_ctx->data);
        if (frames->sw_format != AV_PIX_FMT_NV12) {
            setError(error, QStringLiteral("硬件路径暂只支持 NV12 8-bit，已回退软件解码。"));
            return nullptr;
        }
    }

    impl.packet = av_packet_alloc();
    impl.frame = av_frame_alloc();
    if (!impl.packet || !impl.frame) {
        setError(error, QStringLiteral("无法分配解码帧。"));
        return nullptr;
    }

    return decoder;
}

bool FfmpegD3D11Decoder::seek(int64_t ptsUs, QString *error)
{
    Impl &impl = *m_impl;
    if (impl.pendingFrame) {
        av_frame_free(&impl.pendingFrame);
        impl.pendingFrame = nullptr;
    }

    const int64_t timestamp = av_rescale_q(ptsUs, AV_TIME_BASE_Q,
                                           impl.format->streams[impl.videoStream]->time_base);
    const int ret = av_seek_frame(impl.format, impl.videoStream, timestamp,
                                  AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        setError(error, QStringLiteral("定位失败：%1").arg(ffmpegError(ret)));
        return false;
    }
    avcodec_flush_buffers(impl.codec);
    impl.drainSent = false;

    // av_seek_frame() lands on the keyframe at or before the target. Decode
    // forward and discard frames until the requested presentation time so the
    // next decodeNext() returns the frame the caller asked for.
    while (true) {
        AVFrame *frame = nullptr;
        if (!decodeRawFrame(&frame, error))
            return true; // EOF before the target: decodeNext() reports the end

        int64_t frameTimestamp = frame->best_effort_timestamp;
        if (frameTimestamp == AV_NOPTS_VALUE)
            frameTimestamp = frame->pts;
        const int64_t framePtsUs = frameTimestamp == AV_NOPTS_VALUE
            ? 0
            : av_rescale_q(frameTimestamp,
                           impl.format->streams[impl.videoStream]->time_base,
                           AV_TIME_BASE_Q);
        if (framePtsUs >= ptsUs) {
            impl.pendingFrame = frame;
            return true;
        }
        av_frame_free(&frame);
    }
}

bool FfmpegD3D11Decoder::decodeRawFrame(AVFrame **out, QString *error)
{
    Impl &impl = *m_impl;
    if (!impl.frame) {
        setError(error, QStringLiteral("解码帧缓冲区不可用。"));
        return false;
    }

    while (true) {
        int ret = avcodec_receive_frame(impl.codec, impl.frame);
        if (ret == 0) {
            *out = impl.frame;
            impl.frame = av_frame_alloc();
            if (!impl.frame) {
                av_frame_free(out);
                setError(error, QStringLiteral("无法分配下一帧。"));
                return false;
            }
            return true;
        }

        if (ret == AVERROR_EOF)
            return false;

        if (ret != AVERROR(EAGAIN)) {
            setError(error, QStringLiteral("硬件解码失败：%1").arg(ffmpegError(ret)));
            return false;
        }

        int readRet = av_read_frame(impl.format, impl.packet);
        if (readRet == AVERROR_EOF) {
            // Flush the decoder once; a second EOF means the stream is over.
            if (impl.drainSent)
                return false;
            avcodec_send_packet(impl.codec, nullptr);
            impl.drainSent = true;
            continue;
        }
        if (readRet < 0) {
            setError(error, QStringLiteral("读取视频包失败：%1").arg(ffmpegError(readRet)));
            return false;
        }

        if (impl.packet->stream_index == impl.videoStream) {
            const int sendRet = avcodec_send_packet(impl.codec, impl.packet);
            av_packet_unref(impl.packet);
            if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                setError(error, QStringLiteral("发送视频包失败：%1").arg(ffmpegError(sendRet)));
                return false;
            }
        } else {
            av_packet_unref(impl.packet);
        }
    }
}

bool FfmpegD3D11Decoder::decodeNext(DecodedFrame &out, QString *error)
{
    Impl &impl = *m_impl;

    AVFrame *frame = nullptr;
    if (impl.pendingFrame) {
        frame = impl.pendingFrame;
        impl.pendingFrame = nullptr;
    } else if (!decodeRawFrame(&frame, error)) {
        return false;
    }

    if (frame->format != AV_PIX_FMT_D3D11) {
        av_frame_free(&frame);
        setError(error, QStringLiteral("硬件解码器未返回 D3D11 帧。"));
        return false;
    }
    if (frame->hw_frames_ctx) {
        const auto *frames = reinterpret_cast<AVHWFramesContext *>(
            frame->hw_frames_ctx->data);
        if (frames->sw_format != AV_PIX_FMT_NV12) {
            av_frame_free(&frame);
            setError(error, QStringLiteral("硬件路径暂只支持 NV12 8-bit。"));
            return false;
        }
    }

    out.kind = DecodedFrame::Kind::HardwareTexture;
    out.texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
    out.subresource = static_cast<UINT>(
        reinterpret_cast<uintptr_t>(frame->data[1]));
    out.width = frame->width;
    out.height = frame->height;
    out.rgba.clear();

    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
        timestamp = frame->pts;
    out.ptsUs = timestamp == AV_NOPTS_VALUE
        ? 0
        : av_rescale_q(timestamp, impl.format->streams[impl.videoStream]->time_base,
                       AV_TIME_BASE_Q);

    if (out.avFrame)
        av_frame_free(&out.avFrame);
    out.avFrame = frame;
    return true;
}

const MediaInfo &FfmpegD3D11Decoder::info() const
{
    return m_impl->info;
}

} // namespace VideoEditor

#endif
