#include <gtest/gtest.h>

#include "Timeline.h"
#include "Project.h"

using namespace VideoEditor;

namespace {

Clip makeClip(int64_t inUs, int64_t outUs, int speedMilli = 1000)
{
    Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = QStringLiteral("asset-1");
    clip.sourceInUs = inUs;
    clip.sourceOutUs = outUs;
    clip.timelineStartUs = 0;
    clip.speedMilli = speedMilli;
    return clip;
}

} // namespace

TEST(Timeline, ComputesDurationFromSpeed)
{
    EXPECT_EQ(makeClip(0, 2'000'000, 1000).timelineDurationUs(), 2'000'000);
    EXPECT_EQ(makeClip(0, 2'000'000, 2000).timelineDurationUs(), 1'000'000);
    EXPECT_EQ(makeClip(0, 2'000'000, 500).timelineDurationUs(), 4'000'000);
    EXPECT_EQ(makeClip(0, 1'000'000, 250).timelineDurationUs(), 4'000'000);
    EXPECT_EQ(makeClip(0, 1'000'000, 4000).timelineDurationUs(), 250'000);
}

TEST(Timeline, MapsTimelineTimeToSourceTimeWithoutFloat)
{
    Clip clip = makeClip(1'000'000, 5'000'000, 2000);
    clip.timelineStartUs = 3'000'000;
    EXPECT_EQ(sourceTimeAt(clip, 2'999'999), 1'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 3'000'000), 1'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 3'500'000), 2'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 5'000'000), 5'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 99'000'000), 5'000'000);
}

TEST(Timeline, SurvivesLongTimelineArithmeticAtFourX)
{
    // 2 hours of source at 4x: 1.8e10 * 4 * 1000 still fits in int64.
    Clip clip = makeClip(0, 7'200'000'000, 4000);
    EXPECT_EQ(clip.timelineDurationUs(), 1'800'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 1'800'000'000), 7'200'000'000);
}

TEST(Timeline, TrackTypeRoundTrips)
{
    EXPECT_EQ(trackTypeFromString(QStringLiteral("video")), TrackType::Video);
    EXPECT_EQ(trackTypeFromString(QStringLiteral("audio")), TrackType::Audio);
    EXPECT_EQ(trackTypeFromString(QStringLiteral("subtitle")), TrackType::Subtitle);
    EXPECT_EQ(trackTypeToString(TrackType::Audio), QStringLiteral("audio"));
    bool ok = true;
    trackTypeFromString(QStringLiteral("nope"), &ok);
    EXPECT_FALSE(ok);
}

TEST(Timeline, EnsureDefaultTracksIsIdempotent)
{
    Project project;
    project.ensureDefaultTracks();
    project.ensureDefaultTracks();
    EXPECT_EQ(project.tracks.size(), 3);
    EXPECT_EQ(project.tracks.at(0).type, TrackType::Video);
    EXPECT_EQ(project.tracks.at(1).type, TrackType::Audio);
    EXPECT_EQ(project.tracks.at(2).type, TrackType::Subtitle);
    EXPECT_EQ(project.subtitleStyles.size(), 1);
    EXPECT_EQ(project.subtitleStyles.first().id, QStringLiteral("default"));
}
