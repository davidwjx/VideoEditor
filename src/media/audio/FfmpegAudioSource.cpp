#include "FfmpegAudioSource.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <QFileInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "FfmpegError.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

int64_t sampleIndexForUs(int64_t us, int sampleRate)
{
    if (us <= 0 || sampleRate <= 0)
        return 0;
    return (us * sampleRate) / 1'000'000;
}

} // namespace

struct FfmpegAudioSource::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *fifo = nullptr;
    std::vector<float> scratch;
    int streamIndex = -1;
    int sampleRate = 48000;
    int channels = 2;
    int64_t durationUs = 0;
    int64_t fifoStartSample = 0;   // source sample index of fifo[0]
    bool drainSent = false;
    bool monoToStereo = false;     // apply +3 dB per channel dual-mono upmix

    ~Impl()
    {
        if (fifo)
            av_audio_fifo_free(fifo);
        if (swr)
            swr_free(&swr);
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (codec)
            avcodec_free_context(&codec);
        if (format)
            avformat_close_input(&format);
    }

    int64_t frameStartSample(AVStream *stream, const AVFrame *decoded) const
    {
        int64_t pts = decoded->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
            pts = decoded->pts;
        if (pts == AV_NOPTS_VALUE)
            return AV_NOPTS_VALUE;
        return av_rescale_q(pts, stream->time_base,
                            AVRational{1, sampleRate});
    }

    // Appends converted interleaved samples, aligning them to the decoded
    // frame timestamp and filling any gap with silence.
    void appendConverted(int64_t frameStartSample, int converted)
    {
        if (converted <= 0)
            return;

        int fifoFrames = av_audio_fifo_size(fifo);
        if (fifoFrames == 0 && frameStartSample != AV_NOPTS_VALUE)
            fifoStartSample = frameStartSample;

        int offset = 0;
        int count = converted;
        if (frameStartSample != AV_NOPTS_VALUE) {
            const int64_t expected = fifoStartSample + fifoFrames;
            const int64_t gap = frameStartSample - expected;
            if (gap > int64_t(sampleRate) * 10) {
                // A large discontinuity (e.g. after a seek): trust the new
                // timestamp instead of allocating seconds of silence.
                av_audio_fifo_reset(fifo);
                fifoStartSample = frameStartSample;
            } else if (gap > 0) {
                std::vector<float> silence(size_t(gap) * size_t(channels), 0.0f);
                void *planes[1] = {silence.data()};
                av_audio_fifo_write(fifo, planes, int(gap));
            } else if (gap < 0) {
                const int64_t overlap = -gap;
                if (overlap >= count)
                    return;
                offset = int(overlap);
                count -= offset;
            }
        }
        if (count <= 0)
            return;
        void *planes[1] = {scratch.data() + size_t(offset) * size_t(channels)};
        av_audio_fifo_write(fifo, planes, count);
    }
};

FfmpegAudioSource::FfmpegAudioSource(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

FfmpegAudioSource::~FfmpegAudioSource() = default;

std::unique_ptr<FfmpegAudioSource> FfmpegAudioSource::open(const QString &path,
                                                           int sampleRate,
                                                           int channels,
                                                           QString *error)
{
    if (sampleRate <= 0 || channels <= 0) {
        setError(error, QStringLiteral("无效的音频参数。"));
        return nullptr;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        setError(error, QStringLiteral("音频文件不存在：%1").arg(path));
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->sampleRate = sampleRate;
    impl->channels = channels;

    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&impl->format, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, cannotOpenMessage(path, ret));
        return nullptr;
    }
    ret = avformat_find_stream_info(impl->format, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("无法读取音频流信息：%1")
                            .arg(ffmpegErrorString(ret)));
        return nullptr;
    }

    impl->streamIndex = av_find_best_stream(impl->format, AVMEDIA_TYPE_AUDIO,
                                            -1, -1, nullptr, 0);
    if (impl->streamIndex < 0) {
        setError(error, QStringLiteral("文件中没有音频流：%1").arg(path));
        return nullptr;
    }
    AVStream *stream = impl->format->streams[impl->streamIndex];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        setError(error, QStringLiteral("找不到音频解码器：%1")
                            .arg(QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id))));
        return nullptr;
    }
    impl->codec = avcodec_alloc_context3(decoder);
    if (!impl->codec) {
        setError(error, QStringLiteral("无法分配音频解码器。"));
        return nullptr;
    }
    ret = avcodec_parameters_to_context(impl->codec, stream->codecpar);
    if (ret < 0) {
        setError(error, QStringLiteral("无法初始化解码器参数：%1")
                            .arg(ffmpegErrorString(ret)));
        return nullptr;
    }
    impl->codec->pkt_timebase = stream->time_base;
    ret = avcodec_open2(impl->codec, decoder, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("无法打开音频解码器：%1")
                            .arg(ffmpegErrorString(ret)));
        return nullptr;
    }
    if (impl->codec->sample_rate <= 0) {
        setError(error, QStringLiteral("音频流缺少有效采样率。"));
        return nullptr;
    }

    AVChannelLayout inLayout{};
    if (impl->codec->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&inLayout, &impl->codec->ch_layout);
    } else {
        av_channel_layout_default(&inLayout, channels);
    }
    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, channels);
    ret = swr_alloc_set_opts2(&impl->swr, &outLayout, AV_SAMPLE_FMT_FLT, sampleRate,
                              &inLayout, impl->codec->sample_fmt,
                              impl->codec->sample_rate, 0, nullptr);
    impl->monoToStereo = inLayout.nb_channels == 1 && channels == 2;
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || !impl->swr) {
        setError(error, QStringLiteral("音频重采样初始化失败：%1")
                            .arg(ffmpegErrorString(ret)));
        return nullptr;
    }
    ret = swr_init(impl->swr);
    if (ret < 0) {
        setError(error, QStringLiteral("音频重采样初始化失败：%1")
                            .arg(ffmpegErrorString(ret)));
        return nullptr;
    }

    impl->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, channels, 1);
    impl->packet = av_packet_alloc();
    impl->frame = av_frame_alloc();
    if (!impl->fifo || !impl->packet || !impl->frame) {
        setError(error, QStringLiteral("音频缓存分配失败。"));
        return nullptr;
    }

    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        impl->durationUs = av_rescale_q(stream->duration, stream->time_base,
                                        AV_TIME_BASE_Q);
    } else if (impl->format->duration != AV_NOPTS_VALUE && impl->format->duration > 0) {
        impl->durationUs = impl->format->duration;
    }

    return std::unique_ptr<FfmpegAudioSource>(new FfmpegAudioSource(std::move(impl)));
}

bool FfmpegAudioSource::seek(int64_t sourceUs, QString *error)
{
    if (!m_impl)
        return false;
    Impl &impl = *m_impl;
    if (sourceUs < 0)
        sourceUs = 0;

    AVStream *stream = impl.format->streams[impl.streamIndex];
    const int64_t timestamp = av_rescale_q(sourceUs, AV_TIME_BASE_Q,
                                           stream->time_base);
    const int ret = av_seek_frame(impl.format, impl.streamIndex, timestamp,
                                  AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        setError(error, QStringLiteral("音频定位失败：%1")
                            .arg(ffmpegErrorString(ret)));
        return false;
    }
    avcodec_flush_buffers(impl.codec);
    if (impl.swr)
        swr_convert(impl.swr, nullptr, 0, nullptr, 0);   // discard buffered output
    if (impl.fifo)
        av_audio_fifo_reset(impl.fifo);
    impl.fifoStartSample = sampleIndexForUs(sourceUs, impl.sampleRate);
    impl.drainSent = false;
    return true;
}

bool FfmpegAudioSource::decodeMore(QString *error)
{
    if (!m_impl)
        return false;
    Impl &impl = *m_impl;
    AVStream *stream = impl.format->streams[impl.streamIndex];

    while (true) {
        const int ret = avcodec_receive_frame(impl.codec, impl.frame);
        if (ret == 0) {
            const int64_t frameStart = impl.frameStartSample(stream, impl.frame);
            const int capacity = std::max(
                swr_get_out_samples(impl.swr, impl.frame->nb_samples) + 64,
                impl.frame->nb_samples + 64);
            const int64_t totalSamples = int64_t(capacity) * impl.channels;
            if (totalSamples > int64_t(1) << 24) {
                av_frame_unref(impl.frame);
                setError(error, QStringLiteral("音频帧过大，无法解码。"));
                return false;
            }
            impl.scratch.assign(size_t(capacity) * size_t(impl.channels), 0.0f);
            uint8_t *outPlanes[1] = {reinterpret_cast<uint8_t *>(impl.scratch.data())};
            const int converted = swr_convert(
                impl.swr, outPlanes, capacity,
                const_cast<const uint8_t **>(impl.frame->extended_data),
                impl.frame->nb_samples);
            if (converted < 0) {
                av_frame_unref(impl.frame);
                setError(error, QStringLiteral("音频重采样失败：%1")
                                    .arg(ffmpegErrorString(converted)));
                return false;
            }
            if (converted == 0) {
                av_frame_unref(impl.frame);
                continue;
            }
            // libswresample upmixes mono to stereo with a 1/sqrt(2) matrix so
            // that total power is preserved. Preview mixing instead uses the
            // dual-mono convention (+3 dB per channel), which keeps a mono
            // asset as loud as a stereo one at the same source level.
            if (impl.monoToStereo) {
                const size_t count = size_t(converted) * size_t(impl.channels);
                for (size_t i = 0; i < count; ++i)
                    impl.scratch[i] *= 2.0f;
            }
            av_frame_unref(impl.frame);
            impl.appendConverted(frameStart, converted);
            return true;
        }
        if (ret == AVERROR_EOF) {
            impl.drainSent = true;
            return false;   // EOF, no error message
        }
        if (ret != AVERROR(EAGAIN)) {
            setError(error, QStringLiteral("音频解码失败：%1")
                                .arg(ffmpegErrorString(ret)));
            return false;
        }
        if (impl.drainSent)
            return false;   // drained

        const int readRet = av_read_frame(impl.format, impl.packet);
        if (readRet == AVERROR_EOF) {
            avcodec_send_packet(impl.codec, nullptr);
            impl.drainSent = true;
            continue;
        }
        if (readRet < 0) {
            setError(error, QStringLiteral("读取音频数据失败：%1")
                                .arg(ffmpegErrorString(readRet)));
            return false;
        }
        if (impl.packet->stream_index != impl.streamIndex) {
            av_packet_unref(impl.packet);
            continue;
        }
        const int sendRet = avcodec_send_packet(impl.codec, impl.packet);
        av_packet_unref(impl.packet);
        if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
            setError(error, QStringLiteral("音频解码失败：%1")
                                .arg(ffmpegErrorString(sendRet)));
            return false;
        }
    }
}

bool FfmpegAudioSource::readRange(int64_t startUs, int frames, float *out,
                                  QString *error)
{
    if (!m_impl || !out || frames <= 0)
        return true;
    Impl &impl = *m_impl;
    std::fill(out, out + size_t(frames) * size_t(impl.channels), 0.0f);
    if (startUs < 0)
        startUs = 0;

    const int64_t startSample = sampleIndexForUs(startUs, impl.sampleRate);
    const int fifoFrames = av_audio_fifo_size(impl.fifo);
    const bool needSeek = fifoFrames == 0
        || startSample < impl.fifoStartSample
        || startSample > impl.fifoStartSample + int64_t(fifoFrames) + impl.sampleRate;
    if (needSeek && !seek(startUs, error))
        return false;

    int filled = 0;
    while (filled < frames) {
        const int available = av_audio_fifo_size(impl.fifo);
        if (available <= 0) {
            QString decodeError;
            if (!decodeMore(&decodeError)) {
                if (!decodeError.isEmpty()) {
                    setError(error, decodeError);
                    return false;
                }
                break;   // EOF: keep the remaining frames silent
            }
            continue;
        }
        if (impl.fifoStartSample < startSample) {
            const int drop = int(std::min<int64_t>(startSample - impl.fifoStartSample,
                                                   available));
            av_audio_fifo_drain(impl.fifo, drop);
            impl.fifoStartSample += drop;
            continue;
        }
        const int64_t targetSample = startSample + filled;
        if (impl.fifoStartSample > targetSample) {
            // The decoder seeked past the requested position: leave silence.
            const int64_t gap = impl.fifoStartSample - targetSample;
            filled += int(std::min<int64_t>(gap, frames - filled));
            continue;
        }
        const int want = std::min(frames - filled, available);
        void *planes[1] = {out + size_t(filled) * size_t(impl.channels)};
        const int got = av_audio_fifo_read(impl.fifo, planes, want);
        if (got <= 0)
            break;
        impl.fifoStartSample += got;
        filled += got;
    }
    return true;
}

int64_t FfmpegAudioSource::durationUs() const
{
    return m_impl ? m_impl->durationUs : 0;
}

} // namespace VideoEditor
