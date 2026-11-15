#include "AudioTrackWriter.h"

#include <cstring>

#include <QDir>
#include <QFileInfo>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "FfmpegError.h"

namespace VideoEditor {

AudioTrackWriter::~AudioTrackWriter()
{
    if (m_oc)
        finish(nullptr);
}

bool AudioTrackWriter::open(const QString &path, int sampleRate,
                            AVCodecContext *audio, int64_t totalSamples,
                            QString *error)
{
    if (m_oc) {
        if (error)
            *error = QStringLiteral("音频分段写入器已经打开。");
        return false;
    }
    if (sampleRate <= 0 || !audio || totalSamples <= 0) {
        if (error)
            *error = QStringLiteral("音频分段缺少有效的采样率或编码器参数。");
        return false;
    }
    m_sampleRate = sampleRate;
    const QString parentDir = QFileInfo(path).dir().absolutePath();
    if (!QDir().mkpath(parentDir)) {
        if (error)
            *error = QStringLiteral("无法创建音频分段目录：%1").arg(parentDir);
        return false;
    }
    if (avformat_alloc_output_context2(&m_oc, nullptr, "mp4",
                                       path.toUtf8().constData()) < 0
        || !m_oc) {
        if (error)
            *error = QStringLiteral("无法创建音频分段封装器。");
        return false;
    }
    m_stream = avformat_new_stream(m_oc, nullptr);
    if (!m_stream) {
        if (error)
            *error = QStringLiteral("无法创建音频分段流。");
        finish(nullptr);
        return false;
    }
    m_stream->time_base = AVRational{1, sampleRate};
    if (avcodec_parameters_from_context(m_stream->codecpar, audio) < 0) {
        if (error)
            *error = QStringLiteral("无法复制 AAC 编码参数。");
        finish(nullptr);
        return false;
    }
    // The AAC priming length is part of the stream parameters: the mp4 muxer
    // uses it for the roll sample group, and the timeline length lets it trim
    // the encoder's zero-padded final frame when the file is demuxed again.
    m_stream->codecpar->initial_padding = audio->initial_padding;
    m_stream->duration = totalSamples;
    if (avio_open(&m_oc->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
        if (error)
            *error = QStringLiteral("无法创建音频分段文件：%1").arg(path);
        finish(nullptr);
        return false;
    }
    // Keep the pre-roll packet at its negative pts and let the mp4 edit list
    // represent it. make_zero would move it to sample 0 and destroy the only
    // record of where the AAC delay sits.
    AVDictionary *options = nullptr;
    av_dict_set(&options, "avoid_negative_ts", "disabled", 0);
    av_dict_set(&options, "use_editlist", "1", 0);
    const int ret = avformat_write_header(m_oc, &options);
    av_dict_free(&options);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("无法写入音频分段文件头：%1")
                         .arg(ffmpegErrorString(ret));
        finish(nullptr);
        return false;
    }
    m_packet = av_packet_alloc();
    if (!m_packet) {
        if (error)
            *error = QStringLiteral("无法分配音频分段 packet。");
        finish(nullptr);
        return false;
    }
    m_ok = true;
    return true;
}

bool AudioTrackWriter::write(const EncodedPacket &packet, QString *error)
{
    if (!m_ok || !m_packet || !m_stream) {
        if (error)
            *error = QStringLiteral("音频分段写入器尚未打开。");
        return false;
    }
    if (packet.data.empty()) {
        if (error)
            *error = QStringLiteral("音频分段收到空编码包。");
        return false;
    }
    const int size = int(packet.data.size());
    if (av_new_packet(m_packet, size) < 0) {
        if (error)
            *error = QStringLiteral("无法为音频编码包分配缓冲区。");
        return false;
    }
    memcpy(m_packet->data, packet.data.data(), size_t(size));
    m_packet->pts = packet.pts;
    m_packet->dts = packet.pts;
    m_packet->duration = packet.duration > 0 ? packet.duration : 1024;
    m_packet->stream_index = m_stream->index;
    if (packet.key)
        m_packet->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(m_packet, AVRational{1, m_sampleRate},
                         m_stream->time_base);
    const int ret = av_interleaved_write_frame(m_oc, m_packet);
    av_packet_unref(m_packet);
    if (ret < 0) {
        if (error)
            *error = ffmpegErrorString(ret);
        m_ok = false;
        return false;
    }
    return true;
}

bool AudioTrackWriter::finish(QString *error)
{
    if (!m_oc)
        return false;
    const bool wasOk = m_ok;
    int ret = 0;
    if (wasOk)
        ret = av_write_trailer(m_oc);
    if (m_oc->pb) {
        const int64_t size = avio_size(m_oc->pb);
        m_bytesWritten = size >= 0 ? size : m_oc->pb->bytes_written;
        avio_closep(&m_oc->pb);
    }
    avformat_free_context(m_oc);
    m_oc = nullptr;
    m_stream = nullptr;
    av_packet_free(&m_packet);
    m_ok = false;
    if (!wasOk)
        return false;
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("无法完成音频分段文件：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return true;
}

} // namespace VideoEditor
