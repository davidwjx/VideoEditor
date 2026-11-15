#include "SegmentWriter.h"

#include <cstring>

#include <QDir>
#include <QFileInfo>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "FfmpegError.h"

namespace VideoEditor {

SegmentWriter::~SegmentWriter()
{
    if (m_oc)
        finish(nullptr);
}

bool SegmentWriter::open(const QString &path, const Rational &fps,
                         AVCodecContext *video, int64_t rebaseFrame,
                         QString *error)
{
    if (m_oc) {
        if (error)
            *error = QStringLiteral("视频分段写入器已经打开。");
        return false;
    }
    if (!fps.isValid() || !video) {
        if (error)
            *error = QStringLiteral("视频分段缺少有效的帧率或编码器参数。");
        return false;
    }

    m_fps = fps;
    m_rebaseFrame = rebaseFrame;
    const QString parentDir = QFileInfo(path).dir().absolutePath();
    if (!QDir().mkpath(parentDir)) {
        if (error)
            *error = QStringLiteral("无法创建分段目录：%1").arg(parentDir);
        return false;
    }
    if (avformat_alloc_output_context2(&m_oc, nullptr, "mp4",
                                       path.toUtf8().constData()) < 0
        || !m_oc) {
        if (error)
            *error = QStringLiteral("无法创建 MP4 分段封装器。");
        return false;
    }
    m_video = avformat_new_stream(m_oc, nullptr);
    if (!m_video) {
        if (error)
            *error = QStringLiteral("无法创建分段视频流。");
        finish(nullptr);
        return false;
    }
    m_video->time_base = AVRational{fps.den, fps.num};
    m_video->avg_frame_rate = AVRational{fps.num, fps.den};
    if (avcodec_parameters_from_context(m_video->codecpar, video) < 0) {
        if (error)
            *error = QStringLiteral("无法复制编码参数到分段。");
        finish(nullptr);
        return false;
    }
    if (video->codec_id == AV_CODEC_ID_HEVC)
        m_video->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    if (avio_open(&m_oc->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
        if (error)
            *error = QStringLiteral("无法创建分段文件：%1").arg(path);
        finish(nullptr);
        return false;
    }
    AVDictionary *options = nullptr;
    av_dict_set(&options, "avoid_negative_ts", "make_zero", 0);
    av_dict_set(&options, "use_editlist", "0", 0);
    const int ret = avformat_write_header(m_oc, &options);
    av_dict_free(&options);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("无法写入分段文件头：%1")
                         .arg(ffmpegErrorString(ret));
        finish(nullptr);
        return false;
    }

    m_packet = av_packet_alloc();
    if (!m_packet) {
        if (error)
            *error = QStringLiteral("无法分配分段 packet。");
        finish(nullptr);
        return false;
    }
    // The muxer may pick a finer time base (mp4 normally uses 1/(fps * 512)).
    // Requested frames are rescaled from the encoder time base explicitly, so
    // anchor one frame in the stream time base now that the header is written.
    m_frameDuration = av_rescale_q(1, AVRational{fps.den, fps.num},
                                   m_video->time_base);
    if (m_frameDuration <= 0)
        m_frameDuration = 1;
    m_ok = true;
    return true;
}

bool SegmentWriter::writeVideo(const EncodedPacket &packet, QString *error)
{
    if (!m_ok || !m_packet || !m_video) {
        if (error)
            *error = QStringLiteral("视频分段写入器尚未打开。");
        return false;
    }
    if (packet.data.empty()) {
        if (error)
            *error = QStringLiteral("视频分段收到空编码包。");
        return false;
    }
    if (packet.pts < m_rebaseFrame) {
        if (error)
            *error = QStringLiteral("视频分段帧号 %1 早于分段起点 %2。")
                         .arg(packet.pts).arg(m_rebaseFrame);
        return false;
    }

    const int size = int(packet.data.size());
    if (av_new_packet(m_packet, size) < 0) {
        if (error)
            *error = QStringLiteral("无法为分段编码包分配缓冲区。");
        return false;
    }
    memcpy(m_packet->data, packet.data.data(), size_t(size));
    const AVRational frameTb{m_fps.den, m_fps.num};
    const int64_t localPts =
        av_rescale_q(packet.pts - m_rebaseFrame, frameTb, m_video->time_base);
    m_packet->pts = localPts;
    m_packet->dts = localPts;
    m_packet->duration = m_frameDuration;
    m_packet->stream_index = m_video->index;
    if (packet.key)
        m_packet->flags |= AV_PKT_FLAG_KEY;

    const int ret = av_interleaved_write_frame(m_oc, m_packet);
    av_packet_unref(m_packet);
    if (ret < 0) {
        if (error)
            *error = ffmpegErrorString(ret);
        m_ok = false;
        return false;
    }
    ++m_videoPackets;
    return true;
}

bool SegmentWriter::finish(QString *error)
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
    m_video = nullptr;
    av_packet_free(&m_packet);
    m_ok = false;
    if (!wasOk)
        return false;
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("无法完成分段文件：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return true;
}

} // namespace VideoEditor
