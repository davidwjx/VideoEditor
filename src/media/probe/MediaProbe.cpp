#include "MediaProbe.h"

#include <cmath>
#include <memory>

#include <QFileInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
}

#include "CodecSupport.h"
#include "FfmpegError.h"

namespace VideoEditor {

namespace {

struct FormatContextDeleter {
    void operator()(AVFormatContext *ctx) const
    {
        if (ctx)
            avformat_close_input(&ctx);
    }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

const AVPacketSideData *findDisplayMatrix(AVStream *stream)
{
    if (auto *side = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                             stream->codecpar->nb_coded_side_data,
                                             AV_PKT_DATA_DISPLAYMATRIX)) {
        return side;
    }
    return av_packet_side_data_get(stream->side_data, stream->nb_side_data,
                                   AV_PKT_DATA_DISPLAYMATRIX);
}

QString containerName(AVFormatContext *format)
{
    const QString name = QString::fromUtf8(format->iformat->name ? format->iformat->name : "");
    if (name.contains(QStringLiteral("mp4")) || name.contains(QStringLiteral("mov")))
        return QStringLiteral("MP4");
    if (name.contains(QStringLiteral("asf")))
        return QStringLiteral("WMV");
    return name;
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
    // 转换为顺时针角度，与预览旋转方向一致。
    int normalized = static_cast<int>(std::llround(-counterClockwise)) % 360;
    if (normalized < 0)
        normalized += 360;
    return normalized;
}

} // namespace

std::optional<MediaInfo> MediaProbe::probe(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) {
        setError(error, QStringLiteral("文件不存在：%1").arg(path));
        return std::nullopt;
    }

    AVFormatContext *rawFormat = nullptr;
    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&rawFormat, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, cannotOpenMessage(path, ret));
        return std::nullopt;
    }
    FormatContextPtr format(rawFormat);

    ret = avformat_find_stream_info(format.get(), nullptr);
    if (ret < 0) {
        setError(error, cannotOpenMessage(path, ret));
        return std::nullopt;
    }

    const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO,
                                               -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        setError(error, QStringLiteral("“%1”不包含视频流。").arg(QFileInfo(path).fileName()));
        return std::nullopt;
    }

    AVStream *video = format->streams[videoIndex];
    const AVCodecParameters *videoParams = video->codecpar;
    const QString videoCodec = QString::fromUtf8(avcodec_get_name(videoParams->codec_id));

    MediaInfo info;
    info.path = path;
    info.container = containerName(format.get());
    info.videoCodec = videoCodec;
    info.width = videoParams->width;
    info.height = videoParams->height;
    info.rotationDegrees = rotationFromStream(video);

    AVRational rate = av_guess_frame_rate(format.get(), video, nullptr);
    if (rate.num <= 0 || rate.den <= 0)
        rate = video->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0)
        rate = video->r_frame_rate;
    if (rate.num > 0 && rate.den > 0)
        info.frameRate = Rational{rate.num, rate.den};
    else
        info.frameRate = Rational{0, 1};

    if (video->duration != AV_NOPTS_VALUE) {
        info.durationUs = av_rescale_q(video->duration, video->time_base, AV_TIME_BASE_Q);
    } else if (format->duration != AV_NOPTS_VALUE) {
        info.durationUs = format->duration;
    }

    const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO,
                                               -1, -1, nullptr, 0);
    if (audioIndex >= 0) {
        info.hasAudio = true;
        info.audioCodec = QString::fromUtf8(
            avcodec_get_name(format->streams[audioIndex]->codecpar->codec_id));
    }

    const CodecVerdict verdict = classifyVideoCodec(info.videoCodec);
    info.supported = verdict.supported;
    info.unsupportedReason = verdict.reason;

    if (info.frameRate.isValid() && info.durationUs <= 0) {
        info.supported = false;
        info.unsupportedReason = QStringLiteral("无法读取视频时长，文件可能已损坏。");
    }

    return info;
}

} // namespace VideoEditor
