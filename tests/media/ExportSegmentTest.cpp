#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
}

#include "AudioEncoder.h"
#include "AudioTrackWriter.h"
#include "ExportCheckpoint.h"
#include "ExportEncoderCatalog.h"
#include "ExportSettings.h"
#include "Mp4Assembler.h"
#include "MediaProbe.h"
#include "Project.h"
#include "SegmentRouter.h"
#include "TimelineMixer.h"
#include "VideoEncoder.h"

using namespace VideoEditor;

namespace {

EncoderDescriptor mpeg4()
{
    EncoderDescriptor d;
    d.id = QStringLiteral("mpeg4");
    d.codec = ExportVideoCodec::Mpeg4Part2;
    d.hardware = false;
    d.available = true;
    return d;
}

struct Encoders {
    VideoEncoder video;
    AudioEncoder audio;
};

bool openEncoders(Encoders *encoders, const ExportSettings &settings, QString *error)
{
    return encoders->video.open(mpeg4(), settings, error)
           && encoders->audio.open(48000, 2, 192, error);
}

// Read back the assembled audio track so the test can prove the AAC priming
// packet survived assembly and that skip/padding still add up to the timeline.
struct AssembledAudio {
    bool ok = false;
    int64_t streamDuration = 0;
    int64_t firstPts = 0;
    int64_t firstDuration = 0;
    int64_t firstSkipSamples = -1;
    int64_t secondPts = 0;
    int64_t timelinePackets = 0;
    int64_t lastPts = 0;
    int64_t lastDuration = 0;
    int64_t lastDiscardPadding = -1;
};

bool probeAssembledAudio(const QString &path, int sampleRate, AssembledAudio *out,
                         QString *error)
{
    AVFormatContext *in = nullptr;
    if (avformat_open_input(&in, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        if (error)
            *error = QStringLiteral("无法打开输出文件：%1").arg(path);
        return false;
    }
    if (avformat_find_stream_info(in, nullptr) < 0) {
        if (error)
            *error = QStringLiteral("无法读取输出流信息。");
        avformat_close_input(&in);
        return false;
    }
    const int ai = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ai < 0) {
        if (error)
            *error = QStringLiteral("输出文件缺少音频流。");
        avformat_close_input(&in);
        return false;
    }
    const AVRational tb = in->streams[ai]->time_base;
    const AVRational sampleTb{1, sampleRate};
    out->streamDuration = av_rescale_q(in->streams[ai]->duration, tb, sampleTb);

    AVPacket *packet = av_packet_alloc();
    bool haveFirst = false;
    bool haveSecond = false;
    while (av_read_frame(in, packet) >= 0) {
        if (packet->stream_index != ai) {
            av_packet_unref(packet);
            continue;
        }
        const int64_t pts = av_rescale_q(packet->pts, tb, sampleTb);
        const int64_t duration = av_rescale_q(packet->duration, tb, sampleTb);
        size_t sideSize = 0;
        const uint8_t *side = av_packet_get_side_data(
            packet, AV_PKT_DATA_SKIP_SAMPLES, &sideSize);
        int64_t skip = -1;
        int64_t discard = -1;
        if (side && sideSize >= 8) {
            skip = AV_RL32(side);
            discard = AV_RL32(side + 4);
        }
        if (!haveFirst) {
            out->firstPts = pts;
            out->firstDuration = duration;
            out->firstSkipSamples = skip;
            haveFirst = true;
        } else if (!haveSecond) {
            out->secondPts = pts;
            haveSecond = true;
        }
        if (pts >= 0) {
            ++out->timelinePackets;
            out->lastPts = pts;
            out->lastDuration = duration;
            out->lastDiscardPadding = discard;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&in);
    out->ok = haveFirst;
    return out->ok;
}

void encodeAudioBlock(AudioEncoder *encoder, AudioTrackWriter *writer,
                      const float *samples, int64_t sampleIndex, QString *error)
{
    std::vector<EncodedPacket> packets;
    ASSERT_TRUE(encoder->encode(samples, 1024, sampleIndex, &packets, error))
        << error->toStdString();
    for (const EncodedPacket &packet : packets)
        ASSERT_TRUE(writer->write(packet, error)) << error->toStdString();
}

} // namespace

TEST(ExportSegment, CheckpointRoundTripsAndHashDetectsSettingsChange)
{
    Project project;
    project.ensureDefaultTracks();
    ExportSettings settings;
    settings.outputPath = QStringLiteral("D:/tmp/out.mp4");

    const QString hash = exportProjectHash(project, settings);
    ExportCheckpoint checkpoint;
    checkpoint.projectHash = hash;
    checkpoint.encoderKey = QStringLiteral("mpeg4@sw");
    checkpoint.width = 320;
    checkpoint.height = 180;
    checkpoint.fps = Rational{30, 1};
    checkpoint.sampleRate = 48000;
    checkpoint.totalFrames = 60;
    checkpoint.totalSamples = 96000;
    checkpoint.segmentSamples = 49152;
    checkpoint.segmentCount = 2;
    checkpoint.completedSegments = 1;

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("checkpoint.json"));
    QString error;
    ASSERT_TRUE(checkpoint.save(path, &error)) << error.toStdString();
    const auto loaded = ExportCheckpoint::load(path, &error);
    ASSERT_TRUE(loaded.has_value()) << error.toStdString();
    EXPECT_EQ(loaded->completedSegments, 1);
    EXPECT_EQ(loaded->projectHash, hash);
    EXPECT_EQ(loaded->fps, checkpoint.fps);
    EXPECT_EQ(loaded->totalSamples, checkpoint.totalSamples);

    settings.bitrateKbps = 9000;
    EXPECT_NE(exportProjectHash(project, settings), hash);
}

TEST(ExportSegment, CheckpointRejectsCorruptAndVersionMismatch)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;

    const QString missing = dir.filePath(QStringLiteral("missing.json"));
    EXPECT_FALSE(ExportCheckpoint::load(missing, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    const QString corrupt = dir.filePath(QStringLiteral("corrupt.json"));
    {
        QFile file(corrupt);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("{ not json");
    }
    EXPECT_FALSE(ExportCheckpoint::load(corrupt, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    ExportCheckpoint checkpoint;
    checkpoint.version = ExportCheckpoint::kVersion + 1;
    checkpoint.segmentCount = 2;
    checkpoint.completedSegments = 1;
    const QString outdated = dir.filePath(QStringLiteral("outdated.json"));
    ASSERT_TRUE(checkpoint.save(outdated, &error)) << error.toStdString();
    EXPECT_FALSE(ExportCheckpoint::load(outdated, &error).has_value());
}

TEST(ExportSegment, RouterWritesTwoValidSegmentsAndAssemblerJoinsThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("out.mp4"));
    const QString parts = partsDirectoryFor(output);
    clearParts(parts);

    ExportSettings settings;
    settings.codec = ExportVideoCodec::Mpeg4Part2;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 4000;
    settings.segmentTargetUs = 1'600'000; // 2 * 1.6 s = 2 segments
    const ExportTiming timing =
        computeExportTiming(3'000'000, settings.fps, 48000, settings.segmentTargetUs);
    ASSERT_EQ(timing.segmentCount, 2);
    ASSERT_EQ(timing.totalFrames, 90);
    ASSERT_EQ(timing.totalSamples, 144000);

    Encoders encoders;
    QString error;
    ASSERT_TRUE(openEncoders(&encoders, settings, &error)) << error.toStdString();
    SegmentRouter router(parts, timing, settings.fps, encoders.video.context());

    int64_t vBegin0 = 0, vEnd0 = 0, vBegin1 = 0, vEnd1 = 0;
    videoFrameRangeForSegment(timing, settings.fps, 0, &vBegin0, &vEnd0);
    videoFrameRangeForSegment(timing, settings.fps, 1, &vBegin1, &vEnd1);
    ASSERT_EQ(vEnd0 - vBegin0, 48);
    ASSERT_EQ(vEnd1 - vBegin1, 42);

    for (int64_t frame = 0; frame < timing.totalFrames; ++frame) {
        std::vector<EncodedPacket> packets;
        ASSERT_TRUE(encoders.video.encodeBlack(frame, frame == vBegin0 || frame == vBegin1,
                                               &packets, &error))
            << error.toStdString();
        for (const EncodedPacket &packet : packets)
            ASSERT_TRUE(router.routeVideo(packet, &error)) << error.toStdString();
    }
    std::vector<EncodedPacket> videoTail;
    ASSERT_TRUE(encoders.video.flush(&videoTail, &error)) << error.toStdString();
    for (const EncodedPacket &packet : videoTail)
        ASSERT_TRUE(router.routeVideo(packet, &error)) << error.toStdString();
    ASSERT_TRUE(router.finishAll(true, &error)) << error.toStdString();
    EXPECT_EQ(router.completedSegments(), 2);
    EXPECT_GT(router.bytesWritten(), 0);

    SegmentCounts counts0;
    SegmentCounts counts1;
    ASSERT_TRUE(inspectSegment(segmentPath(parts, 0), &counts0, &error))
        << error.toStdString();
    ASSERT_TRUE(inspectSegment(segmentPath(parts, 1), &counts1, &error))
        << error.toStdString();
    EXPECT_EQ(counts0.videoPackets, 48);
    EXPECT_EQ(counts1.videoPackets, 42);

    const QString audioPath = QDir(parts).filePath(QStringLiteral("audio.mp4"));
    AudioTrackWriter audioTrack;
    ASSERT_TRUE(audioTrack.open(audioPath, 48000, encoders.audio.context(),
                                timing.totalSamples, &error))
        << error.toStdString();
    TimelineMixer mixer(48000, 2);
    Project project;
    project.ensureDefaultTracks();
    mixer.setProject(project);
    const int blockFrames = 480;
    std::vector<float> block(blockFrames * 2, 0.0f);
    std::vector<float> aacBuffer(1024 * 2, 0.0f);
    int aacFill = 0;
    int64_t fed = 0;
    for (int64_t sample = 0; sample < timing.totalSamples; sample += blockFrames) {
        const int frames = int(std::min<int64_t>(blockFrames, timing.totalSamples - sample));
        mixer.setPositionSamples(sample);
        mixer.render(block.data(), frames);
        for (int i = 0; i < frames; ++i) {
            aacBuffer[size_t(aacFill) * 2] = block[size_t(i) * 2];
            aacBuffer[size_t(aacFill) * 2 + 1] = block[size_t(i) * 2 + 1];
            if (++aacFill == 1024) {
                encodeAudioBlock(&encoders.audio, &audioTrack, aacBuffer.data(), fed,
                                 &error);
                fed += 1024;
                aacFill = 0;
            }
        }
    }
    int64_t paddedSamples = 0;
    if (aacFill > 0) {
        std::fill(aacBuffer.begin() + aacFill * 2, aacBuffer.end(), 0.0f);
        paddedSamples = 1024 - aacFill;
        encodeAudioBlock(&encoders.audio, &audioTrack, aacBuffer.data(), fed, &error);
        fed += 1024;
    }
    std::vector<EncodedPacket> audioTail;
    ASSERT_TRUE(encoders.audio.flush(&audioTail, &error)) << error.toStdString();
    for (const EncodedPacket &packet : audioTail)
        ASSERT_TRUE(audioTrack.write(packet, &error)) << error.toStdString();
    ASSERT_TRUE(audioTrack.finish(&error)) << error.toStdString();
    EXPECT_EQ(encoders.audio.encoderDelaySamples(), 1024);
    EXPECT_EQ(paddedSamples, 384);
    EXPECT_EQ(fed, 144384);

    const QString temp = assemblyTempPathFor(output);
    ASSERT_TRUE(Mp4Assembler::assemble(parts, audioPath, timing, settings.fps,
                                      encoders.video.context(),
                                      encoders.audio.context(),
                                      encoders.audio.encoderDelaySamples(),
                                      paddedSamples, temp, &error))
        << error.toStdString();

    const auto info = MediaProbe::probe(temp, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->container, QStringLiteral("MP4"));
    EXPECT_EQ(info->videoCodec, QStringLiteral("mpeg4"));
    EXPECT_EQ(info->audioCodec, QStringLiteral("aac"));
    EXPECT_EQ(info->audioSampleRate, 48000);
    EXPECT_EQ(info->audioChannels, 2);
    const int64_t frameUs = 1'000'000 / 30;
    EXPECT_LE(std::llabs(info->durationUs - 3'000'000), frameUs + 1);

    // Priming evidence: the first audio packet remains the AAC pre-roll at
    // -1024 samples with skip_samples = 1024, the timeline starts at sample 0,
    // and start skip + trailing padding still add up to the exact timeline.
    AssembledAudio audio;
    ASSERT_TRUE(probeAssembledAudio(temp, 48000, &audio, &error))
        << error.toStdString();
    EXPECT_EQ(audio.firstPts, -1024);
    EXPECT_EQ(audio.firstDuration, 1024);
    EXPECT_EQ(audio.firstSkipSamples, 1024);
    EXPECT_EQ(audio.secondPts, 0);
    EXPECT_EQ(audio.timelinePackets, 141);
    EXPECT_EQ(audio.streamDuration, timing.totalSamples);
    EXPECT_EQ(audio.lastPts + audio.lastDuration, timing.totalSamples);
    EXPECT_EQ(audio.lastDuration, 1024 - paddedSamples);
}

TEST(ExportSegment, ResumedRouterCountsOnlySegmentsWrittenInThisRun)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("resume.mp4"));
    const QString parts = partsDirectoryFor(output);
    clearParts(parts);

    ExportSettings settings;
    settings.codec = ExportVideoCodec::Mpeg4Part2;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 4000;
    const ExportTiming timing =
        computeExportTiming(3'000'000, settings.fps, 48000, 1'600'000);
    ASSERT_EQ(timing.segmentCount, 2);

    Encoders encoders;
    QString error;
    ASSERT_TRUE(openEncoders(&encoders, settings, &error)) << error.toStdString();
    SegmentRouter router(parts, timing, settings.fps, encoders.video.context());

    // Resume: segment 0 is already complete on disk, so encoding starts at the
    // first frame of segment 1 and only that range is routed.
    int64_t vBegin = 0;
    int64_t vEnd = 0;
    videoFrameRangeForSegment(timing, settings.fps, 1, &vBegin, &vEnd);
    for (int64_t frame = vBegin; frame < vEnd; ++frame) {
        std::vector<EncodedPacket> packets;
        ASSERT_TRUE(encoders.video.encodeBlack(frame, frame == vBegin, &packets,
                                               &error))
            << error.toStdString();
        for (const EncodedPacket &packet : packets)
            ASSERT_TRUE(router.routeVideo(packet, &error)) << error.toStdString();
    }
    ASSERT_TRUE(router.finishAll(true, &error)) << error.toStdString();
    // The router reports what it validated in this run; the pipeline adds the
    // checkpoint's startSegment. Reporting 2 here would double count.
    EXPECT_EQ(router.completedSegments(), 1);
    EXPECT_FALSE(QFile::exists(segmentPath(parts, 0)));
    SegmentCounts counts;
    ASSERT_TRUE(inspectSegment(segmentPath(parts, 1), &counts, &error))
        << error.toStdString();
    EXPECT_EQ(counts.videoPackets, vEnd - vBegin);
}

TEST(ExportSegment, CancelClosesAndDeletesPartialLastSegment)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("cancel.mp4"));
    const QString parts = partsDirectoryFor(output);
    clearParts(parts);

    ExportSettings settings;
    settings.codec = ExportVideoCodec::Mpeg4Part2;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 4000;
    const ExportTiming timing =
        computeExportTiming(3'000'000, settings.fps, 48000, 1'600'000);
    ASSERT_EQ(timing.segmentCount, 2);

    Encoders encoders;
    QString error;
    ASSERT_TRUE(openEncoders(&encoders, settings, &error)) << error.toStdString();
    SegmentRouter router(parts, timing, settings.fps, encoders.video.context());

    for (int64_t frame = 0; frame < 48; ++frame) {
        std::vector<EncodedPacket> packets;
        ASSERT_TRUE(encoders.video.encodeBlack(frame, frame == 0, &packets, &error))
            << error.toStdString();
        for (const EncodedPacket &packet : packets)
            ASSERT_TRUE(router.routeVideo(packet, &error)) << error.toStdString();
    }
    ASSERT_TRUE(router.closeThrough(0, &error)) << error.toStdString();
    EXPECT_EQ(router.completedSegments(), 1);

    for (int64_t frame = 48; frame < 53; ++frame) {
        std::vector<EncodedPacket> packets;
        ASSERT_TRUE(encoders.video.encodeBlack(frame, false, &packets, &error))
            << error.toStdString();
        for (const EncodedPacket &packet : packets)
            ASSERT_TRUE(router.routeVideo(packet, &error)) << error.toStdString();
    }
    ASSERT_TRUE(router.finishAll(false, &error)) << error.toStdString();
    EXPECT_EQ(router.completedSegments(), 1);
    EXPECT_TRUE(QFile::exists(segmentPath(parts, 0)));
    EXPECT_FALSE(QFile::exists(segmentPath(parts, 1)));

    SegmentCounts counts;
    EXPECT_TRUE(inspectSegment(segmentPath(parts, 0), &counts, &error))
        << error.toStdString();
    EXPECT_EQ(counts.videoPackets, 48);
}
