#pragma once

// Shared helpers for the M3 export acceptance tests (format matrix, preview/
// export parity, subtitle burn-in, limiter and hardware performance).
//
// Everything here is `inline` so ExportMatrixTest.cpp and ExportParityTest.cpp
// can both include the header without duplicate-symbol link errors.

#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include <QDir>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <objbase.h>
#endif

#include "D3D11Context.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "MediaProbe.h"
#include "Project.h"

namespace VideoEditor {

inline bool d3d11Available(QString *error = nullptr)
{
    return D3D11Context::create(true, error) != nullptr
           || D3D11Context::create(false, error) != nullptr;
}

// Production probes encoders in the export QThread, never on the GUI thread.
// Doing it on a QGuiApplication main thread (COM initialized as STA) leaves
// the AMF/Media Foundation probes half-initialized and crashes Qt's teardown
// after the tests pass. A plain worker thread with an explicit MTA apartment
// mirrors the export worker and keeps every test process clean.
inline EncoderCatalog detectEncodersForTest(int width, int height,
                                            const Rational &fps)
{
    EncoderCatalog catalog;
    std::thread worker([&catalog, width, height, fps] {
#ifdef _WIN32
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        catalog = detectEncoders(width, height, fps);
#ifdef _WIN32
        if (SUCCEEDED(com))
            CoUninitialize();
#endif
    });
    worker.join();
    return catalog;
}

// Runs one export on a worker thread with an explicit MTA apartment, exactly
// like ExportWorker does in production. This matters for Media Foundation's
// software encoder (h264_mf@sw): opening it on the GUI main thread, whose COM
// apartment is STA, passes the test but leaves MF half-initialized and crashes
// QGuiApplication teardown.
inline ExportResult runExportForTest(const Project &project,
                                     const ExportSettings &settings,
                                     const EncoderCatalog &catalog,
                                     bool resume = false,
                                     const ExportPipeline::Hooks &hooks = {})
{
    ExportResult result;
    std::thread worker([&] {
#ifdef _WIN32
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        result = ExportPipeline().run(project, settings, catalog, resume, hooks);
#ifdef _WIN32
        if (SUCCEEDED(com))
            CoUninitialize();
#endif
    });
    worker.join();
    return result;
}

// The fixture probes must succeed before a test exports; a missing fixture is
// an environment problem, not a silent project with zero-length assets.
inline bool exportProjectAssetsProbed(const Project &project)
{
    const Asset *video = project.assetById(QStringLiteral("v1"));
    const Asset *audio = project.assetById(QStringLiteral("a1"));
    return video && video->info.hasVideo && audio && audio->info.hasAudio;
}

// 320x180 / 30 fps / 2 s project:
//   V1: h264.mp4 0-2 s
//   A1: tone.wav 0-2 s
//   S1: empty subtitle track (burn-in tests append their own cue)
inline Project makeExportProject(const QString &fixtureDir)
{
    Project project;
    project.name = QStringLiteral("导出验收工程");
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    project.timeline.sampleRate = 48000;

    Asset video;
    video.id = QStringLiteral("v1");
    video.path = QDir(fixtureDir).filePath(QStringLiteral("h264.mp4"));
    if (const auto probed = MediaProbe::probe(video.path))
        video.info = *probed;
    project.assets.append(video);

    Asset audio;
    audio.id = QStringLiteral("a1");
    audio.path = QDir(fixtureDir).filePath(QStringLiteral("tone.wav"));
    if (const auto probed = MediaProbe::probe(audio.path))
        audio.info = *probed;
    project.assets.append(audio);

    project.ensureDefaultTracks();
    for (Track &track : project.tracks) {
        if (track.type == TrackType::Video) {
            Clip clip;
            clip.id = QStringLiteral("vc1");
            clip.assetId = video.id;
            clip.sourceInUs = 0;
            clip.sourceOutUs = 2'000'000;
            clip.timelineStartUs = 0;
            track.clips.append(clip);
        } else if (track.type == TrackType::Audio) {
            Clip clip;
            clip.id = QStringLiteral("ac1");
            clip.assetId = audio.id;
            clip.sourceInUs = 0;
            clip.sourceOutUs = 2'000'000;
            clip.timelineStartUs = 0;
            track.clips.append(clip);
        }
        // Subtitle track: intentionally empty; tests that exercise burn-in
        // append the cue they need.
    }
    return project;
}

// 320x180 / 30 fps / 1 s project built from the flat-colour red.mp4 (64x64,
// letterboxed by the default fit transform). Used for a full-resolution parity
// gate where 4:2:0 chroma subsampling does not dominate the error.
inline Project makeFlatColorProject(const QString &fixtureDir)
{
    Project project;
    project.name = QStringLiteral("平色验收工程");
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    project.timeline.sampleRate = 48000;

    Asset video;
    video.id = QStringLiteral("v1");
    video.path = QDir(fixtureDir).filePath(QStringLiteral("red.mp4"));
    if (const auto probed = MediaProbe::probe(video.path))
        video.info = *probed;
    project.assets.append(video);

    project.ensureDefaultTracks();
    for (Track &track : project.tracks) {
        if (track.type != TrackType::Video)
            continue;
        Clip clip;
        clip.id = QStringLiteral("redc1");
        clip.assetId = video.id;
        clip.sourceInUs = 0;
        clip.sourceOutUs = 1'000'000;
        clip.timelineStartUs = 0;
        track.clips.append(clip);
    }
    return project;
}

// Peak signal-to-noise ratio over the RGB channels of two tightly packed
// RGBA8 frames. Identical frames return 99 dB (the brief's sentinel).
inline double psnrRgba(const std::vector<uint8_t> &a,
                       const std::vector<uint8_t> &b, int w, int h)
{
    const size_t pixels = size_t(w) * size_t(h);
    if (a.size() < pixels * 4 || b.size() < pixels * 4)
        return 0.0;
    double mse = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d =
                double(a[i * 4 + size_t(c)]) - double(b[i * 4 + size_t(c)]);
            mse += d * d;
        }
    }
    mse /= double(pixels * 3);
    if (mse <= 1e-9)
        return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Decodes the first video frame at or after `ptsUs` to tightly packed RGBA8
// (libavcodec + swscale). The export signals BT.709 / limited range, so the
// scaler is configured from the stream's own color metadata: relying on
// swscale's BT.601 default would shift every saturated color and make a
// pixel-parity comparison meaningless. The export tests always export at the
// project resolution, so a geometry mismatch is reported as an error instead
// of silently stretching.
inline std::vector<uint8_t> decodeFrameRgba(const QString &path, int64_t ptsUs,
                                            int w, int h, QString *error)
{
    struct State {
        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        SwsContext *sws = nullptr;
        ~State()
        {
            if (sws)
                sws_freeContext(sws);
            if (frame)
                av_frame_free(&frame);
            if (packet)
                av_packet_free(&packet);
            if (codec)
                avcodec_free_context(&codec);
            if (format)
                avformat_close_input(&format);
        }
    } state;

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::vector<uint8_t>{};
    };

    const QByteArray pathBytes = path.toUtf8();
    if (avformat_open_input(&state.format, pathBytes.constData(), nullptr,
                            nullptr)
        < 0) {
        return fail(QStringLiteral("无法打开待解码文件：%1").arg(path));
    }
    if (avformat_find_stream_info(state.format, nullptr) < 0)
        return fail(QStringLiteral("无法读取视频流信息。"));
    const int videoStream = av_find_best_stream(
        state.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream < 0)
        return fail(QStringLiteral("文件不包含视频流。"));
    AVStream *stream = state.format->streams[videoStream];

    const AVCodec *decoder =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder)
        return fail(QStringLiteral("FFmpeg 不支持该视频编码。"));
    state.codec = avcodec_alloc_context3(decoder);
    if (!state.codec)
        return fail(QStringLiteral("无法分配解码上下文。"));
    if (avcodec_parameters_to_context(state.codec, stream->codecpar) < 0)
        return fail(QStringLiteral("复制解码参数失败。"));
    state.codec->pkt_timebase = stream->time_base;
    state.codec->thread_count = 0;
    if (avcodec_open2(state.codec, decoder, nullptr) < 0)
        return fail(QStringLiteral("打开软件解码器失败。"));
    state.packet = av_packet_alloc();
    state.frame = av_frame_alloc();
    if (!state.packet || !state.frame)
        return fail(QStringLiteral("无法分配解码缓冲。"));
    if (av_seek_frame(state.format, videoStream,
                      av_rescale_q(ptsUs, AV_TIME_BASE_Q, stream->time_base),
                      AVSEEK_FLAG_BACKWARD)
        < 0) {
        return fail(QStringLiteral("定位到 %1 us 失败。").arg(ptsUs));
    }
    avcodec_flush_buffers(state.codec);

    bool gotFrame = false;
    bool drainSent = false;
    while (true) {
        const int ret = avcodec_receive_frame(state.codec, state.frame);
        if (ret == 0) {
            int64_t timestamp = state.frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE)
                timestamp = state.frame->pts;
            const int64_t framePtsUs =
                timestamp == AV_NOPTS_VALUE
                    ? 0
                    : av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
            if (framePtsUs >= ptsUs) {
                gotFrame = true;
                break;
            }
            av_frame_unref(state.frame);
            continue;
        }
        if (ret == AVERROR_EOF)
            break;
        if (ret != AVERROR(EAGAIN))
            return fail(QStringLiteral("软件解码失败。"));
        if (drainSent)
            break;
        const int read = av_read_frame(state.format, state.packet);
        if (read == AVERROR_EOF) {
            avcodec_send_packet(state.codec, nullptr);
            drainSent = true;
            continue;
        }
        if (read < 0)
            return fail(QStringLiteral("读取视频包失败。"));
        if (state.packet->stream_index == videoStream) {
            const int sent = avcodec_send_packet(state.codec, state.packet);
            if (sent < 0 && sent != AVERROR(EAGAIN))
                return fail(QStringLiteral("提交视频包失败。"));
        }
        av_packet_unref(state.packet);
    }
    if (!gotFrame)
        return fail(QStringLiteral("没有找到 %1 us 及之后的视频帧。").arg(ptsUs));
    if (state.frame->width != w || state.frame->height != h) {
        return fail(QStringLiteral("解码帧为 %1x%2，期望 %3x%4")
                        .arg(state.frame->width)
                        .arg(state.frame->height)
                        .arg(w)
                        .arg(h));
    }

    state.sws = sws_getContext(state.frame->width, state.frame->height,
                               static_cast<AVPixelFormat>(state.frame->format),
                               state.frame->width, state.frame->height,
                               AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                               nullptr);
    if (!state.sws)
        return fail(QStringLiteral("无法创建像素转换器。"));
    // sws_setColorspaceDetails: srcRange 0 = limited/tv, 1 = full/pc.
    const bool fullRange =
        stream->codecpar->color_range == AVCOL_RANGE_JPEG;
    const int coefficient =
        stream->codecpar->color_space == AVCOL_SPC_BT470BG
                || stream->codecpar->color_space == AVCOL_SPC_SMPTE170M
            ? SWS_CS_ITU601
            : SWS_CS_ITU709;
    sws_setColorspaceDetails(state.sws, sws_getCoefficients(coefficient),
                             fullRange ? 1 : 0,
                             sws_getCoefficients(coefficient), 1, 0, 1 << 16,
                             1 << 16);

    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    uint8_t *destination[4] = {rgba.data(), nullptr, nullptr, nullptr};
    const int destinationStride[4] = {w * 4, 0, 0, 0};
    if (sws_scale(state.sws, state.frame->data, state.frame->linesize, 0,
                  state.frame->height, destination, destinationStride)
        <= 0) {
        return fail(QStringLiteral("像素格式转换失败。"));
    }
    return rgba;
}

} // namespace VideoEditor
