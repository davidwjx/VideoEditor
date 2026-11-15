#include <gtest/gtest.h>

#include <QSignalSpy>

#include "ProjectDocument.h"
#include "TimelineController.h"
#include "TimelineModel.h"
#include "TrackListModel.h"

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

int rowForItem(const TimelineModel *model, const QString &itemId)
{
    for (int i = 0; i < model->rowCount(); ++i) {
        if (model->data(model->index(i, 0), TimelineModel::ItemIdRole).toString()
            == itemId) {
            return i;
        }
    }
    return -1;
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

// Ctrl+B cuts every independent group under the playhead. That must be one
// undo step, not one per group, or a single Ctrl+Z would leave a half split.
TEST(TimelineController, SplitAcrossGroupsIsOneUndoStep)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    Asset second = videoAsset(5'000'000);
    second.id = QStringLiteral("asset-v2");
    document.addAsset(second);
    TimelineController controller(document);
    ASSERT_FALSE(controller
                     .addAssetToTimeline(QStringLiteral("asset-v"),
                                         QStringLiteral("track-v1"), 0)
                     .isEmpty());
    const QString secondTrack = controller.addTrack(0);
    ASSERT_FALSE(secondTrack.isEmpty());
    ASSERT_FALSE(controller
                     .addAssetToTimeline(QStringLiteral("asset-v2"),
                                         secondTrack, 0)
                     .isEmpty());
    ASSERT_EQ(document.trackById(QStringLiteral("track-v1"))->clips.size(), 1);
    ASSERT_EQ(document.trackById(secondTrack)->clips.size(), 1);

    controller.seekUs(2'000'000);
    const int undoBefore = document.undoStack()->index();
    controller.splitAtPlayhead();

    EXPECT_EQ(document.trackById(QStringLiteral("track-v1"))->clips.size(), 2);
    EXPECT_EQ(document.trackById(secondTrack)->clips.size(), 2);
    EXPECT_EQ(document.undoStack()->index(), undoBefore + 1)
        << "splitting two independent groups must be a single undo step";

    document.undo();
    EXPECT_EQ(document.trackById(QStringLiteral("track-v1"))->clips.size(), 1);
    EXPECT_EQ(document.trackById(secondTrack)->clips.size(), 1);
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
    const int undoIndexBefore = document.undoStack()->index();
    const int videoClipsBefore = document.project().tracks.at(0).clips.size();

    QSignalSpy failures(&controller, &TimelineController::editFailed);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"), 0);
    EXPECT_TRUE(clipId.isEmpty());
    EXPECT_EQ(failures.count(), 1);
    // The partially added video clip and its undo step must be rolled back.
    EXPECT_EQ(document.project().tracks.at(0).clips.size(), videoClipsBefore);
    EXPECT_EQ(document.undoStack()->index(), undoIndexBefore);
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
    // 6 s. Neither may be a snap target, otherwise small drags and trims would
    // stick to their current position.
    EXPECT_EQ(controller.snapUs(1'040'000), 1'040'000);
    // The project duration (6 s) coincides with the group end; it must be
    // filtered too, otherwise trimming the clip's tail sticks.
    EXPECT_EQ(controller.snapUs(5'960'000), 5'960'000);
    // Unrelated candidates still attract normally (here, the playhead).
    EXPECT_EQ(controller.snapUs(3'040'000), 3'000'000);
}

TEST(TimelineController, SnapUsIgnoresOriginAndPlayheadOnOwnEdge)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"), 0);
    controller.selectClip(clipId);
    controller.seekUs(0);

    // Origin 0, the playhead (also 0) and the project duration (the clip end)
    // all coincide with the selected group's own edges; none may self-snap.
    EXPECT_EQ(controller.snapUs(40'000), 40'000);

    // Away from the own edges the origin stays filtered, but the playhead is
    // still a legitimate candidate.
    controller.seekUs(2'000'000);
    EXPECT_EQ(controller.snapUs(40'000), 40'000);
    EXPECT_EQ(controller.snapUs(2'040'000), 2'000'000);
}

TEST(TimelineController, UnlinkAndRelinkSelectedCounterpart)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString videoClipId = controller.addAssetToTimeline(
        QStringLiteral("asset-v"), QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(videoClipId.isEmpty());
    ASSERT_EQ(document.project().tracks.at(0).clips.size(), 1);
    ASSERT_EQ(document.project().tracks.at(1).clips.size(), 1);
    const QString audioClipId = document.project().tracks.at(1).clips.first().id;
    const QString originalLink = document.project().tracks.at(0).clips.first().linkId;

    controller.selectClip(videoClipId);
    controller.unlinkSelectedClip();
    EXPECT_TRUE(document.project().tracks.at(0).clips.first().linkId.isEmpty());
    // Only the selected half is unlinked, matching TimelineEditor semantics.
    EXPECT_EQ(document.project().tracks.at(1).clips.first().linkId, originalLink);

    const int undoIndex = document.undoStack()->index();
    EXPECT_TRUE(controller.linkSelectedClip());
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);
    const QString relinked = document.project().tracks.at(0).clips.first().linkId;
    EXPECT_FALSE(relinked.isEmpty());
    EXPECT_EQ(document.project().tracks.at(1).clips.first().linkId, relinked);
    EXPECT_EQ(document.project().tracks.at(0).clips.first().timelineStartUs,
              document.project().tracks.at(1).clips.first().timelineStartUs);

    // Moving the video half moves the audio half again after relinking.
    controller.moveClip(videoClipId, 2'000'000);
    EXPECT_EQ(document.project().tracks.at(1).clips.first().timelineStartUs,
              2'000'000);

    document.undo(); // move
    document.undo(); // link
    EXPECT_TRUE(document.project().tracks.at(0).clips.first().linkId.isEmpty());
    EXPECT_EQ(audioClipId,
              document.project().tracks.at(1).clips.first().id);

    // With no counterpart left there is nothing to relink to.
    controller.selectClip(audioClipId);
    controller.unlinkSelectedClip();
    controller.deleteSelected(false);
    controller.selectClip(videoClipId);
    EXPECT_FALSE(controller.linkSelectedClip())
        << "no counterpart after the audio half was removed";
}

TEST(TimelineController, CueTimingUpdatesModelAndUndo)
{
    ProjectDocument document;
    TimelineController controller(document);
    const QString cueId = controller.addCueAtPlayhead();
    ASSERT_FALSE(cueId.isEmpty());
    controller.setCueText(cueId, QStringLiteral("时间"));
    controller.applySubtitleStyle(QStringLiteral("boxed"), cueId);
    controller.selectCue(cueId);
    const SubtitleCue before = document.project().tracks.at(2).cues.first();
    const int undoIndex = document.undoStack()->index();

    EXPECT_TRUE(controller.setCueTiming(cueId, 1'000'000, 3'500'000));
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);
    const SubtitleCue after = document.project().tracks.at(2).cues.first();
    EXPECT_EQ(after.startUs, 1'000'000);
    EXPECT_EQ(after.endUs, 3'500'000);
    EXPECT_EQ(after.text, before.text);
    EXPECT_EQ(after.styleId, before.styleId);
    EXPECT_EQ(after.positionXPermille, before.positionXPermille);

    const QVariantMap selected = controller.selectedCue();
    EXPECT_EQ(selected.value(QStringLiteral("startUs")).toLongLong(),
              1'000'000);
    EXPECT_EQ(selected.value(QStringLiteral("endUs")).toLongLong(), 3'500'000);

    const int row = rowForItem(controller.timelineModel(), cueId);
    ASSERT_GE(row, 0);
    EXPECT_EQ(controller.timelineModel()
                  ->data(controller.timelineModel()->index(row, 0),
                         TimelineModel::StartUsRole)
                  .toLongLong(),
              1'000'000);
    EXPECT_EQ(controller.timelineModel()
                  ->data(controller.timelineModel()->index(row, 0),
                         TimelineModel::DurationUsRole)
                  .toLongLong(),
              2'500'000);

    EXPECT_FALSE(controller.setCueTiming(cueId, 5'000'000, 4'000'000));
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);
    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.first().startUs, before.startUs);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().endUs, before.endUs);
}

TEST(TimelineController, FrameQuantizeAndTimecodeSeek)
{
    ProjectDocument document;
    TimelineController controller(document);
    // Default project is 30 fps.
    EXPECT_EQ(controller.frameQuantizeUs(0), 0);
    EXPECT_EQ(controller.frameQuantizeUs(40'000), 33'333);
    EXPECT_EQ(controller.frameQuantizeUs(1'040'000), 1'033'333);

    EXPECT_TRUE(controller.seekTimecode(QStringLiteral("00:00:01:15")));
    EXPECT_EQ(controller.positionUs(), 1'500'000);
    EXPECT_TRUE(controller.seekTimecode(QStringLiteral("2.5")));
    EXPECT_EQ(controller.positionUs(), 2'500'000);
    EXPECT_TRUE(controller.seekTimecode(QStringLiteral("01:02.250")));
    EXPECT_EQ(controller.positionUs(), 62'250'000);
    EXPECT_FALSE(controller.seekTimecode(QStringLiteral("not a time")));
    EXPECT_EQ(controller.positionUs(), 62'250'000);
}

TEST(TimelineController, TargetTrackAndOrdinalAudioPairing)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString video2 = controller.addTrack(0);
    const QString audio2 = controller.addTrack(1);
    ASSERT_FALSE(video2.isEmpty());
    ASSERT_FALSE(audio2.isEmpty());
    EXPECT_EQ(controller.targetTrackId(), audio2);

    controller.setTargetTrackId(video2);
    EXPECT_EQ(controller.targetTrackId(), video2);

    const QString clipId = controller.addAssetToTimeline(
        QStringLiteral("asset-v"), video2, 0);
    ASSERT_FALSE(clipId.isEmpty());
    const Project &project = document.project();
    const Track *v2 = document.trackById(video2);
    const Track *a2 = document.trackById(audio2);
    ASSERT_NE(v2, nullptr);
    ASSERT_NE(a2, nullptr);
    ASSERT_EQ(v2->clips.size(), 1);
    ASSERT_EQ(a2->clips.size(), 1);
    EXPECT_EQ(v2->clips.first().linkId, a2->clips.first().linkId);
    EXPECT_EQ(document.trackById(QStringLiteral("track-a1"))->clips.size(), 0);

    // A removed target is cleared instead of leaving a dangling selection.
    controller.removeTrack(video2);
    EXPECT_TRUE(controller.targetTrackId().isEmpty());
    EXPECT_EQ(document.project().tracks.size(), 4);
    Q_UNUSED(project);
}

TEST(TimelineController, TrackUpDownFollowsDisplayOrderWithinType)
{
    ProjectDocument document;
    TimelineController controller(document);
    // Default project is V1/A1/S1; V2 appends at the end of project order but
    // displays above V1 (reverse composition order).
    const QString video2 = controller.addTrack(0);
    ASSERT_FALSE(video2.isEmpty());
    TrackListModel rows;
    const auto displayRow = [&rows](const QString &trackId) {
        for (int i = 0; i < rows.rowCount(); ++i) {
            if (rows.data(rows.index(i, 0), TrackListModel::TrackIdRole)
                    .toString()
                == trackId) {
                return i;
            }
        }
        return -1;
    };
    rows.setProject(document.project());
    EXPECT_EQ(displayRow(video2), 0);
    EXPECT_EQ(displayRow(QStringLiteral("track-v1")), 1);

    int undoIndex = document.undoStack()->index();
    // Up must move the lane visually up: V1 becomes the top video lane, which
    // means it moves later in project/composition order.
    controller.moveTrackUp(QStringLiteral("track-v1"));
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);
    int v1Project = -1;
    int v2Project = -1;
    for (int i = 0; i < document.project().tracks.size(); ++i) {
        if (document.project().tracks.at(i).id == QStringLiteral("track-v1"))
            v1Project = i;
        if (document.project().tracks.at(i).id == video2)
            v2Project = i;
    }
    EXPECT_LT(v2Project, v1Project) << "V2 stays below V1 in composition order";
    rows.setProject(document.project());
    EXPECT_EQ(displayRow(QStringLiteral("track-v1")), 0);
    EXPECT_EQ(displayRow(video2), 1);

    // At the top of the group, up is a no-op and must not add an undo step.
    undoIndex = document.undoStack()->index();
    controller.moveTrackUp(QStringLiteral("track-v1"));
    EXPECT_EQ(document.undoStack()->index(), undoIndex);

    // Down restores the original visual order.
    controller.moveTrackDown(QStringLiteral("track-v1"));
    rows.setProject(document.project());
    EXPECT_EQ(displayRow(video2), 0);
    EXPECT_EQ(displayRow(QStringLiteral("track-v1")), 1);

    // Audio/subtitle use project order for display. A1 is already first in the
    // audio group, so "up" cannot cross into video and must be a no-op; A2 is
    // last, so "down" is likewise inert.
    const QString audio2 = controller.addTrack(1);
    ASSERT_FALSE(audio2.isEmpty());
    undoIndex = document.undoStack()->index();
    controller.moveTrackUp(QStringLiteral("track-a1"));
    controller.moveTrackDown(audio2);
    EXPECT_EQ(document.undoStack()->index(), undoIndex);

    // The bottom video lane cannot move further down.
    controller.moveTrackDown(video2); // V2 -> bottom
    rows.setProject(document.project());
    EXPECT_EQ(displayRow(QStringLiteral("track-v1")), 0);
    EXPECT_EQ(displayRow(video2), 1);
    undoIndex = document.undoStack()->index();
    controller.moveTrackDown(video2);
    EXPECT_EQ(document.undoStack()->index(), undoIndex);
}

TEST(TimelineController, ClipboardIsClearedWhenProjectIsReplaced)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString clipId = controller.addAssetToTimeline(
        QStringLiteral("asset-v"), QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());
    controller.selectClip(clipId);
    controller.copySelected();

    // Loading/replacing the document invalidates the clipboard's source track
    // ids; a later paste must be a no-op instead of resurrecting old clips.
    document.setProject(Project{});
    controller.pasteAtPlayhead();
    EXPECT_TRUE(document.project().tracks.at(0).clips.isEmpty());
    EXPECT_TRUE(document.project().tracks.at(1).clips.isEmpty());
    EXPECT_EQ(document.undoStack()->index(), 0);
}

TEST(TimelineController, MuteAndSoloAreIndependentPerAudioTrack)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString trackA1 = QStringLiteral("track-a1");
    const QString audio2 = controller.addTrack(1);
    ASSERT_FALSE(audio2.isEmpty());

    controller.setTrackMuted(trackA1, true);
    controller.setTrackSolo(audio2, true);
    const Track *a1 = document.trackById(trackA1);
    const Track *a2 = document.trackById(audio2);
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);
    EXPECT_TRUE(a1->muted);
    EXPECT_FALSE(a1->solo);
    EXPECT_TRUE(a2->solo);
    EXPECT_FALSE(a2->muted);
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
