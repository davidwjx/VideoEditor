#include <gtest/gtest.h>

#include "ProjectDocument.h"
#include "TimelineEditor.h"

using namespace VideoEditor;

namespace {

Asset makeAsset(const QString &id, int64_t durationUs)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/%1.mp4").arg(id);
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
    return Asset{id, info.path, info, {}};
}

Clip makeClip(const QString &id, const QString &assetId, int64_t startUs,
              int64_t inUs, int64_t outUs)
{
    Clip clip;
    clip.id = id;
    clip.assetId = assetId;
    clip.timelineStartUs = startUs;
    clip.sourceInUs = inUs;
    clip.sourceOutUs = outUs;
    return clip;
}

struct Fixture {
    ProjectDocument document;
    Asset videoAsset = makeAsset(QStringLiteral("asset-v"), 20'000'000);
    Asset audioAsset = makeAsset(QStringLiteral("asset-a"), 20'000'000);

    Fixture()
    {
        document.setProject(Project{});
        document.addAsset(videoAsset);
        document.addAsset(audioAsset);
    }

    const Track &videoTrack() const { return document.project().tracks.at(0); }
    const Track &audioTrack() const { return document.project().tracks.at(1); }
};

Clip linkedPair(Fixture &fixture, const QString &linkId, int64_t startUs)
{
    TimelineEditor editor(fixture.document);
    Clip video = makeClip(QStringLiteral("clip-v"), QStringLiteral("asset-v"),
                          startUs, 1'000'000, 9'000'000);
    video.linkId = linkId;
    editor.addClip(QStringLiteral("track-v1"), video);
    Clip audio = makeClip(QStringLiteral("clip-a"), QStringLiteral("asset-a"),
                          startUs, 0, 8'000'000);
    audio.linkId = linkId;
    editor.addClip(QStringLiteral("track-a1"), audio);
    return video;
}

void addLinkedPair(Fixture &fixture, const QString &linkId, int64_t startUs,
                   int64_t videoIn, int64_t videoOut, int64_t audioIn,
                   int64_t audioOut, int videoSpeed = 1000, int audioSpeed = 1000)
{
    TimelineEditor editor(fixture.document);
    Clip video = makeClip(QStringLiteral("clip-v"), QStringLiteral("asset-v"),
                          startUs, videoIn, videoOut);
    video.linkId = linkId;
    video.speedMilli = videoSpeed;
    editor.addClip(QStringLiteral("track-v1"), video);
    Clip audio = makeClip(QStringLiteral("clip-a"), QStringLiteral("asset-a"),
                          startUs, audioIn, audioOut);
    audio.linkId = linkId;
    audio.speedMilli = audioSpeed;
    editor.addClip(QStringLiteral("track-a1"), audio);
}

void setTrackLocked(ProjectDocument &document, const QString &trackId, bool locked)
{
    Project project = document.project();
    for (Track &track : project.tracks) {
        if (track.id == trackId)
            track.locked = locked;
    }
    document.setProject(project);
}

} // namespace

TEST(TimelineEditor, AddsClipsAsUndoableCommands)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    Clip clip = makeClip(QStringLiteral("clip-1"), QStringLiteral("asset-v"),
                         1'000'000, 0, 2'000'000);
    EXPECT_EQ(editor.addClip(QStringLiteral("track-v1"), clip),
              QStringLiteral("clip-1"));
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_TRUE(fixture.document.canUndo());
    fixture.document.undo();
    EXPECT_TRUE(fixture.videoTrack().clips.isEmpty());
    fixture.document.redo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
}

TEST(TimelineEditor, MovesLinkedClipsAsOneUndoStep)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2'000'000);

    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 2'000'000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
}

TEST(TimelineEditor, TrimsLinkedClipsAndKeepsSourceAligned)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 1'000'000);

    editor.trimEnd(QStringLiteral("track-a1"), QStringLiteral("clip-a"), 5'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceOutUs, 6'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceOutUs, 5'000'000);
}

TEST(TimelineEditor, RejectsTrimsShorterThanOneFrame)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"),
                     9'000'000); // 1 frame before the end
    const int64_t oneFrame = framesToMicros(1, Rational{30, 1});
    // The shorter linked member (audio 0-8s) bounds the group delta, so both
    // clips stop one frame before their source out and stay aligned.
    const Clip &video = fixture.videoTrack().clips.first();
    const Clip &audio = fixture.audioTrack().clips.first();
    EXPECT_EQ(video.timelineStartUs, 8'000'000 - oneFrame);
    EXPECT_EQ(audio.timelineStartUs, 8'000'000 - oneFrame);
    EXPECT_EQ(video.timelineDurationUs(), oneFrame);
    EXPECT_EQ(audio.timelineDurationUs(), oneFrame);
}

TEST(TimelineEditor, TrimEndKeepsOneFrameAtFourX)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c4"), QStringLiteral("asset-v"),
                            0, 0, 1'000'000));
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("c4"), 4000, false);
    editor.trimEnd(QStringLiteral("track-v1"), QStringLiteral("c4"), 1'000);
    const Clip &clip = fixture.videoTrack().clips.first();
    EXPECT_EQ(clip.speedMilli, 4000);
    EXPECT_GE(clip.timelineDurationUs(), framesToMicros(1, Rational{30, 1}));
}

TEST(TimelineEditor, TrimStartKeepsOneFrameAtQuarterSpeed)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("cq"), QStringLiteral("asset-v"),
                            0, 0, 8'000'000));
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("cq"), 250, false);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("cq"), 32'000'000);
    const Clip &clip = fixture.videoTrack().clips.first();
    EXPECT_EQ(clip.speedMilli, 250);
    EXPECT_GE(clip.timelineDurationUs(), framesToMicros(1, Rational{30, 1}));
}

TEST(TimelineEditor, SplitsLinkedClipsAndRegroupsRightHalves)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 3'000'000);

    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 2);
    const Clip &leftVideo = fixture.videoTrack().clips.at(0);
    const Clip &rightVideo = fixture.videoTrack().clips.at(1);
    EXPECT_EQ(leftVideo.sourceOutUs, 4'000'000);
    EXPECT_EQ(rightVideo.sourceInUs, 4'000'000);
    EXPECT_EQ(rightVideo.timelineStartUs, 3'000'000);
    EXPECT_EQ(rightVideo.linkId, fixture.audioTrack().clips.at(1).linkId);
    EXPECT_NE(rightVideo.linkId, leftVideo.linkId);
    EXPECT_EQ(fixture.audioTrack().clips.at(1).timelineStartUs, 3'000'000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);
}

TEST(TimelineEditor, RippleDeleteShiftsLaterClips)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c1"), QStringLiteral("asset-v"),
                            0, 0, 2'000'000));
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c2"), QStringLiteral("asset-v"),
                            2'000'000, 0, 2'000'000));
    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("c1"), true);

    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.videoTrack().clips.first().id, QStringLiteral("c2"));
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);

    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    EXPECT_EQ(fixture.videoTrack().clips.at(1).timelineStartUs, 2'000'000);
}

TEST(TimelineEditor, CopyPasteCreatesNewIdsAndOffsets)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 1'000'000);
    TimelineEditor editor(fixture.document);
    editor.copy({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("clip-v")}});
    editor.paste(5'000'000, QStringLiteral("track-v1"));

    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    const Clip &pasted = fixture.videoTrack().clips.at(1);
    EXPECT_NE(pasted.id, QStringLiteral("clip-v"));
    EXPECT_EQ(pasted.timelineStartUs, 5'000'000);
    // A copied linked clip keeps its grouping, but the pasted group gets a new
    // link id so it cannot accidentally re-link with the original.
    EXPECT_FALSE(pasted.linkId.isEmpty());
    EXPECT_NE(pasted.linkId, QStringLiteral("link-1"));

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
}

TEST(TimelineEditor, SpeedChangeKeepsLinkedPairsInSync)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2000, true);

    EXPECT_EQ(fixture.videoTrack().clips.first().speedMilli, 2000);
    EXPECT_EQ(fixture.audioTrack().clips.first().speedMilli, 2000);
    EXPECT_TRUE(fixture.audioTrack().clips.first().preservePitch);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineDurationUs(), 4'000'000);
}

TEST(TimelineEditor, SnapUsesNearestEdgeWithinThreshold)
{
    const QList<int64_t> edges{1'000'000, 2'000'000, 5'000'000};
    const SnapResult near = TimelineEditor::snapTime(1'050'000, 100'000, edges);
    EXPECT_TRUE(near.snapped);
    EXPECT_EQ(near.timeUs, 1'000'000);

    const SnapResult far = TimelineEditor::snapTime(1'500'000, 100'000, edges);
    EXPECT_FALSE(far.snapped);
    EXPECT_EQ(far.timeUs, 1'500'000);
}

TEST(TimelineEditor, ReordersTracks)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    Track video2;
    video2.id = QStringLiteral("track-v2");
    video2.type = TrackType::Video;
    video2.name = QStringLiteral("V2");
    editor.addTrack(video2);
    ASSERT_EQ(fixture.document.project().tracks.size(), 4);

    editor.moveTrack(3, 0);
    EXPECT_EQ(fixture.document.project().tracks.first().id, QStringLiteral("track-v2"));
    fixture.document.undo();
    EXPECT_EQ(fixture.document.project().tracks.first().id, QStringLiteral("track-v1"));
}

TEST(TimelineEditor, AddsMultipleTracksInOneUndoStep)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    Clip video = makeClip(QStringLiteral("clip-v2"), QStringLiteral("asset-v"),
                          0, 0, 2'000'000);
    video.linkId = QStringLiteral("link-x");
    Clip audio = makeClip(QStringLiteral("clip-a2"), QStringLiteral("asset-a"),
                          0, 0, 2'000'000);
    audio.linkId = QStringLiteral("link-x");

    const QStringList ids = editor.addClips(
        {{QStringLiteral("track-v1"), video}, {QStringLiteral("track-a1"), audio}});
    EXPECT_EQ(ids, (QStringList{QStringLiteral("clip-v2"), QStringLiteral("clip-a2")}));
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 1);

    fixture.document.undo();
    EXPECT_TRUE(fixture.videoTrack().clips.isEmpty());
    EXPECT_TRUE(fixture.audioTrack().clips.isEmpty());
}

TEST(TimelineEditor, TrimsLinkedClipsAsOneUndoStep)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 1'000'000);
    ASSERT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 1'000'000);
    ASSERT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 0);
}

TEST(TimelineEditor, DeletesLinkedClipsAsOneUndoStep)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), false);

    EXPECT_TRUE(fixture.videoTrack().clips.isEmpty());
    EXPECT_TRUE(fixture.audioTrack().clips.isEmpty());

    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 1);
}

TEST(TimelineEditor, RippleDeleteLinkedPairShiftsBothTracks)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c-v-later"), QStringLiteral("asset-v"),
                            8'000'000, 0, 2'000'000));
    editor.addClip(QStringLiteral("track-a1"),
                   makeClip(QStringLiteral("c-a-later"), QStringLiteral("asset-a"),
                            8'000'000, 0, 2'000'000));

    editor.deleteClip(QStringLiteral("track-a1"), QStringLiteral("clip-a"), true);
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 1);
    EXPECT_EQ(fixture.videoTrack().clips.first().id, QStringLiteral("c-v-later"));
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().id, QStringLiteral("c-a-later"));
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);

    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 2);
    EXPECT_EQ(fixture.videoTrack().clips.at(1).timelineStartUs, 8'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.at(1).timelineStartUs, 8'000'000);
}

TEST(TimelineEditor, LinksAndUnlinksClips)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c-v"), QStringLiteral("asset-v"),
                            0, 0, 4'000'000));
    editor.addClip(QStringLiteral("track-a1"),
                   makeClip(QStringLiteral("c-a"), QStringLiteral("asset-a"),
                            500'000, 0, 4'000'000));

    editor.linkClips({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("c-v")},
                      ClipLocator{QStringLiteral("track-a1"), QStringLiteral("c-a")}});
    const QString linkId = fixture.videoTrack().clips.first().linkId;
    ASSERT_FALSE(linkId.isEmpty());
    EXPECT_EQ(fixture.audioTrack().clips.first().linkId, linkId);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);

    editor.moveClip(QStringLiteral("track-a1"), QStringLiteral("c-a"), 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);

    editor.unlinkClip(QStringLiteral("track-v1"), QStringLiteral("c-v"));
    EXPECT_TRUE(fixture.videoTrack().clips.first().linkId.isEmpty());
    EXPECT_EQ(fixture.audioTrack().clips.first().linkId, linkId);

    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("c-v"), 2'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);
}

TEST(TimelineEditor, RemovesTracks)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.removeTrack(QStringLiteral("track-a1"));
    ASSERT_EQ(fixture.document.project().tracks.size(), 2);
    EXPECT_EQ(fixture.document.trackById(QStringLiteral("track-a1")), nullptr);

    fixture.document.undo();
    ASSERT_EQ(fixture.document.project().tracks.size(), 3);
    EXPECT_NE(fixture.document.trackById(QStringLiteral("track-a1")), nullptr);
}

TEST(TimelineEditor, LinkedTrimStartClampsToShortestMember)
{
    Fixture fixture;
    addLinkedPair(fixture, QStringLiteral("link-1"), 0,
                  0, 8'000'000, 0, 4'000'000);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 4'000'000);

    const int64_t oneFrame = framesToMicros(1, Rational{30, 1});
    const int64_t expectedStart = 4'000'000 - oneFrame;
    const Clip &video = fixture.videoTrack().clips.first();
    const Clip &audio = fixture.audioTrack().clips.first();
    ASSERT_EQ(video.timelineStartUs, expectedStart);
    EXPECT_EQ(audio.timelineStartUs, expectedStart);
    EXPECT_EQ(video.sourceInUs, expectedStart);
    EXPECT_EQ(audio.sourceInUs, expectedStart);
    EXPECT_EQ(video.timelineDurationUs(), 8'000'000 - expectedStart);
    EXPECT_EQ(audio.timelineDurationUs(), oneFrame);
}

TEST(TimelineEditor, TrimStartExtensionReachesSourceZero)
{
    Fixture fixture;
    addLinkedPair(fixture, QStringLiteral("link-1"), 1'000'000,
                  1'000'000, 9'000'000, 1'000'000, 9'000'000);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 0);

    ASSERT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 0);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineDurationUs(), 9'000'000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 1'000'000);
}

TEST(TimelineEditor, TrimStartExtensionBlockedByMemberSourceZero)
{
    Fixture fixture;
    // The audio member is already at source 0, so the group cannot extend.
    addLinkedPair(fixture, QStringLiteral("link-1"), 1'000'000,
                  1'000'000, 9'000'000, 0, 8'000'000);
    TimelineEditor editor(fixture.document);
    const int index = fixture.document.undoStack()->index();
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 0);

    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 0);
    EXPECT_EQ(fixture.document.undoStack()->index(), index);
}

TEST(TimelineEditor, TrimEndClampsToShortestAssetRoom)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.trimEnd(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 50'000'000);

    const Clip &video = fixture.videoTrack().clips.first();
    const Clip &audio = fixture.audioTrack().clips.first();
    EXPECT_EQ(video.sourceOutUs, 20'000'000); // asset room: 20s - 1s in-point
    EXPECT_EQ(audio.sourceOutUs, 19'000'000); // common span bounded by video
    EXPECT_EQ(video.timelineStartUs, 0);
    EXPECT_EQ(audio.timelineStartUs, 0);
    EXPECT_EQ(video.timelineDurationUs(), 19'000'000);
    EXPECT_EQ(audio.timelineDurationUs(), 19'000'000);
}

TEST(TimelineEditor, RedoReappliesGroupEdit)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2'000'000);
    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    ASSERT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);

    fixture.document.redo();
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 2'000'000);
}

TEST(TimelineEditor, UndoRestoresLinkedSpeed)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2000, true);
    ASSERT_EQ(fixture.videoTrack().clips.first().speedMilli, 2000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.first().speedMilli, 1000);
    EXPECT_EQ(fixture.audioTrack().clips.first().speedMilli, 1000);
    EXPECT_FALSE(fixture.videoTrack().clips.first().preservePitch);
    EXPECT_FALSE(fixture.audioTrack().clips.first().preservePitch);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineDurationUs(), 8'000'000);
}

TEST(TimelineEditor, RippleDeleteUsesGroupUnionInterval)
{
    Fixture fixture;
    addLinkedPair(fixture, QStringLiteral("link-1"), 0,
                  0, 8'000'000, 0, 4'000'000);
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("later-v"), QStringLiteral("asset-v"),
                            10'000'000, 0, 2'000'000));
    editor.addClip(QStringLiteral("track-a1"),
                   makeClip(QStringLiteral("later-a"), QStringLiteral("asset-a"),
                            10'000'000, 0, 2'000'000));

    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), true);
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 1);
    EXPECT_EQ(fixture.videoTrack().clips.first().id, QStringLiteral("later-v"));
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().id, QStringLiteral("later-a"));
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 2'000'000);

    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 2);
    EXPECT_EQ(fixture.videoTrack().clips.at(1).timelineStartUs, 10'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.at(1).timelineStartUs, 10'000'000);
}

TEST(TimelineEditor, RippleDeleteKeepsLaterLinkedPairInSync)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("unlinked"), QStringLiteral("asset-v"),
                            0, 0, 2'000'000));
    addLinkedPair(fixture, QStringLiteral("later-link"), 5'000'000,
                  0, 4'000'000, 0, 4'000'000);

    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("unlinked"), true);
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 1);
    const Clip &video = fixture.videoTrack().clips.first();
    const Clip &audio = fixture.audioTrack().clips.first();
    EXPECT_EQ(video.timelineStartUs, 3'000'000);
    EXPECT_EQ(audio.timelineStartUs, 3'000'000);
    EXPECT_EQ(video.linkId, QStringLiteral("later-link"));
    EXPECT_EQ(audio.linkId, QStringLiteral("later-link"));
}

TEST(TimelineEditor, AddClipsRejectsInvalidEntries)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    ASSERT_FALSE(editor.addClip(QStringLiteral("track-v1"),
                                makeClip(QStringLiteral("dup"), QStringLiteral("asset-v"),
                                         0, 0, 2'000'000)).isEmpty());
    // Duplicate existing id.
    EXPECT_TRUE(editor.addClip(QStringLiteral("track-v1"),
                               makeClip(QStringLiteral("dup"), QStringLiteral("asset-v"),
                                        3'000'000, 0, 2'000'000)).isEmpty());
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);

    // Duplicate id inside one batch: only the first is accepted.
    const QStringList batch = editor.addClips(
        {{QStringLiteral("track-a1"),
          makeClip(QStringLiteral("dup-b"), QStringLiteral("asset-a"), 0, 0, 2'000'000)},
         {QStringLiteral("track-a1"),
          makeClip(QStringLiteral("dup-b"), QStringLiteral("asset-a"), 2'000'000, 0, 2'000'000)}});
    EXPECT_EQ(batch, QStringList{QStringLiteral("dup-b")});
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);

    // Negative source in, out beyond known asset duration, empty source range.
    EXPECT_TRUE(editor.addClip(QStringLiteral("track-v1"),
                               makeClip(QStringLiteral("neg"), QStringLiteral("asset-v"),
                                        0, -1, 2'000'000)).isEmpty());
    EXPECT_TRUE(editor.addClip(QStringLiteral("track-v1"),
                               makeClip(QStringLiteral("long"), QStringLiteral("asset-v"),
                                        0, 0, 25'000'000)).isEmpty());
    EXPECT_TRUE(editor.addClip(QStringLiteral("track-v1"),
                               makeClip(QStringLiteral("empty"), QStringLiteral("asset-v"),
                                        0, 2'000'000, 2'000'000)).isEmpty());
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
}

TEST(TimelineEditor, PasteLinkedPairKeepsBothHalvesOnOriginalTracks)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.copy({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("clip-v")},
                 ClipLocator{QStringLiteral("track-a1"), QStringLiteral("clip-a")}});

    // Target a video track; the audio half must still land on A1, not be
    // dropped because its track type differs from the paste target.
    editor.paste(20'000'000, QStringLiteral("track-v1"));

    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 2);
    const Clip &video = fixture.videoTrack().clips.at(1);
    const Clip &audio = fixture.audioTrack().clips.at(1);
    EXPECT_EQ(video.timelineStartUs, 20'000'000);
    EXPECT_EQ(audio.timelineStartUs, 20'000'000);
    EXPECT_FALSE(video.linkId.isEmpty());
    EXPECT_EQ(video.linkId, audio.linkId);
    EXPECT_NE(video.linkId, QStringLiteral("link-1"));

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);
}

TEST(TimelineEditor, PasteLinkedPairRejectsAtomicallyWhenAnyDestinationLocked)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.copy({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("clip-v")},
                 ClipLocator{QStringLiteral("track-a1"), QStringLiteral("clip-a")}});
    setTrackLocked(fixture.document, QStringLiteral("track-a1"), true);
    const int undoIndex = fixture.document.undoStack()->index();

    editor.paste(20'000'000, QStringLiteral("track-v1"));

    EXPECT_EQ(fixture.videoTrack().clips.size(), 1)
        << "the unlocked video half must not be pasted alone";
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);
    EXPECT_EQ(fixture.document.undoStack()->index(), undoIndex);
}

TEST(TimelineEditor, PasteOffsetsAndClampsNegativeStart)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c1"), QStringLiteral("asset-v"),
                            1'000'000, 0, 2'000'000));
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c2"), QStringLiteral("asset-v"),
                            3'000'000, 0, 2'000'000));
    editor.copy({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("c1")},
                 ClipLocator{QStringLiteral("track-v1"), QStringLiteral("c2")}});

    editor.paste(-5'000'000, QStringLiteral("track-v1"));
    ASSERT_EQ(fixture.videoTrack().clips.size(), 4);
    EXPECT_EQ(fixture.videoTrack().clips.at(2).timelineStartUs, 0);
    EXPECT_EQ(fixture.videoTrack().clips.at(3).timelineStartUs, 2'000'000);

    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    editor.paste(5'000'000, QStringLiteral("track-v1"));
    ASSERT_EQ(fixture.videoTrack().clips.size(), 4);
    EXPECT_EQ(fixture.videoTrack().clips.at(2).timelineStartUs, 5'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.at(3).timelineStartUs, 7'000'000);
}

TEST(TimelineEditor, MixedSpeedGroupRejectsTrimAndSplit)
{
    Fixture fixture;
    addLinkedPair(fixture, QStringLiteral("link-1"), 0,
                  0, 8'000'000, 0, 8'000'000, 1000, 2000);
    TimelineEditor editor(fixture.document);
    const int index = fixture.document.undoStack()->index();

    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 1'000'000);
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2'000'000);

    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);
    EXPECT_EQ(fixture.document.undoStack()->index(), index);
}

TEST(TimelineEditor, LinkClipsNormalizesSpeed)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    Clip video = makeClip(QStringLiteral("c-v"), QStringLiteral("asset-v"),
                          0, 0, 8'000'000);
    video.speedMilli = 1000;
    editor.addClip(QStringLiteral("track-v1"), video);
    Clip audio = makeClip(QStringLiteral("c-a"), QStringLiteral("asset-a"),
                          500'000, 0, 8'000'000);
    audio.speedMilli = 2000;
    editor.addClip(QStringLiteral("track-a1"), audio);

    editor.linkClips({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("c-v")},
                      ClipLocator{QStringLiteral("track-a1"), QStringLiteral("c-a")}});
    EXPECT_EQ(fixture.videoTrack().clips.first().speedMilli, 1000);
    EXPECT_EQ(fixture.audioTrack().clips.first().speedMilli, 1000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
    EXPECT_FALSE(fixture.videoTrack().clips.first().linkId.isEmpty());
    EXPECT_EQ(fixture.videoTrack().clips.first().linkId,
              fixture.audioTrack().clips.first().linkId);
}

TEST(TimelineEditor, SplitRejectsWhenAnyMemberCannotSplit)
{
    Fixture fixture;
    addLinkedPair(fixture, QStringLiteral("link-1"), 0,
                  0, 8'000'000, 0, 4'000'000);
    TimelineEditor editor(fixture.document);
    const int index = fixture.document.undoStack()->index();

    // 6s is past the audio member's 4s source out.
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 6'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);

    // A sub-frame split is rejected for the whole group.
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 10);
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);
    EXPECT_EQ(fixture.document.undoStack()->index(), index);
}

TEST(TimelineEditor, LockedTrackRejectsAllEdits)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.copy({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("clip-v")}});
    const AudioParams originalAudio = fixture.videoTrack().clips.first().audio;
    const Transform originalTransform = fixture.videoTrack().clips.first().transform;

    setTrackLocked(fixture.document, QStringLiteral("track-v1"), true);
    ASSERT_EQ(fixture.document.undoStack()->index(), 0);

    EXPECT_TRUE(editor.addClip(QStringLiteral("track-v1"),
                               makeClip(QStringLiteral("locked-add"),
                                        QStringLiteral("asset-v"), 0, 0, 2'000'000)).isEmpty());
    editor.paste(10'000'000, QStringLiteral("track-v1"));
    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 5'000'000);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 1'000'000);
    editor.trimEnd(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2'000'000);
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2'000'000);
    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), false);
    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), true);
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2000, true);
    AudioParams louder = originalAudio;
    louder.volumeMilli = 300;
    editor.setClipAudio(QStringLiteral("track-v1"), QStringLiteral("clip-v"), louder);
    Transform zoomed = originalTransform;
    zoomed.scaleMilli = 500;
    editor.setClipTransform(QStringLiteral("track-v1"), QStringLiteral("clip-v"), zoomed);
    editor.setClipEnabled(QStringLiteral("track-v1"), QStringLiteral("clip-v"), false);
    editor.unlinkClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"));
    editor.linkClips({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("clip-v")},
                      ClipLocator{QStringLiteral("track-a1"), QStringLiteral("clip-a")}});
    editor.removeTrack(QStringLiteral("track-v1"));
    editor.moveTrack(0, 1);

    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    const Clip &video = fixture.videoTrack().clips.first();
    EXPECT_EQ(video.timelineStartUs, 0);
    EXPECT_EQ(video.sourceInUs, 1'000'000);
    EXPECT_EQ(video.sourceOutUs, 9'000'000);
    EXPECT_EQ(video.speedMilli, 1000);
    EXPECT_EQ(video.audio, originalAudio);
    EXPECT_EQ(video.transform, originalTransform);
    EXPECT_TRUE(video.enabled);
    EXPECT_EQ(video.linkId, QStringLiteral("link-1"));
    EXPECT_EQ(fixture.document.project().tracks.size(), 3);
    EXPECT_EQ(fixture.document.project().tracks.first().id, QStringLiteral("track-v1"));
    EXPECT_EQ(fixture.document.undoStack()->index(), 0);
}

TEST(TimelineEditor, NoOpEditsDoNotCreateUndoEntries)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c1"), QStringLiteral("asset-v"),
                            1'000'000, 0, 2'000'000));
    const int index = fixture.document.undoStack()->index();

    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("c1"), 1'000'000);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("c1"), 1'000'000);
    editor.trimEnd(QStringLiteral("track-v1"), QStringLiteral("c1"), 3'000'000);
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("c1"), 1'000'000);
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("c1"), 1000, false);
    editor.setClipAudio(QStringLiteral("track-v1"), QStringLiteral("c1"), AudioParams{});
    editor.setClipTransform(QStringLiteral("track-v1"), QStringLiteral("c1"), Transform{});
    editor.setClipEnabled(QStringLiteral("track-v1"), QStringLiteral("c1"), true);
    editor.unlinkClip(QStringLiteral("track-v1"), QStringLiteral("c1"));
    editor.linkClips({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("c1")}});
    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("missing"), 5'000'000);
    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("missing"), false);
    editor.moveTrack(0, 0);
    editor.removeTrack(QStringLiteral("missing-track"));
    editor.paste(0, QStringLiteral("track-v1"));
    Track duplicate;
    duplicate.id = QStringLiteral("track-v1");
    editor.addTrack(duplicate);

    EXPECT_EQ(fixture.document.undoStack()->index(), index);
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceOutUs, 2'000'000);
    EXPECT_EQ(fixture.document.project().tracks.size(), 3);
}

TEST(TimelineEditor, TrimStartWithPositiveSourceInKeepsValidRange)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c1"), QStringLiteral("asset-v"),
                            0, 1'000'000, 9'000'000));
    const int64_t oneFrame = framesToMicros(1, Rational{30, 1});

    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("c1"), 9'000'000);
    const Clip &clip = fixture.videoTrack().clips.first();
    EXPECT_LT(clip.sourceInUs, clip.sourceOutUs);
    EXPECT_EQ(clip.sourceInUs, 9'000'000 - oneFrame);
    EXPECT_EQ(clip.sourceOutUs, 9'000'000);
    EXPECT_EQ(clip.timelineStartUs, 8'000'000 - oneFrame);
    EXPECT_EQ(clip.timelineDurationUs(), oneFrame);

    fixture.document.undo();
    const Clip &restored = fixture.videoTrack().clips.first();
    EXPECT_EQ(restored.sourceInUs, 1'000'000);
    EXPECT_EQ(restored.timelineStartUs, 0);
    EXPECT_EQ(restored.timelineDurationUs(), 8'000'000);
}

TEST(TimelineEditor, LinkedTrimStartWithPositiveInsKeepsBothValid)
{
    Fixture fixture;
    addLinkedPair(fixture, QStringLiteral("link-1"), 0,
                  1'000'000, 9'000'000, 1'000'000, 9'000'000);
    TimelineEditor editor(fixture.document);
    const int64_t oneFrame = framesToMicros(1, Rational{30, 1});

    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 9'000'000);
    const Clip &video = fixture.videoTrack().clips.first();
    const Clip &audio = fixture.audioTrack().clips.first();
    EXPECT_EQ(video.sourceInUs, 9'000'000 - oneFrame);
    EXPECT_EQ(audio.sourceInUs, 9'000'000 - oneFrame);
    EXPECT_EQ(video.sourceOutUs, 9'000'000);
    EXPECT_EQ(audio.sourceOutUs, 9'000'000);
    EXPECT_EQ(video.timelineStartUs, 8'000'000 - oneFrame);
    EXPECT_EQ(audio.timelineStartUs, 8'000'000 - oneFrame);
    EXPECT_EQ(video.timelineDurationUs(), oneFrame);
    EXPECT_EQ(audio.timelineDurationUs(), oneFrame);
    EXPECT_EQ(video.linkId, audio.linkId);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
}

TEST(TimelineEditor, TrimStartBeyondOneFrameMinimumIsNoOp)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c1"), QStringLiteral("asset-v"),
                            0, 1'000'000, 9'000'000));
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("c1"), 9'000'000);
    const Clip before = fixture.videoTrack().clips.first();
    const int index = fixture.document.undoStack()->index();

    // One frame already remains; trimming in any further cannot be satisfied
    // without crossing the source out point.
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("c1"), 9'000'000);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("c1"), 20'000'000);

    EXPECT_EQ(fixture.document.undoStack()->index(), index);
    EXPECT_EQ(fixture.videoTrack().clips.first(), before);
}

