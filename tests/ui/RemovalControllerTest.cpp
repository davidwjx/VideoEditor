#include <gtest/gtest.h>

#include <QPointF>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "MaskSidecarStore.h"
#include "ProjectDocument.h"
#include "RemovalController.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

namespace {

Project projectWithAudioOnlyClip()
{
    MediaInfo info;
    info.path = QStringLiteral("C:/音频/示例.mp3");
    info.durationUs = 10'000'000;
    info.supported = true;
    info.hasAudio = true;
    info.kind = MediaKind::Audio;

    Project project;
    project.assets.append(
        Asset{QStringLiteral("asset-a"), info.path, info, {}});
    Track track;
    track.id = QStringLiteral("track-a");
    track.type = TrackType::Audio;
    Clip clip;
    clip.id = QStringLiteral("clip-a");
    clip.assetId = QStringLiteral("asset-a");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 10'000'000;
    track.clips.append(clip);
    project.tracks.append(track);
    project.ensureSubtitleStyles();
    return project;
}

Project projectWithMappedClip(int speedMilli)
{
    Project project = projectWithClip();
    Clip &clip = project.tracks[0].clips[0];
    clip.sourceInUs = 1'000'000;
    clip.sourceOutUs = 9'000'000;
    clip.speedMilli = speedMilli;
    return project;
}

} // namespace

TEST(RemovalController, StrokeCreatesOneUndoCommandAndUndoRestoresMask)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(controller.maskMode());
    const QString before = controller.currentMaskHash();

    controller.beginStroke(20, false);
    controller.continueStroke(QPointF(0.4, 0.4), QPointF(0.6, 0.6));
    controller.endStroke();

    EXPECT_NE(controller.currentMaskHash(), before);
    EXPECT_TRUE(doc.canUndo());
    EXPECT_EQ(doc.project().removalLayers.size(), 1);

    doc.undo();
    EXPECT_EQ(controller.currentMaskHash(), before);
    doc.redo();
    EXPECT_NE(controller.currentMaskHash(), before);
}

TEST(RemovalController, FeatherChangeDoesNotChangeMaskHash)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    const QString maskHash = controller.currentMaskHash();

    controller.setFeatherPx(24);
    EXPECT_EQ(controller.currentMaskHash(), maskHash);
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().removalLayers[0].maskKeyframes[0].featherPx, 24);
}

TEST(RemovalController, AddAndDeleteKeyframesStaySorted)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();

    // 测试直接调用控制器的时间点版本，避免依赖 QML 播放头。
    controller.addMaskKeyframeAtSourceTime(0, QStringLiteral("mk-a"));
    controller.addMaskKeyframeAtSourceTime(2'000'000, QStringLiteral("mk-b"));
    controller.addMaskKeyframeAtSourceTime(1'000'000, QStringLiteral("mk-c"));
    const auto layer = doc.project().removalLayers.first();
    EXPECT_EQ(layer.maskKeyframes.size(), 3);
    EXPECT_EQ(layer.maskKeyframes[1].sourceTimeUs, 1'000'000);

    controller.removeMaskKeyframe(1);
    EXPECT_EQ(doc.project().removalLayers.first().maskKeyframes.size(), 2);
}

TEST(RemovalController, EmptyStrokePushesNoUndoCommand)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    const int index = doc.undoStack()->index();

    // A press/release without movement must not create a layer.
    controller.beginStroke(10, false);
    controller.endStroke();
    EXPECT_EQ(doc.undoStack()->index(), index);
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());

    // Erasing an already-empty mask leaves the hash unchanged.
    controller.beginStroke(10, true);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    EXPECT_EQ(doc.undoStack()->index(), index);
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());
}

TEST(RemovalController, FeatherUndoRestoresFeatherWithoutTouchingMask)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    const QString hash = controller.currentMaskHash();
    controller.setFeatherPx(24);
    ASSERT_EQ(doc.project().removalLayers.first().maskKeyframes.first().featherPx,
              24);

    doc.undo();
    EXPECT_EQ(controller.currentMaskHash(), hash);
    EXPECT_EQ(controller.featherPx(), 4);
    ASSERT_EQ(doc.project().removalLayers.first().maskKeyframes.first().featherPx,
              4);

    doc.redo();
    EXPECT_EQ(controller.currentMaskHash(), hash);
    EXPECT_EQ(controller.featherPx(), 24);
}

TEST(RemovalController, DeleteLayerClearsMaskModeAndToolsAndUndoRestores)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    controller.setBoxMode(true);
    controller.setEraseMode(true);

    controller.deleteLayer();
    EXPECT_FALSE(controller.maskMode());
    EXPECT_FALSE(controller.hasLayer());
    EXPECT_FALSE(controller.boxMode());
    EXPECT_FALSE(controller.eraseMode());
    EXPECT_TRUE(controller.activeClipId().isEmpty());
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());

    doc.undo();
    EXPECT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_FALSE(controller.maskMode());

    doc.redo();
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());
}

TEST(RemovalController, DeletingLastKeyframeReportsExactError)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    controller.addMaskKeyframeAtSourceTime(1'000'000,
                                           QStringLiteral("mk-second"));
    ASSERT_EQ(doc.project().removalLayers.first().maskKeyframes.size(), 2);

    // Removing the second-to-last keyframe is allowed.
    controller.removeMaskKeyframe(0);
    controller.clearError();
    ASSERT_EQ(doc.project().removalLayers.first().maskKeyframes.size(), 1);

    controller.removeMaskKeyframe(0);
    EXPECT_EQ(controller.error(), QStringLiteral("至少保留一个遮罩关键帧。"));
    EXPECT_EQ(doc.project().removalLayers.first().maskKeyframes.size(), 1);
}

TEST(RemovalController, AudioOnlyClipIsRejected)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithAudioOnlyClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-a"));
    EXPECT_FALSE(controller.maskMode());
    EXPECT_EQ(controller.error(),
              QStringLiteral("只能对视频片段使用 AI 消除。"));
}

TEST(RemovalController, SourceTimeMappingHonorsSourceInAndSpeed)
{
    QTemporaryDir dir;
    for (const int speedMilli : {2000, 500}) {
        ProjectDocument doc;
        doc.setProject(projectWithMappedClip(speedMilli));
        MaskSidecarStore sidecars(
            dir.filePath(QStringLiteral("mapped-%1.veproj").arg(speedMilli)));
        RemovalController controller(doc, sidecars);
        // 1 s into a clip whose source starts at 1 s.
        controller.setPlayheadProvider([] { return qint64(1'000'000); });

        controller.beginMask(QStringLiteral("clip-1"));
        controller.beginStroke(10, false);
        controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
        controller.endStroke();

        ASSERT_EQ(doc.project().removalLayers.size(), 1);
        const int64_t expected =
            1'000'000 + (1'000'000 * int64_t(speedMilli) / 1000);
        EXPECT_EQ(doc.project()
                      .removalLayers.first()
                      .maskKeyframes.first()
                      .sourceTimeUs,
                  expected);
    }
}

TEST(RemovalController, UndoWhileMaskModeOffRefreshesKeyframeList)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    controller.endMask();
    ASSERT_EQ(controller.maskKeyframesForClip(QStringLiteral("clip-1")).size(),
              1);

    QSignalSpy spy(&controller, &RemovalController::layerChanged);
    ASSERT_TRUE(spy.isValid());
    doc.undo();
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(controller.maskKeyframesForClip(QStringLiteral("clip-1")).isEmpty());
}

TEST(RemovalController, RemovingActiveClipExitsMaskMode)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    EXPECT_TRUE(controller.maskMode());

    // applyTrackEdit emits changed without projectReplaced, exercising the
    // controller's own active-clip guard.
    doc.applyTrackEdit(QStringLiteral("track-v"), {}, {});
    EXPECT_FALSE(controller.maskMode());
    EXPECT_TRUE(controller.activeClipId().isEmpty());
}

TEST(RemovalController, EmptyMaskCannotBecomeAKeyframe)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(controller.maskMode());
    EXPECT_TRUE(controller.currentMaskEmpty());
    EXPECT_FALSE(controller.maskHasPixels());

    const int keyframesBefore = controller.maskKeyframes().size();
    controller.addMaskKeyframeAtPlayhead();
    EXPECT_EQ(controller.maskKeyframes().size(), keyframesBefore)
        << "an all-zero mask must not be addable";
    EXPECT_TRUE(controller.error().contains(QStringLiteral("遮罩为空")))
        << controller.error().toStdString();

    // The first stroke paints pixels and creates the first keyframe.
    controller.clearError();
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.4, 0.4), QPointF(0.6, 0.6));
    controller.endStroke();
    EXPECT_TRUE(controller.maskHasPixels());
    EXPECT_FALSE(controller.currentMaskEmpty());
    EXPECT_FALSE(controller.maskKeyframes().isEmpty());
}
