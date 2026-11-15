#include <gtest/gtest.h>

#include <algorithm>

#include <QJsonObject>
#include <QPair>

#include "ExportSettings.h"

using namespace VideoEditor;

TEST(ExportSettings, BuiltinPresetsCoverAllThreeCodecs)
{
    const QList<ExportPreset> presets = builtinExportPresets();
    ASSERT_GE(presets.size(), 5);
    bool sawMpeg4 = false, sawH264 = false, sawH265 = false, sawMatch = false;
    for (const ExportPreset &preset : presets) {
        sawMpeg4 |= preset.codec == ExportVideoCodec::Mpeg4Part2;
        sawH264 |= preset.codec == ExportVideoCodec::H264;
        sawH265 |= preset.codec == ExportVideoCodec::H265;
        sawMatch |= preset.matchTimeline;
    }
    EXPECT_TRUE(sawMpeg4);
    EXPECT_TRUE(sawH264);
    EXPECT_TRUE(sawH265);
    EXPECT_TRUE(sawMatch);
}

TEST(ExportSettings, MatchPresetAdoptsTimelineResolutionAndEvenRounds)
{
    TimelineSettings timeline;
    timeline.width = 1921;
    timeline.height = 1081;
    timeline.fps = Rational{60, 1};

    ExportPreset preset;
    preset.matchTimeline = true;
    preset.codec = ExportVideoCodec::H264;

    const ExportSettings settings = settingsFromPreset(preset, timeline);
    EXPECT_EQ(settings.width % 2, 0);
    EXPECT_EQ(settings.height % 2, 0);
    EXPECT_EQ(settings.fps.num, 60);
    EXPECT_EQ(settings.codec, ExportVideoCodec::H264);
}

TEST(ExportSettings, QualityTierDerivesBitratePerPixelAndFrame)
{
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H264;
    settings.width = 1920;
    settings.height = 1080;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Quality;
    settings.quality = ExportQuality::Medium;
    // 1920*1080*30*0.10 bpp / 1000 = 6220 kbps, tolerance for the chosen bpp.
    const int kbps = effectiveBitrateKbps(settings);
    EXPECT_GE(kbps, 5000);
    EXPECT_LE(kbps, 8000);

    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 12345;
    EXPECT_EQ(effectiveBitrateKbps(settings), 12345);
}

TEST(ExportSettings, TimingIsExactAtCommonFrameRates)
{
    const Rational rates[] = {{24, 1}, {25, 1}, {30, 1}, {50, 1}, {60, 1},
                              {30000, 1001}};
    for (const Rational &fps : rates) {
        const ExportTiming timing =
            computeExportTiming(10'000'000, fps, 48000, 8'000'000);
        EXPECT_GE(timing.totalFrames, 1) << fps.num << "/" << fps.den;
        EXPECT_GT(timing.totalSamples, 0);
        EXPECT_EQ(timing.segmentSamples % 1024, 0);
        EXPECT_GE(timing.segmentCount, 1);
        // Every video frame belongs to exactly one segment, with no gap.
        int64_t expected = 0;
        for (int i = 0; i < timing.segmentCount; ++i) {
            int64_t begin = 0, end = 0;
            videoFrameRangeForSegment(timing, fps, i, &begin, &end);
            EXPECT_EQ(begin, expected) << "segment " << i;
            EXPECT_GE(end, begin);
            expected = end;
        }
        EXPECT_EQ(expected, timing.totalFrames);
        // Audio samples also tile the timeline exactly.
        int64_t sample = 0;
        for (int i = 0; i < timing.segmentCount; ++i) {
            int64_t begin = 0, end = 0;
            audioSampleRangeForSegment(timing, i, &begin, &end);
            EXPECT_EQ(begin, sample);
            sample = end;
        }
        EXPECT_EQ(sample, timing.totalSamples);
    }
}

TEST(ExportSettings, OneFrameTimelineStillHasOneFrameAndOneSegment)
{
    const Rational fps{30, 1};
    const ExportTiming timing =
        computeExportTiming(1, fps, 48000, 8'000'000);
    EXPECT_EQ(timing.totalFrames, 1);
    EXPECT_EQ(timing.segmentCount, 1);
    int64_t begin = -1, end = -1;
    videoFrameRangeForSegment(timing, fps, 0, &begin, &end);
    EXPECT_EQ(begin, 0);
    EXPECT_EQ(end, 1);
    int64_t sb = -1, se = -1;
    audioSampleRangeForSegment(timing, 0, &sb, &se);
    EXPECT_EQ(sb, 0);
    EXPECT_EQ(se, timing.totalSamples);
}

// A BGM file is often a few milliseconds longer than the last video frame
// (MP3 padding). At 30 fps with one-second segments that tail can start a
// segment after the final frame PTS; such a segment has no video and must be
// folded into its predecessor instead of failing the export.
TEST(ExportSettings, SubFrameAudioTailHasNoEmptyTrailingSegment)
{
    const Rational fps{30, 1};
    const ExportTiming timing =
        computeExportTiming(10'032'000, fps, 48000, 1'000'000);
    ASSERT_EQ(timing.totalFrames, 301);
    ASSERT_EQ(timing.totalSamples, 481'600);
    EXPECT_EQ(timing.segmentCount, 10);
    int64_t expected = 0;
    for (int i = 0; i < timing.segmentCount; ++i) {
        int64_t begin = 0;
        int64_t end = 0;
        videoFrameRangeForSegment(timing, fps, i, &begin, &end);
        EXPECT_EQ(begin, expected) << "segment " << i;
        EXPECT_GT(end, begin) << "segment " << i;
        expected = end;
    }
    EXPECT_EQ(expected, timing.totalFrames);
    // Audio still tiles the full timeline, with the folded tail in the last
    // segment.
    int64_t sample = 0;
    for (int i = 0; i < timing.segmentCount; ++i) {
        int64_t begin = 0;
        int64_t end = 0;
        audioSampleRangeForSegment(timing, i, &begin, &end);
        EXPECT_EQ(begin, sample) << "segment " << i;
        sample = end;
    }
    EXPECT_EQ(sample, timing.totalSamples);
}

TEST(ExportSettings, FirstFrameAtOrAfterUsesRoundedFramePts)
{
    const Rational fps{30000, 1001};
    EXPECT_EQ(framePtsUs(0, fps), 0);
    EXPECT_EQ(firstVideoFrameAtOrAfter(0, fps), 0);
    for (int64_t frame = 0; frame < 300; ++frame) {
        const int64_t pts = framePtsUs(frame, fps);
        EXPECT_EQ(firstVideoFrameAtOrAfter(pts, fps), frame);
        if (frame > 0) {
            EXPECT_EQ(firstVideoFrameAtOrAfter(pts - 1, fps), frame);
        }
    }
}

TEST(ExportSettings, ValidationRejectsBadOutputs)
{
    TimelineSettings timeline;
    timeline.width = 1920;
    timeline.height = 1080;
    timeline.fps = Rational{30, 1};

    ExportSettings settings;
    settings.outputPath = QStringLiteral("out.mp4");
    settings.codec = ExportVideoCodec::H264;
    EXPECT_TRUE(validateExportSettings(settings, timeline).isEmpty());

    settings.width = 1919;
    EXPECT_FALSE(validateExportSettings(settings, timeline).isEmpty());
    settings.width = 1920;
    settings.fps = Rational{0, 1};
    EXPECT_FALSE(validateExportSettings(settings, timeline).isEmpty());
    settings.fps = Rational{30, 1};
    settings.outputPath.clear();
    EXPECT_FALSE(validateExportSettings(settings, timeline).isEmpty());
}

TEST(ExportSettings, ValidationAcceptsNtscFractionalRates)
{
    TimelineSettings timeline;
    ExportSettings settings;
    settings.outputPath = QStringLiteral("out.mp4");

    settings.fps = Rational{30000, 1001};
    EXPECT_TRUE(validateExportSettings(settings, timeline).isEmpty());
    settings.fps = Rational{24000, 1001};
    EXPECT_TRUE(validateExportSettings(settings, timeline).isEmpty());
    settings.fps = Rational{240, 2}; // equivalent to 120 fps
    EXPECT_TRUE(validateExportSettings(settings, timeline).isEmpty());
    settings.fps = Rational{121, 1};
    EXPECT_FALSE(validateExportSettings(settings, timeline).isEmpty());
}

TEST(ExportSettings, CodecNamesMapAndParse)
{
    EXPECT_EQ(exportCodecToString(ExportVideoCodec::Mpeg4Part2),
              QStringLiteral("mpeg4"));
    EXPECT_EQ(exportCodecToString(ExportVideoCodec::H264),
              QStringLiteral("h264"));
    EXPECT_EQ(exportCodecToString(ExportVideoCodec::H265),
              QStringLiteral("h265"));

    EXPECT_EQ(exportCodecFfmpegName(ExportVideoCodec::Mpeg4Part2),
              QStringLiteral("mpeg4"));
    EXPECT_EQ(exportCodecFfmpegName(ExportVideoCodec::H264),
              QStringLiteral("h264"));
    // MediaProbe stores avcodec_get_name() output, which is "hevc".
    EXPECT_EQ(exportCodecFfmpegName(ExportVideoCodec::H265),
              QStringLiteral("hevc"));

    EXPECT_EQ(exportCodecDisplayName(ExportVideoCodec::Mpeg4Part2),
              QStringLiteral("MPEG-4 Part 2"));
    EXPECT_EQ(exportCodecDisplayName(ExportVideoCodec::H264),
              QStringLiteral("H.264"));
    EXPECT_EQ(exportCodecDisplayName(ExportVideoCodec::H265),
              QStringLiteral("H.265"));

    ExportVideoCodec codec = ExportVideoCodec::H264;
    EXPECT_TRUE(exportCodecFromString(QStringLiteral("MPEG4"), &codec));
    EXPECT_EQ(codec, ExportVideoCodec::Mpeg4Part2);
    EXPECT_TRUE(exportCodecFromString(QStringLiteral(" h264 "), &codec));
    EXPECT_EQ(codec, ExportVideoCodec::H264);
    EXPECT_TRUE(exportCodecFromString(QStringLiteral("HEVC"), &codec));
    EXPECT_EQ(codec, ExportVideoCodec::H265);
    EXPECT_FALSE(exportCodecFromString(QStringLiteral("vp9"), &codec));
    EXPECT_EQ(codec, ExportVideoCodec::H265); // unchanged on failure
    EXPECT_FALSE(exportCodecFromString(QString(), &codec));
}

TEST(ExportSettings, JsonRejectsMissingCodecAndInvalidFields)
{
    QString error;
    EXPECT_FALSE(exportSettingsFromJson(QJsonObject{}, &error).has_value());
    EXPECT_NE(error.indexOf(QStringLiteral("缺少")), -1);

    QJsonObject unknownCodec;
    unknownCodec.insert(QStringLiteral("codec"), QStringLiteral("vp9"));
    error.clear();
    EXPECT_FALSE(exportSettingsFromJson(unknownCodec, &error).has_value());
    EXPECT_NE(error.indexOf(QStringLiteral("vp9")), -1);

    QJsonObject badFps;
    badFps.insert(QStringLiteral("codec"), QStringLiteral("h264"));
    badFps.insert(QStringLiteral("fpsNum"), 0);
    badFps.insert(QStringLiteral("fpsDen"), 1);
    error.clear();
    EXPECT_FALSE(exportSettingsFromJson(badFps, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    QJsonObject badQuality;
    badQuality.insert(QStringLiteral("codec"), QStringLiteral("h264"));
    badQuality.insert(QStringLiteral("quality"), QStringLiteral("ultra"));
    error.clear();
    EXPECT_FALSE(exportSettingsFromJson(badQuality, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    QJsonObject minimal;
    minimal.insert(QStringLiteral("codec"), QStringLiteral("h265"));
    error = QStringLiteral("stale");
    EXPECT_TRUE(exportSettingsFromJson(minimal, &error).has_value());
    EXPECT_TRUE(error.isEmpty());
}

TEST(ExportSettings, BuiltinPresetIdsAndBitratesMatchContract)
{
    const QList<ExportPreset> presets = builtinExportPresets();
    const QList<QPair<QString, int>> expected = {
        {QStringLiteral("match"), 0},
        {QStringLiteral("h264-1080p30"), 12'000},
        {QStringLiteral("h264-720p30"), 6'000},
        {QStringLiteral("h265-1080p30"), 8'000},
        {QStringLiteral("mpeg4-1080p30"), 10'000},
    };
    ASSERT_EQ(presets.size(), expected.size());
    for (const auto &pair : expected) {
        const auto it = std::find_if(presets.cbegin(), presets.cend(),
                                     [&pair](const ExportPreset &preset) {
                                         return preset.id == pair.first;
                                     });
        ASSERT_NE(it, presets.cend()) << pair.first.toStdString();
        EXPECT_EQ(it->bitrateKbps, pair.second);
    }

    TimelineSettings timeline;
    const auto it = std::find_if(presets.cbegin(), presets.cend(),
                                 [](const ExportPreset &preset) {
                                     return preset.id
                                            == QStringLiteral("h264-720p30");
                                 });
    ASSERT_NE(it, presets.cend());
    const ExportSettings settings = settingsFromPreset(*it, timeline);
    EXPECT_EQ(settings.codec, ExportVideoCodec::H264);
    EXPECT_EQ(settings.width, 1280);
    EXPECT_EQ(settings.height, 720);
    EXPECT_EQ(settings.fps, (Rational{30, 1}));
    EXPECT_EQ(settings.bitrateKbps, 6'000);
    EXPECT_EQ(settings.rateControl, ExportRateControl::Bitrate);
    EXPECT_EQ(settings.quality, ExportQuality::Medium);
    EXPECT_TRUE(settings.outputPath.isEmpty());
}

TEST(ExportSettings, EffectiveBitrateClampsBothRateControlModes)
{
    ExportSettings settings;
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 1;
    EXPECT_EQ(effectiveBitrateKbps(settings), 100);
    settings.bitrateKbps = 10'000'000;
    EXPECT_EQ(effectiveBitrateKbps(settings), 200'000);

    settings.rateControl = ExportRateControl::Quality;
    settings.quality = ExportQuality::Low;
    settings.width = 16;
    settings.height = 16;
    settings.fps = Rational{1, 1};
    EXPECT_EQ(effectiveBitrateKbps(settings), 500);

    settings.quality = ExportQuality::High;
    settings.width = 7680;
    settings.height = 4320;
    settings.fps = Rational{120, 1};
    EXPECT_EQ(effectiveBitrateKbps(settings), 100'000);
}

TEST(ExportSettings, DefaultSrtPathIsNormalized)
{
    EXPECT_EQ(defaultSrtPathFor(QStringLiteral("D:/视频/成片.MP4")),
              QStringLiteral("D:/视频/成片.srt"));
    EXPECT_EQ(defaultSrtPathFor(QStringLiteral("out.mp4")),
              QStringLiteral("out.srt"));
}

TEST(ExportSettings, JsonRoundTripsEveryField)
{
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H265;
    settings.width = 1280;
    settings.height = 720;
    settings.fps = Rational{60, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.quality = ExportQuality::High;
    settings.bitrateKbps = 9000;
    settings.preferHardware = false;
    settings.burnSubtitles = false;
    settings.writeSrt = true;
    settings.outputPath = QStringLiteral("D:/视频/成片.MP4");
    settings.srtPath = QStringLiteral("D:/视频/成片.srt");
    settings.encoderId = QStringLiteral("h264_amf");
    settings.segmentTargetUs = 500'000;

    const QJsonObject json = exportSettingsToJson(settings);
    QString error;
    const auto restored = exportSettingsFromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, settings);
}
