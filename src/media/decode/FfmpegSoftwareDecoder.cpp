#include "FfmpegSoftwareDecoder.h"

#ifdef _WIN32

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
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

} // namespace

struct FfmpegSoftwareDecoder::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *pendingFrame = nullptr;
    bool drainSent = false;
    SwsContext *scaler = nullptr;
    int scalerWidth = 0;
    int scalerHeight = 0;
    int scalerFormat = AV_PIX_FMT_NONE;
    int videoStream = -1;
    MediaInfo info;

    ~Impl()
    {
        if (pendingFrame)
            av_frame_free(&pendingFrame);
        if (scaler)
            sws_freeContext(scaler);
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (codec)
            avcodec_free_context(&codec);
        if (format)
            avformat_close_input(&format);
    }
};

FfmpegSoftwareDecoder::FfmpegSoftwareDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

FfmpegSoftwareDecoder::~FfmpegSoftwareDecoder() = default;

std::unique_ptr<FfmpegSoftwareDecoder> FfmpegSoftwareDecoder::open(
    const QString &path, D3D11Context &, const MediaInfo &info, QString *error)
{
    auto decoder = std::unique_ptr<FfmpegSoftwareDecoder>(new FfmpegSoftwareDecoder);
    Impl &impl = *decoder->m_impl;
    impl.info = info;

    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&impl.format, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("软件解码打开文件失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }
    ret = avformat_find_stream_info(impl.format, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("软件解码读取流信息失败：%1").arg(ffmpegError(ret)));
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
    impl.codec->thread_count = 0;

    ret = avcodec_open2(impl.codec, codec, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("打开软件解码器失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.packet = av_packet_alloc();
    impl.frame = av_frame_alloc();
    if (!impl.packet || !impl.frame) {
        setError(error, QStringLiteral("无法分配解码帧。"));
        return nullptr;
    }

    return decoder;
}

bool FfmpegSoftwareDecoder::seek(int64_t ptsUs, QString *error)
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

bool FfmpegSoftwareDecoder::decodeRawFrame(AVFrame **out, QString *error)
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
            setError(error, QStringLiteral("软件解码失败：%1").arg(ffmpegError(ret)));
            return false;
        }

        int readRet = av_read_frame(impl.format, impl.packet);
        if (readRet == AVERROR_EOF) {
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

bool FfmpegSoftwareDecoder::decodeNext(DecodedFrame &out, QString *error)
{
    Impl &impl = *m_impl;

    AVFrame *frame = nullptr;
    if (impl.pendingFrame) {
        frame = impl.pendingFrame;
        impl.pendingFrame = nullptr;
    } else if (!decodeRawFrame(&frame, error)) {
        return false;
    }

    // Cache the scaler keyed by source geometry and format; SwsContext is
    // opaque and must never be inspected directly.
    if (!impl.scaler || impl.scalerWidth != frame->width
        || impl.scalerHeight != frame->height
        || impl.scalerFormat != frame->format) {
        if (impl.scaler)
            sws_freeContext(impl.scaler);
        impl.scaler = sws_getContext(
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        impl.scalerWidth = frame->width;
        impl.scalerHeight = frame->height;
        impl.scalerFormat = frame->format;
    }
    if (!impl.scaler) {
        av_frame_free(&frame);
        setError(error, QStringLiteral("无法创建像素转换器。"));
        return false;
    }

    out.kind = DecodedFrame::Kind::CpuRgba;
    out.texture = nullptr;
    out.subresource = 0;
    out.width = frame->width;
    out.height = frame->height;
    out.rgba.resize(size_t(out.width) * size_t(out.height) * 4);

    uint8_t *destination[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
    int destinationStride[4] = {out.width * 4, 0, 0, 0};
    sws_scale(impl.scaler, frame->data, frame->linesize, 0, frame->height,
              destination, destinationStride);

    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
        timestamp = frame->pts;
    out.ptsUs = timestamp == AV_NOPTS_VALUE
        ? 0
        : av_rescale_q(timestamp, impl.format->streams[impl.videoStream]->time_base,
                       AV_TIME_BASE_Q);

    if (out.avFrame) {
        av_frame_free(&out.avFrame);
        out.avFrame = nullptr;
    }
    av_frame_free(&frame);
    return true;
}

const MediaInfo &FfmpegSoftwareDecoder::info() const
{
    return m_impl->info;
}

} // namespace VideoEditor

#endif
