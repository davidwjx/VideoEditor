#include <gtest/gtest.h>

#include <QSignalSpy>

#include "ProjectDocument.h"
#include "TimelineController.h"
#include "TimelineModel.h"

using namespace VideoEditor;

namespace {

Asset videoAsset(int64_t durationUs)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/a.mp4");
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = durationUs;
    info.hasAudio = true;
    info.hasVideo = true;
    info.kind = MediaKind::Video;
    info.supported = true;
    return Asset{QStringLiteral("asset-v"), info.path, info, {}};
}

} // namespace

TEST(TimelineController, AddsVideoAndLinkedAudioAtPlayhead)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.seekUs(1'000'000);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"),
                                      controller.positionUs());
    ASSERT_FALSE(clipId.isEmpty());
    ASSERT_EQ(document.project().tracks.at(0).clips.size(), 1);
    ASSERT_EQ(document.project().tracks.at(1).clips.size(), 1);
    EXPECT_EQ(document.project().tracks.at(1).clips.first().linkId,
              document.project().tracks.at(0).clips.first().linkId);
    EXPECT_EQ(controller.timelineModel()->rowCount(), 2);
}

TEST(TimelineController, SplitAtPlayheadAndUndo)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                  QStringLiteral("track-v1"), 0);
    controller.seekUs(2'000'000);
    controller.splitAtPlayhead();
    EXPECT_EQ(document.project().tracks.at(0).clips.size(), 2);
    EXPECT_EQ(document.project().tracks.at(1).clips.size(), 2);

    document.undo();
    EXPECT_EQ(document.project().tracks.at(0).clips.size(), 1);
}

TEST(TimelineController, SnapUsUsesPlayheadAndClipEdges)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                  QStringLiteral("track-v1"), 0);
    controller.seekUs(3'000'000);
    EXPECT_EQ(controller.snapUs(3'040'000), 3'000'000);
    EXPECT_EQ(controller.snapUs(1'500'000), 1'500'000);
}

TEST(TimelineController, AddAssetReportsPartialAddOnLockedTrack)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.setTrackLocked(QStringLiteral("track-a1"), true);

    QSignalSpy failures(&controller, &TimelineController::editFailed);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"), 0);
    EXPECT_TRUE(clipId.isEmpty());
    EXPECT_EQ(failures.count(), 1);
}

TEST(TimelineController, SnapUsExcludesSelectedClipAndLinkedTwin)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"), 1'000'000);
    controller.selectClip(clipId);
    controller.seekUs(3'000'000);

    // The dragged clip and its linked audio twin both start at 1 s and end at
    // 6 s. Neither may act as a snap target, otherwise small drags and trims
    // would stick to their current position.
    EXPECT_EQ(controller.snapUs(1'040'000), 1'040'000);
    // Unrelated candidates still attract normally (here, the playhead).
    EXPECT_EQ(controller.snapUs(3'040'000), 3'000'000);
}

TEST(TimelineController, EditsCueTextAndSpeedThroughCommands)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"), 0);
    controller.setClipSpeed(clipId, 2000, true);
    EXPECT_EQ(document.project().tracks.at(0).clips.first().speedMilli, 2000);
    EXPECT_TRUE(document.project().tracks.at(1).clips.first().preservePitch);

    controller.addCueAtPlayhead();
    ASSERT_EQ(document.project().tracks.at(2).cues.size(), 1);
    const QString cueId = document.project().tracks.at(2).cues.first().id;
    controller.setCueText(cueId, QStringLiteral("新字幕"));
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("新字幕"));
}
