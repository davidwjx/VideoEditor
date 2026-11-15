#include "VideoEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <QByteArray>

#include "ExportEncoderCatalog.h"
#include "FfmpegError.h"

namespace VideoEditor {

namespace {

void fillPlane(uint8_t *data, int linesize, int planeHeight, uint8_t value)
{
    if (!data || linesize <= 0 || planeHeight <= 0)
        return;
    for (int y = 0; y < planeHeight; ++y)
        memset(data + int64_t(y) * linesize, value, size_t(linesize));
}

} // namespace

VideoEncoder::~VideoEncoder()
{
    close();
}

bool VideoEncoder::open(const EncoderDescriptor &encoder,
                        const ExportSettings &settings, QString *error)
{
    close();
    if (error)
        error->clear();

    if (settings.width <= 0 || settings.height <= 0
        || settings.width % 2 != 0 || settings.height % 2 != 0) {
        if (error)
            *error = QStringLiteral("视频编码要求偶数宽高。");
        return false;
    }
    if (!settings.fps.isValid()) {
        if (error)
            *error = QStringLiteral("视频编码要求有效的帧率。");
        return false;
    }

    const QByteArray id = encoder.id.toUtf8();
    const AVCodec *codec = avcodec_find_encoder_by_name(id.constData());
    if (!codec) {
        if (error)
            *error = QStringLiteral("当前 FFmpeg 构建不包含编码器 %1。")
                         .arg(encoder.id);
        return false;
    }
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
        if (error)
            *error = QStringLiteral("无法分配视频编码器上下文。");
        return false;
    }
    m_ctx->width = settings.width;
    m_ctx->height = settings.height;
    m_ctx->time_base = AVRational{settings.fps.den, settings.fps.num};
    m_ctx->framerate = AVRational{settings.fps.num, settings.fps.den};
    m_pixelFormat = encoderPixelFormat(encoder);
    m_ctx->pix_fmt = m_pixelFormat;
    m_ctx->bit_rate = int64_t(effectiveBitrateKbps(settings)) * 1000;
    m_ctx->gop_size = std::max(1, int(std::lround(settings.fps.toDouble())));
    m_ctx->max_b_frames = 0;
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    m_ctx->color_range = AVCOL_RANGE_MPEG;
    m_ctx->colorspace = AVCOL_SPC_BT709;
    m_ctx->color_primaries = AVCOL_PRI_BT709;
    m_ctx->color_trc = AVCOL_TRC_BT709;
    applyEncoderOptions(m_ctx, encoder);

    int ret = avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("编码器 %1 打开失败：%2")
                         .arg(encoder.id, ffmpegErrorString(ret));
        close();
        return false;
    }
    // Encoders may negotiate a different format; trust the context.
    m_pixelFormat = m_ctx->pix_fmt;

    m_frame = av_frame_alloc();
    if (!m_frame) {
        if (error)
            *error = QStringLiteral("无法分配视频帧。");
        close();
        return false;
    }
    m_frame->format = m_ctx->pix_fmt;
    m_frame->width = settings.width;
    m_frame->height = settings.height;
    m_frame->color_range = AVCOL_RANGE_MPEG;
    m_frame->colorspace = AVCOL_SPC_BT709;
    m_frame->color_primaries = AVCOL_PRI_BT709;
    m_frame->color_trc = AVCOL_TRC_BT709;
    ret = av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("无法为视频帧分配缓冲区：%1")
                         .arg(ffmpegErrorString(ret));
        close();
        return false;
    }
    m_packet = av_packet_alloc();
    if (!m_packet) {
        if (error)
            *error = QStringLiteral("无法分配视频 packet。");
        close();
        return false;
    }
    m_sws = sws_getContext(settings.width, settings.height, AV_PIX_FMT_RGBA,
                           settings.width, settings.height, m_ctx->pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        if (error)
            *error = QStringLiteral("无法创建 RGBA 到 YUV 的颜色转换器。");
        close();
        return false;
    }
    sws_setColorspaceDetails(m_sws, sws_getCoefficients(SWS_CS_ITU709), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0,
                             1 << 16, 1 << 16);
    m_width = settings.width;
    m_height = settings.height;
    m_hardware = encoder.hardware;
    return true;
}

bool VideoEncoder::encodeRgba(const uint8_t *rgba, int64_t frameIndex,
                              bool forceKeyframe,
                              std::vector<EncodedPacket> *packets,
                              QString *error)
{
    if (!m_ctx || !m_frame || !rgba || !packets)
        return false;
    if (av_frame_make_writable(m_frame) < 0) {
        if (error)
            *error = QStringLiteral("无法写入视频帧缓冲区。");
        return false;
    }
    const uint8_t *srcData[4] = {rgba, nullptr, nullptr, nullptr};
    const int srcStride[4] = {m_width * 4, 0, 0, 0};
    sws_scale(m_sws, srcData, srcStride, 0, m_height, m_frame->data,
              m_frame->linesize);
    return sendFrame(forceKeyframe, frameIndex, packets, error);
}

bool VideoEncoder::encodeBlack(int64_t frameIndex, bool forceKeyframe,
                               std::vector<EncodedPacket> *packets,
                               QString *error)
{
    if (!m_ctx || !m_frame || !packets)
        return false;
    if (av_frame_make_writable(m_frame) < 0) {
        if (error)
            *error = QStringLiteral("无法写入视频帧缓冲区。");
        return false;
    }
    fillBlackFrame();
    return sendFrame(forceKeyframe, frameIndex, packets, error);
}

bool VideoEncoder::sendFrame(bool forceKeyframe, int64_t frameIndex,
                             std::vector<EncodedPacket> *packets,
                             QString *error)
{
    m_frame->pts = frameIndex;
    // One frame in encoder time base; some encoders copy it to the packet.
    m_frame->duration = 1;
    m_frame->pict_type
        = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    const int ret = avcodec_send_frame(m_ctx, m_frame);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("提交视频帧失败：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return receivePackets(packets, error);
}

void VideoEncoder::fillBlackFrame()
{
    const int chromaHeight = (m_height + 1) / 2;
    fillPlane(m_frame->data[0], m_frame->linesize[0], m_height, 16);
    fillPlane(m_frame->data[1], m_frame->linesize[1], chromaHeight, 128);
    fillPlane(m_frame->data[2], m_frame->linesize[2], chromaHeight, 128);
}

bool VideoEncoder::receivePackets(std::vector<EncodedPacket> *packets,
                                  QString *error)
{
    if (!m_ctx || !m_packet || !packets)
        return false;
    while (true) {
        const int ret = avcodec_receive_packet(m_ctx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return true;
        if (ret < 0) {
            if (error)
                *error = QStringLiteral("读取编码 packet 失败：%1")
                             .arg(ffmpegErrorString(ret));
            return false;
        }
        EncodedPacket packet;
        packet.data.assign(m_packet->data, m_packet->data + m_packet->size);
        packet.pts = m_packet->pts;
        packet.dts = m_packet->dts == AV_NOPTS_VALUE ? m_packet->pts
                                                     : m_packet->dts;
        packet.duration = m_packet->duration > 0 ? m_packet->duration : 1;
        packet.key = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        packets->push_back(std::move(packet));
        av_packet_unref(m_packet);
    }
}

bool VideoEncoder::flush(std::vector<EncodedPacket> *packets, QString *error)
{
    if (!m_ctx)
        return true;
    if (!packets) {
        if (error)
            *error = QStringLiteral("请提供用于接收编码 packet 的容器。");
        return false;
    }
    const int ret = avcodec_send_frame(m_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        if (error)
            *error = QStringLiteral("刷新视频编码器失败：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return receivePackets(packets, error);
}

void VideoEncoder::close()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_ctx)
        avcodec_free_context(&m_ctx);
    m_pixelFormat = AV_PIX_FMT_NONE;
    m_width = 0;
    m_height = 0;
    m_hardware = false;
}

} // namespace VideoEditor
