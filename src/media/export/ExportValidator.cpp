#include "ExportValidator.h"

#include <cmath>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "FfmpegError.h"
#include "MediaProbe.h"

namespace VideoEditor {

namespace {

bool decodeFirstPacket(AVStream *stream, const AVPacket *packet, QString *error)
{
    if (!stream || !packet)
        return false;
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        if (error)
            *error = QStringLiteral("找不到对应的解码器。");
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0
        || avcodec_open2(ctx, codec, nullptr) < 0) {
        if (error)
            *error = QStringLiteral("无法打开解码器。");
        avcodec_free_context(&ctx);
        return false;
    }
    AVFrame *frame = av_frame_alloc();
    int ret = frame ? avcodec_send_packet(ctx, packet) : AVERROR(ENOMEM);
    if (ret >= 0)
        ret = avcodec_receive_frame(ctx, frame);
    const bool ok = ret == 0;
    if (!ok && error)
        *error = ffmpegErrorString(ret);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

} // namespace

ExportValidation validateExportFile(const QString &path,
                                    const ExportSettings &settings,
                                    const ExportTiming &timing)
{
    ExportValidation v;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    if (!info) {
        v.error = error.isEmpty()
                      ? QStringLiteral("无法重新读取导出文件：%1").arg(path)
                      : error;
        return v;
    }
    v.info = *info;
    const int64_t frameUs =
        (1'000'000LL * settings.fps.den + settings.fps.num / 2)
        / std::max(1, settings.fps.num);
    if (info->container != QStringLiteral("MP4")) {
        v.error = QStringLiteral("导出文件不是 MP4 容器。");
        return v;
    }
    if (info->videoCodec != exportCodecFfmpegName(settings.codec)) {
        v.error = QStringLiteral("视频编码不匹配：期望 %1，实际 %2。")
                      .arg(exportCodecFfmpegName(settings.codec),
                           info->videoCodec);
        return v;
    }
    if (info->width != settings.width || info->height != settings.height) {
        v.error = QStringLiteral("分辨率不匹配：期望 %1×%2，实际 %3×%4。")
                      .arg(settings.width)
                      .arg(settings.height)
                      .arg(info->width)
                      .arg(info->height);
        return v;
    }
    if (!info->hasAudio || info->audioCodec != QStringLiteral("aac")
        || info->audioSampleRate != 48'000 || info->audioChannels != 2) {
        v.error = QStringLiteral("音频轨不符合要求（需要 AAC、48 kHz、双声道）。");
        return v;
    }
    const double expectedFps = settings.fps.toDouble();
    if (!info->frameRate.isValid()
        || std::abs(info->frameRate.toDouble() - expectedFps) > 0.01) {
        v.error = QStringLiteral("帧率不匹配（期望 %1，实际 %2）。")
                      .arg(expectedFps, 0, 'f', 3)
                      .arg(info->frameRate.toDouble(), 0, 'f', 3);
        return v;
    }

    // Walk every packet to count video frames and audio samples, and keep the
    // first video packet for a real software decode below.
    AVFormatContext *in = nullptr;
    if (avformat_open_input(&in, path.toUtf8().constData(), nullptr, nullptr)
        < 0) {
        v.error = QStringLiteral("无法重新打开导出文件。");
        return v;
    }
    if (avformat_find_stream_info(in, nullptr) < 0) {
        v.error = QStringLiteral("无法读取导出文件的流信息。");
        avformat_close_input(&in);
        return v;
    }
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vi < 0 || ai < 0) {
        v.error = QStringLiteral("导出文件缺少视频或音频流。");
        avformat_close_input(&in);
        return v;
    }
    AVPacket *pkt = av_packet_alloc();
    AVPacket *firstVideo = nullptr;
    if (!pkt) {
        v.error = QStringLiteral("无法分配导出校验缓冲区。");
        avformat_close_input(&in);
        return v;
    }
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            ++v.videoPacketCount;
            if (!firstVideo) {
                firstVideo = av_packet_alloc();
                if (firstVideo)
                    av_packet_ref(firstVideo, pkt);
            }
        } else if (pkt->stream_index == ai && pkt->duration > 0) {
            v.audioSampleCount += av_rescale_q(
                pkt->duration, in->streams[ai]->time_base,
                AVRational{1, timing.sampleRate});
        }
        av_packet_unref(pkt);
    }
    const int64_t expectedVideoUs =
        (timing.totalFrames * 1'000'000LL * settings.fps.den
         + settings.fps.num / 2)
        / std::max(1, settings.fps.num);
    const int64_t expectedAudioUs =
        (timing.totalSamples * 1'000'000LL + timing.sampleRate / 2)
        / std::max(1, timing.sampleRate);
    v.videoDurationUs = info->durationUs;
    v.audioDurationUs = info->audioDurationUs;
    if (std::llabs(v.videoDurationUs - expectedVideoUs) > frameUs + 1) {
        v.error = QStringLiteral("视频时长偏差超过一帧（期望 %1 微秒，实际 %2 微秒）。")
                      .arg(expectedVideoUs)
                      .arg(v.videoDurationUs);
    } else if (std::llabs(v.audioDurationUs - expectedAudioUs) > frameUs + 1) {
        v.error = QStringLiteral("音频时长偏差超过一帧（期望 %1 微秒，实际 %2 微秒）。")
                      .arg(expectedAudioUs)
                      .arg(v.audioDurationUs);
    } else if (std::llabs(v.audioDurationUs - v.videoDurationUs) > frameUs + 1) {
        v.error = QStringLiteral("音画时长偏差超过一帧（视频 %1 微秒，音频 %2 微秒）。")
                      .arg(v.videoDurationUs)
                      .arg(v.audioDurationUs);
    } else if (v.videoPacketCount != timing.totalFrames) {
        v.error = QStringLiteral("视频帧数不匹配：期望 %1，实际 %2。")
                      .arg(timing.totalFrames)
                      .arg(v.videoPacketCount);
    } else if (std::llabs(v.audioSampleCount - timing.totalSamples) > 1024) {
        v.error = QStringLiteral("音频采样数偏差超过一个 AAC 帧（期望 %1，实际 %2）。")
                      .arg(timing.totalSamples)
                      .arg(v.audioSampleCount);
    } else {
        // Decode the first video packet with a software decoder so the file is
        // proven playable, not merely well-formed.
        v.decodedFirstFrame =
            decodeFirstPacket(in->streams[vi], firstVideo, &error);
        if (!v.decodedFirstFrame)
            v.error = QStringLiteral("无法解码导出文件的首帧：%1").arg(error);
    }
    av_packet_free(&firstVideo);
    av_packet_free(&pkt);
    avformat_close_input(&in);
    v.ok = v.error.isEmpty();
    return v;
}

} // namespace VideoEditor
