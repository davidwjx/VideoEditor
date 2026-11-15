#include <gtest/gtest.h>

#include <QJsonObject>

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
