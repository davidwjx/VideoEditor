#include <algorithm>

#include <gtest/gtest.h>

#include "TimelineEvaluator.h"

using namespace VideoEditor;

namespace {

Clip clip(const QString &id, const QString &asset, int64_t startUs, int64_t inUs,
          int64_t outUs, int speedMilli = 1000)
{
    Clip value;
    value.id = id;
    value.assetId = asset;
    value.timelineStartUs = startUs;
    value.sourceInUs = inUs;
    value.sourceOutUs = outUs;
    value.speedMilli = speedMilli;
    return value;
}

Project projectWithTracks()
{
    Project project;
    project.timeline.fps = Rational{30, 1};
    Track bottom;
    bottom.id = QStringLiteral("v1");
    bottom.name = QStringLiteral("V1");
    bottom.type = TrackType::Video;
    bottom.clips.append(clip(QStringLiteral("v1-c1"), QStringLiteral("a1"), 0,
                             0, 2'000'000));
    Track top;
    top.id = QStringLiteral("v2");
    top.name = QStringLiteral("V2");
    top.type = TrackType::Video;
    top.clips.append(clip(QStringLiteral("v2-c1"), QStringLiteral("a2"), 1'000'000,
                          0, 4'000'000));
    Track audio;
    audio.id = QStringLiteral("a1");
    audio.name = QStringLiteral("A1");
    audio.type = TrackType::Audio;
    audio.clips.append(clip(QStringLiteral("a1-c1"), QStringLiteral("a3"), 0,
                            0, 2'000'000));
    Track subs;
    subs.id = QStringLiteral("s1");
    subs.name = QStringLiteral("S1");
    subs.type = TrackType::Subtitle;
    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 500'000;
    cue.endUs = 1'500'000;
    cue.text = QStringLiteral("你好");
    subs.cues.append(cue);
    project.tracks = {bottom, top, audio, subs};
    project.subtitleStyles.append(SubtitleStyle{});
    return project;
}

} // namespace

TEST(TimelineEvaluator, OrdersVideoLayersBottomToTop)
{
    const Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(1'200'000);
    ASSERT_EQ(frame.video.size(), 2);
    EXPECT_EQ(frame.video.at(0).trackId, QStringLiteral("v1"));
    EXPECT_EQ(frame.video.at(1).trackId, QStringLiteral("v2"));
    EXPECT_EQ(frame.video.at(0).sourceTimeUs, 1'200'000);
    EXPECT_EQ(frame.video.at(1).sourceTimeUs, 200'000);
}

TEST(TimelineEvaluator, ReturnsGapsAndSkipsDisabledTracks)
{
    Project project = projectWithTracks();
    TimelineEvaluator evaluator(project);
    EXPECT_TRUE(evaluator.frameAt(2'500'000).video.size() == 1); // only V2
    EXPECT_EQ(evaluator.frameAt(2'500'000).video.first().trackId,
              QStringLiteral("v2"));
    EXPECT_TRUE(evaluator.frameAt(6'000'000).video.isEmpty());

    project.tracks[1].enabled = false;
    TimelineEvaluator disabled(project);
    EXPECT_TRUE(disabled.frameAt(2'500'000).video.isEmpty());
}

TEST(TimelineEvaluator, SubtitlesAreIndependentAndOnTop)
{
    const Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(1'000'000);
    ASSERT_EQ(frame.subtitles.size(), 1);
    EXPECT_EQ(frame.subtitles.first().text, QStringLiteral("你好"));
    EXPECT_EQ(frame.subtitles.first().style.id, QStringLiteral("default"));
    EXPECT_TRUE(evaluator.frameAt(2'000'000).subtitles.isEmpty());
}

TEST(TimelineEvaluator, AudioRangeIntersectsClips)
{
    const Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    const QList<AudioLayer> layers = evaluator.audioRange(1'500'000, 1'600'000);
    ASSERT_EQ(layers.size(), 1);
    EXPECT_EQ(layers.first().clipId, QStringLiteral("a1-c1"));
    EXPECT_EQ(layers.first().sourceTimeUs, 1'500'000);
    EXPECT_TRUE(evaluator.audioRange(2'000'000, 2'100'000).isEmpty());
}

TEST(TimelineEvaluator, AppliesSpeedToSourceMapping)
{
    Project project = projectWithTracks();
    project.tracks[1].clips[0].speedMilli = 2000;
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(2'000'000);
    const auto it = std::find_if(frame.video.cbegin(), frame.video.cend(),
                                 [](const VideoLayer &layer) {
                                     return layer.trackId == QStringLiteral("v2");
                                 });
    ASSERT_NE(it, frame.video.cend());
    // 2x speed: one second of timeline consumes two seconds of source.
    EXPECT_EQ(it->sourceTimeUs, 2'000'000);
    EXPECT_EQ(it->localTimeUs, 1'000'000);
}

TEST(TimelineEvaluator, ReportsTimelineDuration)
{
    Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    EXPECT_EQ(evaluator.durationUs(), 5'000'000); // V2 ends at 1s + 4s
    project.tracks[3].cues[0].endUs = 9'000'000;
    const TimelineEvaluator longer(project);
    EXPECT_EQ(longer.durationUs(), 9'000'000);
}

TEST(TimelineEvaluator, PicksLastOverlappingClipInTrackOrder)
{
    Project project;
    Track track;
    track.id = QStringLiteral("v1");
    track.type = TrackType::Video;
    track.clips.append(clip(QStringLiteral("first"), QStringLiteral("a1"), 0, 0, 4'000'000));
    track.clips.append(clip(QStringLiteral("second"), QStringLiteral("a1"), 1'000'000, 0, 4'000'000));
    project.tracks.append(track);
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(1'500'000);
    ASSERT_EQ(frame.video.size(), 1);
    EXPECT_EQ(frame.video.first().clipId, QStringLiteral("second"));
}

TEST(TimelineEvaluator, PropagatesMuteAndSoloFlags)
{
    Project project = projectWithTracks();
    project.tracks[2].muted = true;
    project.tracks[2].solo = true;
    const TimelineEvaluator evaluator(project);
    const QList<AudioLayer> layers = evaluator.audioRange(500'000, 600'000);
    ASSERT_EQ(layers.size(), 1);
    EXPECT_TRUE(layers.first().trackMuted);
    EXPECT_TRUE(layers.first().trackSolo);
    EXPECT_EQ(layers.first().clipId, QStringLiteral("a1-c1"));
}

TEST(TimelineEvaluator, ExcludesDisabledAudioContent)
{
    Project trackDisabled = projectWithTracks();
    trackDisabled.tracks[2].enabled = false;
    const TimelineEvaluator trackEvaluator(trackDisabled);
    EXPECT_TRUE(trackEvaluator.audioRange(0, 1'000'000).isEmpty());

    Project clipDisabled = projectWithTracks();
    clipDisabled.tracks[2].clips[0].enabled = false;
    const TimelineEvaluator clipEvaluator(clipDisabled);
    EXPECT_TRUE(clipEvaluator.audioRange(0, 1'000'000).isEmpty());
}

TEST(TimelineEvaluator, ResolvesBuiltinPresetWhenProjectOnlyHasDefault)
{
    Project project = projectWithTracks();
    project.subtitleStyles = {SubtitleStyle{}}; // v2 file written before presets
    project.tracks[3].cues[0].styleId = QStringLiteral("boxed");
    const FrameComposition frame =
        TimelineEvaluator(project).frameAt(1'000'000);
    ASSERT_EQ(frame.subtitles.size(), 1);
    EXPECT_EQ(frame.subtitles.first().style.id, QStringLiteral("boxed"));
    EXPECT_EQ(frame.subtitles.first().style.backgroundArgb, 0xB0000000u);
    EXPECT_EQ(frame.subtitles.first().style.outlineWidth, 1);
}

TEST(TimelineEvaluator, AnySoloIsProjectWideAcrossGaps)
{
    Project project = projectWithTracks();
    project.tracks[2].solo = true; // solo track's clip spans 0..2s
    EXPECT_TRUE(TimelineEvaluator(project).anySolo());

    // Past the solo clip's end the block has no solo layer, but solo state
    // must still mute the other tracks.
    EXPECT_TRUE(TimelineEvaluator(project).audioRange(2'500'000, 2'600'000)
                    .isEmpty());

    project.tracks[2].enabled = false;
    EXPECT_FALSE(TimelineEvaluator(project).anySolo());
}

TEST(TimelineEvaluator, ExcludesDisabledSubtitleCue)
{
    Project project = projectWithTracks();
    project.tracks[3].cues[0].enabled = false;
    const TimelineEvaluator evaluator(project);
    EXPECT_TRUE(evaluator.frameAt(1'000'000).subtitles.isEmpty());
}
