#include "Mp4Assembler.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <QFile>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
}

#include "ExportCheckpoint.h"
#include "FfmpegError.h"

namespace VideoEditor {

namespace {

void setSkipSamples(AVPacket *packet, uint32_t skipSamples,
                    uint32_t discardPadding)
{
    av_packet_side_data_remove(packet->side_data, &packet->side_data_elems,
                               AV_PKT_DATA_SKIP_SAMPLES);
    uint8_t *side =
        av_packet_new_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, 8);
    if (!side)
        return;
    AV_WL32(side, skipSamples);
    AV_WL32(side + 4, discardPadding);
}

} // namespace

bool Mp4Assembler::assemble(const QString &partsDir,
                            const QString &audioPartPath,
                            const ExportTiming &timing, const Rational &fps,
                            AVCodecContext *video, AVCodecContext *audio,
                            int64_t audioEncoderDelay, int64_t paddedSamples,
                            const QString &tempOutput, QString *error)
{
    if (error)
        error->clear();
    if (timing.segmentCount <= 0 || !fps.isValid() || timing.sampleRate <= 0
        || !video || !audio) {
        if (error)
            *error = QStringLiteral("无法合并导出文件：时间线或编码参数无效。");
        return false;
    }

    // Every part must carry identical encoder parameters. Re-muxing parts with
    // different SPS/PPS would produce a stream no decoder can follow. Part 0's
    // extradata is kept: after a round trip through MP4 it is already in the
    // container's native format (avcC/hvcC), which is exactly what the packets
    // read back from the parts are describing. Reusing the live encoder's
    // Annex B extradata here would make movenc treat the already
    // length-prefixed packets as Annex B and rewrite them as 1-byte samples.
    std::vector<uint8_t> referenceExtradata;
    for (int i = 0; i < timing.segmentCount; ++i) {
        AVFormatContext *in = nullptr;
        const QString path = segmentPath(partsDir, i);
        if (avformat_open_input(&in, path.toUtf8().constData(), nullptr,
                                nullptr)
            < 0) {
            if (error)
                *error = QStringLiteral("无法读取分段：%1").arg(path);
            return false;
        }
        const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1,
                                           nullptr, 0);
        if (vi < 0) {
            avformat_close_input(&in);
            if (error)
                *error = QStringLiteral("分段缺少视频流：%1").arg(path);
            return false;
        }
        AVCodecParameters *par = in->streams[vi]->codecpar;
        if (i == 0) {
            if (par->extradata_size > 0 && par->extradata) {
                referenceExtradata.assign(par->extradata,
                                          par->extradata + par->extradata_size);
            } else {
                referenceExtradata.clear();
            }
        } else {
            const bool differs =
                referenceExtradata.size() != size_t(par->extradata_size)
                || (referenceExtradata.size() > 0
                    && memcmp(referenceExtradata.data(), par->extradata,
                              referenceExtradata.size())
                           != 0);
            if (differs) {
                avformat_close_input(&in);
                if (error)
                    *error = QStringLiteral(
                        "分段编码参数不一致（SPS/PPS 不同），无法合并为单个 MP4。");
                return false;
            }
        }
        avformat_close_input(&in);
    }

    AVFormatContext *out = nullptr;
    bool headerWritten = false;
    const auto fail = [&](const QString &message) -> bool {
        if (error)
            *error = message;
        if (out) {
            if (headerWritten)
                av_write_trailer(out);
            if (out->pb)
                avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
        }
        QFile::remove(tempOutput);
        return false;
    };

    if (avformat_alloc_output_context2(&out, nullptr, "mp4",
                                       tempOutput.toUtf8().constData()) < 0
        || !out) {
        out = nullptr;
        return fail(QStringLiteral("无法创建最终 MP4 封装器。"));
    }
    AVStream *outVideo = avformat_new_stream(out, nullptr);
    AVStream *outAudio = avformat_new_stream(out, nullptr);
    if (!outVideo || !outAudio)
        return fail(QStringLiteral("无法创建最终 MP4 流。"));
    outVideo->time_base = AVRational{fps.den, fps.num};
    outAudio->time_base = AVRational{1, timing.sampleRate};
    outVideo->avg_frame_rate = AVRational{fps.num, fps.den};
    if (avcodec_parameters_from_context(outVideo->codecpar, video) < 0
        || avcodec_parameters_from_context(outAudio->codecpar, audio) < 0) {
        return fail(QStringLiteral("无法复制最终文件的编码参数。"));
    }
    // Use the part's container-native extradata (avcC/hvcC for H.264/H.265)
    // rather than the encoder's Annex B copy. The packets below are copied out
    // of MP4 parts, so they are already length-prefixed; feeding movenc Annex B
    // extradata makes it run the Annex B parser over them and emit 1-byte
    // samples.
    if (!referenceExtradata.empty()) {
        av_freep(&outVideo->codecpar->extradata);
        outVideo->codecpar->extradata = static_cast<uint8_t *>(av_mallocz(
            referenceExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!outVideo->codecpar->extradata)
            return fail(QStringLiteral("无法复制最终文件的编码参数。"));
        memcpy(outVideo->codecpar->extradata, referenceExtradata.data(),
               referenceExtradata.size());
        outVideo->codecpar->extradata_size = int(referenceExtradata.size());
    }
    if (video->codec_id == AV_CODEC_ID_HEVC)
        outVideo->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    // The AAC part ends with a zero-padded encoder frame. Publishing the
    // priming and trailing padding lengths makes the mp4 muxer describe the
    // stream as exactly the requested timeline length (the final sample's
    // duration is shortened instead of playing the padding as audio).
    outAudio->codecpar->initial_padding = int(audioEncoderDelay);
    outAudio->codecpar->trailing_padding =
        int(std::clamp<int64_t>(paddedSamples, 0, 1024));
    outVideo->duration = timing.totalFrames;
    outAudio->duration = timing.totalSamples;
    if (avio_open(&out->pb, tempOutput.toUtf8().constData(), AVIO_FLAG_WRITE)
        < 0) {
        return fail(QStringLiteral("无法创建最终 MP4 文件：%1").arg(tempOutput));
    }
    AVDictionary *options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    int ret = avformat_write_header(out, &options);
    av_dict_free(&options);
    if (ret < 0) {
        return fail(QStringLiteral("无法写入最终 MP4 文件头：%1")
                        .arg(ffmpegErrorString(ret)));
    }
    headerWritten = true;

    // The muxer may pick finer time bases than the requested ones.
    const AVRational outVideoTb = outVideo->time_base;
    const AVRational outAudioTb = outAudio->time_base;
    const AVRational frameTb{fps.den, fps.num};
    const int64_t frameDuration =
        std::max<int64_t>(1, av_rescale_q(1, frameTb, outVideoTb));
    const int64_t audioEnd =
        av_rescale_q(timing.totalSamples, AVRational{1, timing.sampleRate},
                     outAudioTb);

    // 2) Video: read each part separately and rewrite packets at their exact
    //    global frame offset, so the joined stream is CFR with no gaps.
    int64_t totalVideo = 0;
    for (int i = 0; i < timing.segmentCount; ++i) {
        AVFormatContext *in = nullptr;
        const QString path = segmentPath(partsDir, i);
        if (avformat_open_input(&in, path.toUtf8().constData(), nullptr,
                                nullptr)
            < 0) {
            return fail(QStringLiteral("无法读取分段：%1").arg(path));
        }
        const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1,
                                           nullptr, 0);
        if (vi < 0) {
            avformat_close_input(&in);
            return fail(QStringLiteral("分段缺少视频流：%1").arg(path));
        }
        int64_t vBegin = 0;
        int64_t vEnd = 0;
        videoFrameRangeForSegment(timing, fps, i, &vBegin, &vEnd);
        const int64_t expected = vEnd - vBegin;
        const int64_t offset = av_rescale_q(vBegin, frameTb, outVideoTb);
        int64_t copied = 0;
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            avformat_close_input(&in);
            return fail(QStringLiteral("无法分配视频合并缓冲区。"));
        }
        while (av_read_frame(in, pkt) >= 0) {
            if (pkt->stream_index != vi) {
                av_packet_unref(pkt);
                continue;
            }
            av_packet_rescale_ts(pkt, in->streams[vi]->time_base, outVideoTb);
            pkt->pts += offset;
            pkt->dts = pkt->pts;
            pkt->duration = frameDuration;
            pkt->stream_index = outVideo->index;
            ret = av_interleaved_write_frame(out, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                const QString message = ffmpegErrorString(ret);
                av_packet_free(&pkt);
                avformat_close_input(&in);
                return fail(message);
            }
            ++copied;
        }
        av_packet_free(&pkt);
        avformat_close_input(&in);
        if (copied != expected) {
            return fail(QStringLiteral(
                            "分段 %1 的视频帧数为 %2，时间线要求 %3。")
                            .arg(i)
                            .arg(copied)
                            .arg(expected));
        }
        totalVideo += copied;
    }
    if (totalVideo != timing.totalFrames) {
        return fail(QStringLiteral("合并后的视频帧数为 %1，时间线要求 %2。")
                        .arg(totalVideo)
                        .arg(timing.totalFrames));
    }

    // 3) Audio: a single continuous track. Preserve the priming packet at its
    //    negative pts and write the explicit skip/padding side data.
    AVFormatContext *audioIn = nullptr;
    if (avformat_open_input(&audioIn, audioPartPath.toUtf8().constData(),
                            nullptr, nullptr)
        < 0) {
        return fail(QStringLiteral("无法读取音频分段：%1").arg(audioPartPath));
    }
    const int ai =
        av_find_best_stream(audioIn, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ai < 0) {
        avformat_close_input(&audioIn);
        return fail(QStringLiteral("音频分段缺少音频流。"));
    }
    if (audioIn->streams[ai]->codecpar->sample_rate != timing.sampleRate) {
        avformat_close_input(&audioIn);
        return fail(QStringLiteral("音频分段的采样率与导出时间线不一致。"));
    }
    AVPacket *apkt = av_packet_alloc();
    if (!apkt) {
        avformat_close_input(&audioIn);
        return fail(QStringLiteral("无法分配音频合并缓冲区。"));
    }
    bool firstAudio = true;
    bool sawTimelineStart = false;
    int64_t lastAudioEnd = 0;
    const int64_t expectedPriming = -av_rescale_q(
        audioEncoderDelay, AVRational{1, timing.sampleRate}, outAudioTb);
    while (av_read_frame(audioIn, apkt) >= 0) {
        if (apkt->stream_index != ai) {
            av_packet_unref(apkt);
            continue;
        }
        av_packet_rescale_ts(apkt, audioIn->streams[ai]->time_base, outAudioTb);
        apkt->stream_index = outAudio->index;
        apkt->dts = apkt->pts;
        if (firstAudio) {
            if (audioEncoderDelay > 0 && apkt->pts != expectedPriming) {
                const int64_t actualPts = apkt->pts;
                av_packet_free(&apkt);
                avformat_close_input(&audioIn);
                return fail(
                    QStringLiteral(
                        "音频分段的 AAC 预延迟位置已丢失（首包 PTS %1，应为 %2），"
                        "无法保证音画同步。")
                        .arg(actualPts)
                        .arg(expectedPriming));
            }
            if (audioEncoderDelay > 0) {
                setSkipSamples(apkt, uint32_t(audioEncoderDelay), 0);
            } else if (apkt->pts != 0) {
                const int64_t actualPts = apkt->pts;
                av_packet_free(&apkt);
                avformat_close_input(&audioIn);
                return fail(QStringLiteral(
                    "音频时间线未从采样 0 开始（首个内容包 PTS 为 %1）。")
                                .arg(actualPts));
            }
            sawTimelineStart = apkt->pts == 0;
            firstAudio = false;
        } else if (!sawTimelineStart && apkt->pts >= 0) {
            if (apkt->pts != 0) {
                const int64_t actualPts = apkt->pts;
                av_packet_free(&apkt);
                avformat_close_input(&audioIn);
                return fail(QStringLiteral(
                    "音频时间线未从采样 0 开始（首个内容包 PTS 为 %1）。")
                                .arg(actualPts));
            }
            sawTimelineStart = true;
        }
        lastAudioEnd = apkt->pts + std::max<int64_t>(apkt->duration, 0);
        if (lastAudioEnd > audioEnd) {
            // The encoder's final AAC frame is zero-padded to 1024 samples.
            // Record the padding as discard_padding and, because MP4 derives
            // the sample table length from packet durations, shorten the
            // final sample too. Without the shortened duration the container
            // would claim an extra 384 samples of audio (~8 ms) and make the
            // edit-list duration disagree with the video track.
            const int64_t excess = lastAudioEnd - audioEnd;
            const uint32_t discard = uint32_t(std::min<int64_t>(
                excess, paddedSamples > 0 ? paddedSamples : excess));
            setSkipSamples(apkt, 0, discard);
            const int64_t trimmed = audioEnd - apkt->pts;
            if (trimmed <= 0) {
                av_packet_unref(apkt);
                continue;
            }
            apkt->duration = trimmed;
            lastAudioEnd = audioEnd;
        }
        ret = av_interleaved_write_frame(out, apkt);
        av_packet_unref(apkt);
        if (ret < 0) {
            const QString message = ffmpegErrorString(ret);
            av_packet_free(&apkt);
            avformat_close_input(&audioIn);
            return fail(message);
        }
    }
    av_packet_free(&apkt);
    avformat_close_input(&audioIn);
    if (firstAudio) {
        return fail(QStringLiteral("音频分段不包含音频帧。"));
    }
    if (lastAudioEnd < audioEnd) {
        return fail(QStringLiteral("音频轨道长度不足（%1/%2 采样）。")
                        .arg(lastAudioEnd)
                        .arg(audioEnd));
    }

    ret = av_write_trailer(out);
    if (out->pb)
        avio_closep(&out->pb);
    avformat_free_context(out);
    out = nullptr;
    if (ret < 0) {
        QFile::remove(tempOutput);
        if (error)
            *error = QStringLiteral("无法完成最终 MP4 文件：%1")
                         .arg(ffmpegErrorString(ret));
        return false;
    }
    return true;
}

} // namespace VideoEditor
