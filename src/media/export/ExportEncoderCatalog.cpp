#include "ExportEncoderCatalog.h"

#include <cstring>

extern "C" {
#include <libavutil/opt.h>
}

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include "FfmpegError.h"

namespace VideoEditor {

namespace {

struct Candidate {
    const char *id;
    ExportVideoCodec codec;
    bool hardware;
    const char *displayName;
};

// Order is the documented preference order: MPEG-4 native first, then H.264
// hardware NVENC -> QSV -> AMF -> Media Foundation, then Media Foundation
// software and finally the optional OpenH264 fallback; H.265 is hardware-only.
const Candidate kCandidates[] = {
    {"mpeg4", ExportVideoCodec::Mpeg4Part2, false, "MPEG-4 Part 2（软件）"},
    {"h264_nvenc", ExportVideoCodec::H264, true, "NVIDIA NVENC H.264"},
    {"h264_qsv", ExportVideoCodec::H264, true, "Intel Quick Sync H.264"},
    {"h264_amf", ExportVideoCodec::H264, true, "AMD AMF H.264"},
    {"h264_mf", ExportVideoCodec::H264, true, "Media Foundation H.264（硬件）"},
    {"h264_mf", ExportVideoCodec::H264, false, "Media Foundation H.264（软件）"},
    {"libopenh264", ExportVideoCodec::H264, false, "OpenH264（软件）"},
    {"hevc_nvenc", ExportVideoCodec::H265, true, "NVIDIA NVENC HEVC"},
    {"hevc_qsv", ExportVideoCodec::H265, true, "Intel Quick Sync HEVC"},
    {"hevc_amf", ExportVideoCodec::H265, true, "AMD AMF HEVC"},
    {"hevc_mf", ExportVideoCodec::H265, true, "Media Foundation HEVC（硬件）"},
};

struct ProbeResult {
    bool available = false;
    QString reason;
};

QMutex s_probeMutex;
QHash<QString, ProbeResult> s_probeCache;

QString probeCacheKey(const QString &id, bool hardware, int w, int h,
                      const Rational &fps)
{
    return QStringLiteral("%1@%2:%3x%4@%5/%6")
        .arg(id, hardware ? QStringLiteral("hw") : QStringLiteral("sw"))
        .arg(w)
        .arg(h)
        .arg(fps.num)
        .arg(fps.den);
}

// The user can force the H.264 software path via VIDEOEDITOR_H264_SOFTWARE.
// Media Foundation is the licensed-by-Windows default; OpenH264 is only
// preferred when explicitly requested.
QString preferredSoftwareH264Key()
{
    if (qEnvironmentVariable("VIDEOEDITOR_H264_SOFTWARE")
            .compare(QStringLiteral("openh264"), Qt::CaseInsensitive)
        == 0) {
        return QStringLiteral("libopenh264@sw");
    }
    return QStringLiteral("h264_mf@sw");
}

EncoderDescriptor descriptorFromCandidate(const Candidate &candidate)
{
    EncoderDescriptor descriptor;
    descriptor.id = QString::fromUtf8(candidate.id);
    descriptor.codec = candidate.codec;
    descriptor.hardware = candidate.hardware;
    descriptor.displayName = QString::fromUtf8(candidate.displayName);
    return descriptor;
}

} // namespace

QString EncoderDescriptor::key() const
{
    return id + (hardware ? QStringLiteral("@hw") : QStringLiteral("@sw"));
}

AVPixelFormat encoderPixelFormat(const EncoderDescriptor &encoder)
{
    if (encoder.id == QStringLiteral("mpeg4")
        || encoder.id == QStringLiteral("libopenh264")) {
        return AV_PIX_FMT_YUV420P;
    }
    return AV_PIX_FMT_NV12;
}

void applyEncoderOptions(AVCodecContext *ctx, const EncoderDescriptor &encoder)
{
    if (!ctx || !ctx->priv_data) {
        return;
    }
    // Media Foundation distinguishes the hardware MFT from the software MFT
    // through this option; without it both rows probe identically.
    if (encoder.id == QStringLiteral("h264_mf")
        || encoder.id == QStringLiteral("hevc_mf")) {
        av_opt_set_int(ctx->priv_data, "hw_encoding", encoder.hardware ? 1 : 0,
                       0);
    }
    // CFR export uses zero B-frames so every frame yields exactly one packet.
    // Encoders that do not know the option simply ignore the failed set.
    av_opt_set_int(ctx->priv_data, "bf", 0, 0);
}

bool probeEncoder(const QString &encoderId, ExportVideoCodec codec,
                  bool hardware, int width, int height, const Rational &fps,
                  QString *error)
{
    if (error) {
        error->clear();
    }
    const QByteArray id = encoderId.toUtf8();
    const AVCodec *codecPtr = avcodec_find_encoder_by_name(id.constData());
    if (!codecPtr) {
        if (error) {
            *error = QStringLiteral("当前 FFmpeg 构建不包含编码器 %1。")
                         .arg(encoderId);
        }
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codecPtr);
    if (!ctx) {
        if (error) {
            *error = QStringLiteral("无法分配编码器上下文。");
        }
        return false;
    }
    EncoderDescriptor descriptor;
    descriptor.id = encoderId;
    descriptor.codec = codec;
    descriptor.hardware = hardware;

    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{fps.den, fps.num};
    ctx->framerate = AVRational{fps.num, fps.den};
    ctx->pix_fmt = encoderPixelFormat(descriptor);
    ctx->bit_rate = 2'000'000;
    ctx->gop_size = 30;
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    applyEncoderOptions(ctx, descriptor);

    int ret = avcodec_open2(ctx, codecPtr, nullptr);
    if (ret < 0) {
        if (error) {
            *error = ffmpegErrorString(ret);
        }
        avcodec_free_context(&ctx);
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        if (error) {
            *error = QStringLiteral("无法分配测试帧。");
        }
        avcodec_free_context(&ctx);
        return false;
    }
    frame->format = ctx->pix_fmt;
    frame->width = width;
    frame->height = height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        if (error) {
            *error = ffmpegErrorString(ret);
        }
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        if (error) {
            *error = QStringLiteral("无法分配测试 packet。");
        }
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }

    bool gotPacket = false;
    for (int i = 0; i < 3 && !gotPacket; ++i) {
        if (av_frame_make_writable(frame) < 0) {
            if (error) {
                *error = QStringLiteral("无法写入测试帧。");
            }
            break;
        }
        memset(frame->data[0], 16, size_t(frame->linesize[0]) * height);
        if (frame->data[1]) {
            memset(frame->data[1], 128,
                   size_t(frame->linesize[1]) * ((height + 1) / 2));
        }
        if (frame->data[2]) {
            memset(frame->data[2], 128,
                   size_t(frame->linesize[2]) * ((height + 1) / 2));
        }
        frame->pts = i;
        ret = avcodec_send_frame(ctx, frame);
        if (ret < 0) {
            if (error) {
                *error = ffmpegErrorString(ret);
            }
            break;
        }
        ret = avcodec_receive_packet(ctx, packet);
        if (ret == 0) {
            gotPacket = packet->size > 0;
            av_packet_unref(packet);
        } else if (ret != AVERROR(EAGAIN)) {
            if (error) {
                *error = ffmpegErrorString(ret);
            }
            break;
        }
    }
    if (!gotPacket) {
        // Flush delayed frames (some encoders wait for a second frame).
        avcodec_send_frame(ctx, nullptr);
        if (avcodec_receive_packet(ctx, packet) == 0) {
            gotPacket = packet->size > 0;
            av_packet_unref(packet);
        }
    }
    if (!gotPacket && error && error->isEmpty()) {
        *error = QStringLiteral("编码器无法输出测试帧。");
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return gotPacket;
}

EncoderCatalog detectEncoders(int width, int height, const Rational &fps,
                              bool forceReprobe)
{
    if (forceReprobe) {
        QMutexLocker locker(&s_probeMutex);
        s_probeCache.clear();
    }

    EncoderCatalog catalog;
    for (const Candidate &candidate : kCandidates) {
        EncoderDescriptor descriptor = descriptorFromCandidate(candidate);
        const QString cacheKey
            = probeCacheKey(descriptor.id, descriptor.hardware, width, height,
                            fps);

        ProbeResult result;
        bool cached = false;
        {
            QMutexLocker locker(&s_probeMutex);
            const auto it = s_probeCache.constFind(cacheKey);
            if (it != s_probeCache.constEnd()) {
                result = it.value();
                cached = true;
            }
        }
        if (!cached) {
            QString reason;
            result.available = probeEncoder(descriptor.id, descriptor.codec,
                                            descriptor.hardware, width, height,
                                            fps, &reason);
            result.reason = result.available ? QString() : reason;
            QMutexLocker locker(&s_probeMutex);
            s_probeCache.insert(cacheKey, result);
        }

        descriptor.available = result.available;
        descriptor.unavailableReason
            = result.available ? QString() : result.reason;
        catalog.encoders.append(descriptor);
    }
    return catalog;
}

QString EncoderCatalog::selectEncoder(ExportVideoCodec codec,
                                      bool preferHardware, bool *hardwareOut,
                                      QString *reason) const
{
    if (hardwareOut) {
        *hardwareOut = false;
    }
    if (reason) {
        reason->clear();
    }

    QList<const EncoderDescriptor *> available;
    for (const EncoderDescriptor &encoder : encoders) {
        if (encoder.codec == codec && encoder.available) {
            available.append(&encoder);
        }
    }

    // H.265 is hardware-only: a software encoder never satisfies the gate.
    if (codec == ExportVideoCodec::H265) {
        for (const EncoderDescriptor *encoder : available) {
            if (encoder->hardware) {
                if (hardwareOut) {
                    *hardwareOut = true;
                }
                return encoder->id;
            }
        }
        if (reason) {
            *reason = QStringLiteral(
                "当前系统没有可用的 H.265/HEVC 硬件编码器"
                "（NVENC / Quick Sync / AMF / Media Foundation 均不可用）。"
                "H.265 导出仅支持硬件编码，请改用 H.264。");
        }
        return {};
    }

    auto first = [&available](bool hardware) -> const EncoderDescriptor * {
        for (const EncoderDescriptor *encoder : available) {
            if (encoder->hardware == hardware) {
                return encoder;
            }
        }
        return nullptr;
    };

    const EncoderDescriptor *chosen = nullptr;
    if (codec == ExportVideoCodec::H264 && !preferHardware) {
        // Honour the configured software preference before falling back.
        const QString preferred = preferredSoftwareH264Key();
        for (const EncoderDescriptor *encoder : available) {
            if (!encoder->hardware && encoder->key() == preferred) {
                chosen = encoder;
                break;
            }
        }
        if (!chosen) {
            chosen = first(false);
        }
        if (!chosen) {
            chosen = first(true);
        }
    } else {
        chosen = preferHardware ? first(true) : first(false);
        if (!chosen) {
            chosen = preferHardware ? first(false) : first(true);
        }
    }

    if (!chosen) {
        if (reason) {
            *reason = QStringLiteral("没有可用的 %1 编码器。")
                          .arg(exportCodecDisplayName(codec));
        }
        return {};
    }
    if (hardwareOut) {
        *hardwareOut = chosen->hardware;
    }
    return chosen->id;
}

bool EncoderCatalog::hasHardware(ExportVideoCodec codec) const
{
    for (const EncoderDescriptor &encoder : encoders) {
        if (encoder.codec == codec && encoder.hardware && encoder.available) {
            return true;
        }
    }
    return false;
}

bool EncoderCatalog::hasAny(ExportVideoCodec codec) const
{
    for (const EncoderDescriptor &encoder : encoders) {
        if (encoder.codec == codec && encoder.available) {
            return true;
        }
    }
    return false;
}

const EncoderDescriptor *EncoderCatalog::findByKey(const QString &key) const
{
    for (const EncoderDescriptor &encoder : encoders) {
        if (encoder.key() == key) {
            return &encoder;
        }
    }
    return nullptr;
}

EncoderCatalog EncoderCatalog::withoutHardwareForTest()
{
    EncoderCatalog catalog;
    for (const Candidate &candidate : kCandidates) {
        EncoderDescriptor descriptor = descriptorFromCandidate(candidate);
        const bool softwareOnlyRow
            = !descriptor.hardware
              && (descriptor.id == QStringLiteral("mpeg4")
                  || descriptor.id == QStringLiteral("h264_mf")
                  || descriptor.id == QStringLiteral("libopenh264"));
        descriptor.available = softwareOnlyRow;
        if (!softwareOnlyRow) {
            descriptor.unavailableReason = QStringLiteral("test");
        }
        catalog.encoders.append(descriptor);
    }
    return catalog;
}

void resetEncoderProbeCacheForTest()
{
    QMutexLocker locker(&s_probeMutex);
    s_probeCache.clear();
}

} // namespace VideoEditor
