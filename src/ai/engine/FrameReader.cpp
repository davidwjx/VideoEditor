#include "FrameReader.h"

#include <algorithm>
#include <cmath>

#include <QTransform>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace VideoEditor {

namespace {

QString ffmpegError(int value)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

const AVPacketSideData *findDisplayMatrix(AVStream *stream)
{
    if (auto *side = av_packet_side_data_get(
            stream->codecpar->coded_side_data,
            stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
        return side;
    }
    return av_packet_side_data_get(stream->side_data, stream->nb_side_data,
                                   AV_PKT_DATA_DISPLAYMATRIX);
}

int rotationFromStream(AVStream *stream)
{
    const AVPacketSideData *side = findDisplayMatrix(stream);
    if (!side || !side->data || side->size < 9 * sizeof(int32_t))
        return 0;
    const double counterClockwise = av_display_rotation_get(
        reinterpret_cast<const int32_t *>(side->data));
    if (!std::isfinite(counterClockwise))
        return 0;
    // Convert to a clockwise angle to match displaySizeForAsset().
    int normalized = static_cast<int>(std::llround(-counterClockwise)) % 360;
    if (normalized < 0)
        normalized += 360;
    // Only right-angle rotations change the display geometry; round to the
    // nearest quarter turn so a slightly noisy matrix cannot skew the mask.
    normalized = ((normalized + 45) / 90 * 90) % 360;
    return normalized;
}

} // namespace

struct FrameReader::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVPacket *pendingPacket = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *scaler = nullptr;
    int scalerWidth = 0;
    int scalerHeight = 0;
    int scalerFormat = AV_PIX_FMT_NONE;
    int videoStream = -1;
    int rotation = 0;
    int frameCount = 0;
    int nextIndex = 0;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    int64_t discardBeforeUs = -1;
    Rational fps;
    QSize displaySize;
    bool drainSent = false;
    bool eof = false;
    QString lastError;

    ~Impl() { cleanup(); }

    void cleanup()
    {
        if (scaler)
            sws_freeContext(scaler);
        scaler = nullptr;
        if (frame)
            av_frame_free(&frame);
        if (pendingPacket)
            av_packet_free(&pendingPacket);
        if (packet)
            av_packet_free(&packet);
        if (codec)
            avcodec_free_context(&codec);
        if (format)
            avformat_close_input(&format);
        videoStream = -1;
    }

    void setError(const QString &message) { lastError = message; }

    int64_t framePtsUs() const
    {
        int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE)
            timestamp = frame->pts;
        if (timestamp == AV_NOPTS_VALUE)
            return 0;
        return av_rescale_q(timestamp, format->streams[videoStream]->time_base,
                            AV_TIME_BASE_Q);
    }

    bool seekTo(int64_t ptsUs)
    {
        if (discardBeforeUs >= 0)
            discardBeforeUs = -1;
        if (pendingPacket)
            av_packet_free(&pendingPacket);
        const int64_t timestamp =
            av_rescale_q(ptsUs, AV_TIME_BASE_Q,
                         format->streams[videoStream]->time_base);
        const int ret = av_seek_frame(format, videoStream, timestamp,
                                      AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            setError(QStringLiteral("视频定位失败：%1").arg(ffmpegError(ret)));
            return false;
        }
        avcodec_flush_buffers(codec);
        drainSent = false;
        eof = false;
        return true;
    }

    bool decodeRaw()
    {
        int stalledRetries = 0;
        while (true) {
            const int ret = avcodec_receive_frame(codec, frame);
            if (ret == 0)
                return true;
            if (ret == AVERROR_EOF) {
                eof = true;
                return false;
            }
            if (ret != AVERROR(EAGAIN)) {
                setError(QStringLiteral("视频解码失败：%1").arg(ffmpegError(ret)));
                return false;
            }

            // A packet rejected with EAGAIN must be retried after draining the
            // decoder, never dropped. Keep it across decodeRaw() calls.
            if (pendingPacket) {
                const int sendRet = avcodec_send_packet(codec, pendingPacket);
                if (sendRet == 0) {
                    av_packet_free(&pendingPacket);
                    stalledRetries = 0;
                    continue;
                }
                if (sendRet == AVERROR(EAGAIN)) {
                    if (++stalledRetries > 1000) {
                        setError(QStringLiteral("视频解码器停滞，无法发送"
                                                "待处理视频包。"));
                        return false;
                    }
                    continue;
                }
                setError(QStringLiteral("发送视频包失败：%1")
                             .arg(ffmpegError(sendRet)));
                return false;
            }
            stalledRetries = 0;

            const int readRet = av_read_frame(format, packet);
            if (readRet == AVERROR_EOF) {
                if (drainSent) {
                    eof = true;
                    return false;
                }
                avcodec_send_packet(codec, nullptr);
                drainSent = true;
                continue;
            }
            if (readRet < 0) {
                setError(QStringLiteral("读取视频包失败：%1")
                             .arg(ffmpegError(readRet)));
                return false;
            }
            if (packet->stream_index == videoStream) {
                const int sendRet = avcodec_send_packet(codec, packet);
                if (sendRet == 0) {
                    av_packet_unref(packet);
                } else if (sendRet == AVERROR(EAGAIN)) {
                    if (!pendingPacket)
                        pendingPacket = av_packet_alloc();
                    if (!pendingPacket) {
                        av_packet_unref(packet);
                        setError(QStringLiteral("无法分配待发送视频包。"));
                        return false;
                    }
                    av_packet_move_ref(pendingPacket, packet);
                } else {
                    av_packet_unref(packet);
                    setError(QStringLiteral("发送视频包失败：%1")
                                 .arg(ffmpegError(sendRet)));
                    return false;
                }
            } else {
                av_packet_unref(packet);
            }
        }
    }

    QImage convertToDisplay()
    {
        if (frame->width <= 0 || frame->height <= 0) {
            setError(QStringLiteral("解码帧尺寸无效。"));
            return {};
        }
        if (!scaler || scalerWidth != frame->width
            || scalerHeight != frame->height
            || scalerFormat != frame->format) {
            if (scaler)
                sws_freeContext(scaler);
            scaler = sws_getContext(
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), frame->width,
                frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                nullptr);
            scalerWidth = frame->width;
            scalerHeight = frame->height;
            scalerFormat = frame->format;
        }
        if (!scaler) {
            setError(QStringLiteral("无法创建视频像素转换器。"));
            return {};
        }

        QImage source(frame->width, frame->height, QImage::Format_ARGB32);
        if (source.isNull()) {
            setError(QStringLiteral("无法分配视频帧内存。"));
            return {};
        }
        uint8_t *destination[4] = {source.bits(), nullptr, nullptr, nullptr};
        int destinationStride[4] = {int(source.bytesPerLine()), 0, 0, 0};
        sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
                  destination, destinationStride);

        QImage oriented = source;
        if (rotation == 90) {
            oriented = source.transformed(QTransform().rotate(90),
                                          Qt::FastTransformation);
        } else if (rotation == 180) {
            oriented = source.transformed(QTransform().rotate(180),
                                          Qt::FastTransformation);
        } else if (rotation == 270) {
            oriented = source.transformed(QTransform().rotate(270),
                                          Qt::FastTransformation);
        }
        if (oriented.size() != displaySize) {
            oriented = oriented.scaled(displaySize, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
        }
        return oriented.convertToFormat(QImage::Format_RGB32);
    }

    // Decodes the next frame, discarding anything the current seek target
    // asked us to skip. Returns the display image.
    bool advance(QImage *out)
    {
        while (true) {
            if (!decodeRaw())
                return false;
            const int64_t pts = framePtsUs();
            if (discardBeforeUs >= 0
                && pts + framesToMicros(1, fps) / 2 < discardBeforeUs) {
                av_frame_unref(frame);
                continue;
            }
            discardBeforeUs = -1;
            QImage image = convertToDisplay();
            av_frame_unref(frame);
            if (image.isNull()) {
                if (lastError.isEmpty())
                    setError(QStringLiteral("视频帧转换失败。"));
                return false;
            }
            *out = std::move(image);
            return true;
        }
    }
};

FrameReader::FrameReader()
    : m_impl(std::make_unique<Impl>())
{
}

FrameReader::~FrameReader() = default;

bool FrameReader::open(const QString &path, Rational fps, int64_t sourceInUs,
                       int64_t sourceOutUs, QSize displaySize, QString *error)
{
    close();
    Impl &impl = *m_impl;

    if (!fps.isValid()) {
        impl.setError(QStringLiteral("视频帧率无效。"));
        if (error)
            *error = impl.lastError;
        return false;
    }
    if (sourceOutUs <= sourceInUs) {
        impl.setError(QStringLiteral("视频时间范围无效。"));
        if (error)
            *error = impl.lastError;
        return false;
    }
    if (displaySize.width() <= 0 || displaySize.height() <= 0) {
        impl.setError(QStringLiteral("目标显示尺寸无效。"));
        if (error)
            *error = impl.lastError;
        return false;
    }
    if (sourceInUs < 0) {
        impl.setError(QStringLiteral("视频起始时间无效。"));
        if (error)
            *error = impl.lastError;
        return false;
    }

    impl.fps = fps;
    impl.displaySize = displaySize;

    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&impl.format, utf8Path.constData(), nullptr,
                                  nullptr);
    if (ret < 0) {
        impl.setError(QStringLiteral("无法打开视频文件：%1")
                          .arg(ffmpegError(ret)));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    ret = avformat_find_stream_info(impl.format, nullptr);
    if (ret < 0) {
        impl.setError(QStringLiteral("读取视频流信息失败：%1")
                          .arg(ffmpegError(ret)));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }

    impl.videoStream = av_find_best_stream(impl.format, AVMEDIA_TYPE_VIDEO, -1,
                                           -1, nullptr, 0);
    if (impl.videoStream < 0) {
        impl.setError(QStringLiteral("文件不包含视频流。"));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    AVStream *stream = impl.format->streams[impl.videoStream];
    impl.rotation = rotationFromStream(stream);

    const auto validRate = [](AVRational rate) {
        return rate.num > 0 && rate.den > 0;
    };
    const AVRational averageRate = stream->avg_frame_rate;
    const AVRational nominalRate = stream->r_frame_rate;
    if (validRate(averageRate) && validRate(nominalRate)) {
        const double average = double(averageRate.num) / averageRate.den;
        const double nominal = double(nominalRate.num) / nominalRate.den;
        if (std::abs(average - nominal)
            / std::max(average, nominal) > 0.01) {
            impl.setError(QStringLiteral(
                "视频为可变帧率（VFR），AI 跟踪暂不支持；请先转换为恒定"
                "帧率视频。"));
            close();
            if (error)
                *error = impl.lastError;
            return false;
        }
    }
    AVRational streamRate = validRate(averageRate) ? averageRate
                                                   : nominalRate;
    if (!validRate(streamRate))
        streamRate = av_guess_frame_rate(impl.format, stream, nullptr);
    if (validRate(streamRate)) {
        const double streamFps = double(streamRate.num) / streamRate.den;
        const double requestedFps = fps.toDouble();
        if (std::abs(streamFps - requestedFps)
            / std::max(streamFps, requestedFps) > 0.01) {
            impl.setError(
                QStringLiteral("视频帧率 %1 与跟踪帧率 %2 不一致；AI 跟踪"
                               "暂不支持变帧率或帧率转换。")
                    .arg(streamFps, 0, 'g', 4)
                    .arg(requestedFps, 0, 'g', 4));
            close();
            if (error)
                *error = impl.lastError;
            return false;
        }
    }

    // Clamp the requested range to the stream duration and frame count so a
    // rounding error cannot request a frame the file does not contain.
    int64_t streamDurationUs = 0;
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        streamDurationUs = av_rescale_q(stream->duration, stream->time_base,
                                        AV_TIME_BASE_Q);
    } else if (impl.format->duration != AV_NOPTS_VALUE
               && impl.format->duration > 0) {
        streamDurationUs = impl.format->duration;
    }
    int64_t effectiveIn = sourceInUs;
    int64_t effectiveOut = sourceOutUs;
    if (streamDurationUs > 0) {
        if (effectiveIn >= streamDurationUs) {
            impl.setError(QStringLiteral("起始时间超出视频时长。"));
            close();
            if (error)
                *error = impl.lastError;
            return false;
        }
        effectiveOut = std::min(effectiveOut, streamDurationUs);
    }
    if (effectiveOut <= effectiveIn) {
        impl.setError(QStringLiteral("视频时间范围不足一帧。"));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    int count = int(microsToFrames(effectiveOut - effectiveIn, fps));
    if (stream->nb_frames > 0 && count > int(stream->nb_frames))
        count = int(stream->nb_frames);
    if (count <= 0) {
        impl.setError(QStringLiteral("视频时间范围不足一帧。"));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    impl.sourceInUs = effectiveIn;
    impl.sourceOutUs = effectiveOut;
    impl.frameCount = count;

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        impl.setError(QStringLiteral("FFmpeg 不支持该视频编码。"));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    impl.codec = avcodec_alloc_context3(codec);
    if (!impl.codec) {
        impl.setError(QStringLiteral("无法分配视频解码上下文。"));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    ret = avcodec_parameters_to_context(impl.codec, stream->codecpar);
    if (ret < 0) {
        impl.setError(QStringLiteral("复制视频解码参数失败：%1")
                          .arg(ffmpegError(ret)));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    impl.codec->pkt_timebase = stream->time_base;
    impl.codec->thread_count = 0;
    ret = avcodec_open2(impl.codec, codec, nullptr);
    if (ret < 0) {
        impl.setError(QStringLiteral("打开视频解码器失败：%1")
                          .arg(ffmpegError(ret)));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }

    impl.packet = av_packet_alloc();
    impl.frame = av_frame_alloc();
    if (!impl.packet || !impl.frame) {
        impl.setError(QStringLiteral("无法分配视频解码缓冲区。"));
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }

    // Position before the first requested frame; advance() drops frames whose
    // timestamps are before sourceInUs.
    if (!impl.seekTo(sourceInUs)) {
        close();
        if (error)
            *error = impl.lastError;
        return false;
    }
    impl.discardBeforeUs = sourceInUs;
    impl.nextIndex = 0;
    impl.eof = false;

    impl.lastError.clear();
    if (error)
        error->clear();
    return true;
}

void FrameReader::close()
{
    if (!m_impl)
        return;
    m_impl->cleanup();
    m_impl->frameCount = 0;
    m_impl->nextIndex = 0;
    m_impl->discardBeforeUs = -1;
    m_impl->drainSent = false;
    m_impl->eof = false;
}

bool FrameReader::isOpen() const
{
    return m_impl && m_impl->format != nullptr && m_impl->codec != nullptr;
}

int FrameReader::frameCount() const { return m_impl ? m_impl->frameCount : 0; }

QSize FrameReader::displaySize() const
{
    return m_impl ? m_impl->displaySize : QSize();
}

int FrameReader::rotationDegrees() const
{
    return m_impl ? m_impl->rotation : 0;
}

QImage FrameReader::readFrame(int index, QString *error)
{
    Impl &impl = *m_impl;
    const auto fail = [&](const QString &message) {
        impl.setError(message);
        if (error)
            *error = message;
        return QImage();
    };

    if (!isOpen())
        return fail(QStringLiteral("视频读取器尚未打开。"));
    if (index < 0 || index >= impl.frameCount) {
        return fail(QStringLiteral("视频帧索引超出范围：%1。").arg(index));
    }

    if (index < impl.nextIndex) {
        const int64_t targetUs = impl.sourceInUs + framesToMicros(index, impl.fps);
        if (!impl.seekTo(targetUs))
            return fail(impl.lastError);
        impl.discardBeforeUs = targetUs;
        QImage rewound;
        if (!impl.advance(&rewound)) {
            return fail(impl.lastError.isEmpty()
                            ? QStringLiteral("视频解码在帧 %1 结束。").arg(index)
                            : impl.lastError);
        }
        impl.nextIndex = index + 1;
        if (error)
            error->clear();
        return rewound;
    }

    while (impl.nextIndex < index) {
        QImage skipped;
        if (!impl.advance(&skipped)) {
            return fail(impl.lastError.isEmpty()
                            ? QStringLiteral("视频解码在帧 %1 结束。").arg(index)
                            : impl.lastError);
        }
        ++impl.nextIndex;
    }

    QImage image;
    if (!impl.advance(&image)) {
        return fail(impl.lastError.isEmpty()
                        ? QStringLiteral("视频解码在帧 %1 结束。").arg(index)
                        : impl.lastError);
    }
    impl.nextIndex = index + 1;
    if (error)
        error->clear();
    return image;
}

QList<QImage> FrameReader::readAll(QString *error)
{
    QList<QImage> frames;
    if (!isOpen()) {
        m_impl->setError(QStringLiteral("视频读取器尚未打开。"));
        if (error)
            *error = m_impl->lastError;
        return frames;
    }
    frames.reserve(frameCount());
    for (int i = 0; i < frameCount(); ++i) {
        const QImage image = readFrame(i, error);
        if (image.isNull()) {
            // Preserve the valid prefix; the error is already populated.
            return frames;
        }
        frames.append(image);
    }
    if (error)
        error->clear();
    return frames;
}

QString FrameReader::lastError() const
{
    return m_impl ? m_impl->lastError : QString();
}

} // namespace VideoEditor
