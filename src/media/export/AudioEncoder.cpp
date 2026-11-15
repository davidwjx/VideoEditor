#include "AudioEncoder.h"

#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include "FfmpegError.h"

namespace VideoEditor {

AudioEncoder::~AudioEncoder()
{
    close();
}

bool AudioEncoder::open(int sampleRate, int channels, int bitrateKbps,
                        QString *error)
{
    close();
    if (error)
        error->clear();

    if (sampleRate <= 0 || channels <= 0 || bitrateKbps <= 0) {
        if (error)
            *error = QStringLiteral("音频编码要求有效的采样率、声道数和码率。");
        return false;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    if (!codec) {
        if (error)
            *error = QStringLiteral("当前 FFmpeg 构建不包含 AAC 编码器。");
        return false;
    }
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
        if (error)
            *error = QStringLiteral("无法分配音频编码器上下文。");
        return false;
    }
    m_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_ctx->sample_rate = sampleRate;
    av_channel_layout_default(&m_ctx->ch_layout, channels);
    m_ctx->bit_rate = int64_t(bitrateKbps) * 1000;
    m_ctx->profile = AV_PROFILE_AAC_LOW;
    m_ctx->time_base = AVRational{1, sampleRate};
    // Global header keeps the AudioSpecificConfig for the MP4 muxer in Task 6.
    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("AAC 编码器打开失败：%1")
                         .arg(ffmpegErrorString(ret));
        close();
        return false;
    }

    m_frameSize = m_ctx->frame_size > 0 ? m_ctx->frame_size : 1024;
    m_frame = av_frame_alloc();
    if (!m_frame) {
        if (error)
            *error = QStringLiteral("无法分配音频帧。");
        close();
        return false;
    }
    m_frame->format = AV_SAMPLE_FMT_FLTP;
    m_frame->sample_rate = sampleRate;
    av_channel_layout_copy(&m_frame->ch_layout, &m_ctx->ch_layout);
    m_frame->nb_samples = m_frameSize;
    ret = av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("无法为音频帧分配缓冲区：%1")
                         .arg(ffmpegErrorString(ret));
        close();
        return false;
    }

    m_packet = av_packet_alloc();
    if (!m_packet) {
        if (error)
            *error = QStringLiteral("无法分配音频 packet。");
        close();
        return false;
    }
    m_channels = channels;
    m_delay = m_ctx->initial_padding;
    return true;
}

bool AudioEncoder::encode(const float *interleaved, int frames,
                          int64_t sampleIndex,
                          std::vector<EncodedPacket> *packets, QString *error)
{
    if (error)
        error->clear();
    if (!m_ctx || !m_frame || !m_packet || !interleaved || !packets) {
        if (error)
            *error = QStringLiteral("音频编码器尚未打开。");
        return false;
    }
    if (frames != m_frameSize) {
        if (error)
            *error = QStringLiteral("音频帧必须为 %1 个采样，实际为 %2。")
                         .arg(m_frameSize)
                         .arg(frames);
        return false;
    }
    if (av_frame_make_writable(m_frame) < 0) {
        if (error)
            *error = QStringLiteral("无法写入音频帧缓冲区。");
        return false;
    }

    // Deinterleave into the encoder's planar FLTP layout.
    for (int c = 0; c < m_channels; ++c) {
        float *plane = reinterpret_cast<float *>(m_frame->data[c]);
        for (int i = 0; i < frames; ++i)
            plane[i] = interleaved[size_t(i) * size_t(m_channels) + size_t(c)];
    }
    m_frame->pts = sampleIndex;

    const int ret = avcodec_send_frame(m_ctx, m_frame);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("提交音频帧失败：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return receivePackets(packets, error);
}

bool AudioEncoder::receivePackets(std::vector<EncodedPacket> *packets,
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
                *error = QStringLiteral("读取音频编码 packet 失败：%1")
                             .arg(ffmpegErrorString(ret));
            return false;
        }
        // libavcodec prepends one silent pre-roll packet at
        // pts == -initial_padding. The container signals that priming through
        // skip_samples (encoderDelaySamples()), so only timeline packets from
        // sample 0 onwards are exposed to the muxer.
        if (m_packet->pts != AV_NOPTS_VALUE && m_packet->pts < 0) {
            av_packet_unref(m_packet);
            continue;
        }
        EncodedPacket packet;
        packet.data.assign(m_packet->data, m_packet->data + m_packet->size);
        packet.pts = m_packet->pts;
        packet.dts = m_packet->dts == AV_NOPTS_VALUE ? m_packet->pts
                                                     : m_packet->dts;
        // Native AAC normally reports 1024; fall back so callers always get a
        // usable duration.
        packet.duration = m_packet->duration > 0 ? m_packet->duration
                                                 : m_frameSize;
        packet.key = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        packets->push_back(std::move(packet));
        av_packet_unref(m_packet);
    }
}

bool AudioEncoder::flush(std::vector<EncodedPacket> *packets, QString *error)
{
    if (error)
        error->clear();
    if (!m_ctx)
        return true;
    if (!packets) {
        if (error)
            *error = QStringLiteral("请提供用于接收音频 packet 的容器。");
        return false;
    }
    const int ret = avcodec_send_frame(m_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        if (error)
            *error = QStringLiteral("刷新音频编码器失败：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return receivePackets(packets, error);
}

void AudioEncoder::close()
{
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_ctx)
        avcodec_free_context(&m_ctx);
    m_channels = 0;
    m_frameSize = 0;
    m_delay = 0;
}

} // namespace VideoEditor
