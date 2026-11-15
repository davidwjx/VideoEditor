#include <gtest/gtest.h>

#include <memory>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "AppController.h"
#include "AudioEngine.h"
#include "AudioSink.h"
#include "PreviewController.h"
#include "ProjectDocument.h"
#include "ProjectSerializer.h"
#include "TimelineController.h"
#include "TimelineModel.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

AudioEngine::SinkFactory nullSinkFactory()
{
    return [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
}

const Clip *findClip(const Project &project, const QString &clipId)
{
    for (const Track &track : project.tracks) {
        for (const Clip &clip : track.clips) {
            if (clip.id == clipId)
                return &clip;
        }
    }
    return nullptr;
}

const SubtitleCue *findCue(const Project &project, const QString &cueId)
{
    for (const Track &track : project.tracks) {
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id == cueId)
                return &cue;
        }
    }
    return nullptr;
}

Asset audioAsset(int64_t durationUs)
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

QString addAudioClip(TimelineController &controller)
{
    Clip clip;
    clip.id = QStringLiteral("clip-a");
    clip.assetId = QStringLiteral("asset-v");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 5'000'000;
    clip.timelineStartUs = 0;
    return controller.addClipForTest(QStringLiteral("track-a1"), clip);
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

QVariantList keyframeList(const TimelineModel *model, int row)
{
    return model->data(model->index(row, 0), TimelineModel::KeyframesRole)
        .toList();
}

} // namespace

TEST(TimelineUi, ControllerIsExposedAndEditable)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    ASSERT_NE(controller.timeline(), nullptr);
    ASSERT_NE(controller.timeline()->timelineModel(), nullptr);
    EXPECT_NE(controller.audioEngine(), nullptr);

    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;

    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());
    EXPECT_EQ(controller.timeline()->timelineModel()->rowCount(), 2); // video + audio

    controller.timeline()->seekUs(1'000'000);
    controller.timeline()->splitAtPlayhead();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 2);
    controller.undo();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 1);
}

TEST(TimelineUi, ImportsSrtAndExportsIt)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString srtPath = dir.filePath(QStringLiteral("in.srt"));
    QFile file(srtPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("1\n00:00:00,000 --> 00:00:01,000\n字幕测试\n");
    file.close();

    EXPECT_EQ(controller.timeline()->importSrtFile(srtPath), QStringLiteral("1"));
    ASSERT_EQ(controller.document()->project().tracks.at(2).cues.size(), 1);

    const QString outPath = dir.filePath(QStringLiteral("out.srt"));
    ASSERT_TRUE(controller.timeline()->exportSrtFile(outPath));
    QFile out(outPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_TRUE(out.readAll().contains(QStringLiteral("字幕测试").toUtf8()));
}

TEST(TimelineUi, OpenTimelineSwitchesPreviewMode)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    controller.timeline()->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0);
    controller.refreshTimelinePreview();
    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_GT(controller.preview()->durationUs(), 0);
}

TEST(TimelineUi, AppAddsAssetSeeksAndExposesSubtitleStyles)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;

    controller.addAssetToTimeline(assetId);
    const Project &project = controller.document()->project();
    ASSERT_EQ(project.tracks.at(0).clips.size(), 1);
    ASSERT_EQ(project.tracks.at(1).clips.size(), 1);
    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_GT(controller.preview()->durationUs(), 0);

    controller.seekTimeline(1'500'000);
    EXPECT_EQ(controller.timeline()->positionUs(), 1'500'000);
    EXPECT_EQ(controller.preview()->positionMs(), 1500);
    EXPECT_FALSE(controller.preview()->playing());

    const QVariantList styles = controller.subtitleStyles();
    ASSERT_EQ(styles.size(), 3);
    QSet<QString> ids;
    for (const QVariant &value : styles) {
        const QVariantMap map = value.toMap();
        EXPECT_FALSE(map.value(QStringLiteral("id")).toString().isEmpty());
        EXPECT_FALSE(map.value(QStringLiteral("name")).toString().isEmpty());
        ids.insert(map.value(QStringLiteral("id")).toString());
    }
    EXPECT_TRUE(ids.contains(QStringLiteral("default")));
    EXPECT_TRUE(ids.contains(QStringLiteral("boxed")));
    EXPECT_TRUE(ids.contains(QStringLiteral("bold-outline")));

    // Double-clicking media opens the asset and then adds it to the timeline;
    // toggling the proxy afterwards must not jump out of timeline mode.
    controller.openAsset(assetId);
    EXPECT_FALSE(controller.preview()->timelineMode());
    controller.addAssetToTimeline(assetId);
    EXPECT_TRUE(controller.preview()->timelineMode());
    controller.setUseProxy(false);
    EXPECT_FALSE(controller.useProxy());
    // The flag must reach the compositor worker, not just the asset preview;
    // toggling must not leave timeline mode.
    EXPECT_FALSE(controller.preview()->useProxy());
    EXPECT_TRUE(controller.preview()->timelineMode());
    controller.setUseProxy(true);
    EXPECT_TRUE(controller.preview()->useProxy());
    EXPECT_TRUE(controller.preview()->timelineMode());
}

TEST(TimelineUi, AddingAssetKeepsPlayheadAndTargetTrack)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;

    controller.addAssetToTimeline(assetId);
    ASSERT_TRUE(controller.preview()->timelineMode());
    controller.seekTimeline(1'500'000);

    // Adding another clip must not reset the playhead to zero.
    controller.addAssetToTimeline(assetId, QStringLiteral("track-v1"));
    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_EQ(controller.timeline()->positionUs(), 1'500'000);
    EXPECT_EQ(controller.preview()->positionMs(), 1500);
    EXPECT_FALSE(controller.preview()->playing());
}

TEST(TimelineUi, AudioAssetGoesToTargetTrackAndVideoAudioPairsByOrdinal)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    const QString video = fixture(QStringLiteral("h264.mp4"));
    const QString music = fixture(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(video) || !QFileInfo::exists(music))
        GTEST_SKIP() << "missing fixtures";
    controller.importUrls({QUrl::fromLocalFile(video),
                           QUrl::fromLocalFile(music)});
    ASSERT_EQ(controller.document()->project().assets.size(), 2);
    const QString videoId = controller.document()->project().assets.at(0).id;
    const QString musicId = controller.document()->project().assets.at(1).id;

    const QString video2 = controller.timeline()->addTrack(0);
    const QString audio2 = controller.timeline()->addTrack(1);
    ASSERT_FALSE(video2.isEmpty());
    ASSERT_FALSE(audio2.isEmpty());

    // Original audio follows the chosen video lane (V2 -> A2).
    controller.addAssetToTimeline(videoId, video2);
    ASSERT_EQ(controller.document()->trackById(video2)->clips.size(), 1);
    ASSERT_EQ(controller.document()->trackById(audio2)->clips.size(), 1);
    EXPECT_TRUE(controller.document()
                    ->trackById(QStringLiteral("track-a1"))
                    ->clips.isEmpty());

    // BGM is an audio-only asset: the target track wins.
    controller.addAssetToTimeline(musicId, QStringLiteral("track-a1"));
    ASSERT_EQ(controller.document()->trackById(QStringLiteral("track-a1"))
                  ->clips.size(),
              1);
    EXPECT_EQ(controller.document()->trackById(QStringLiteral("track-a1"))
                  ->clips.first().assetId,
              musicId);
    EXPECT_EQ(controller.document()->trackById(audio2)->clips.first().assetId,
              videoId);
}

TEST(TimelineUi, SingleFieldSettersPreserveSiblingProperties)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());

    controller.timeline()->setClipTransform(clipId, 1000, 250, -300, 800);
    controller.timeline()->setClipVolume(clipId, 1500);
    controller.timeline()->setClipPan(clipId, -200);

    controller.timeline()->setClipScale(clipId, 1500);
    const Clip *clip = findClip(controller.document()->project(), clipId);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->transform.scaleMilli, 1500);
    EXPECT_EQ(clip->transform.offsetXPermille, 250);
    EXPECT_EQ(clip->transform.offsetYPermille, -300);
    EXPECT_EQ(clip->transform.opacityMilli, 800);
    EXPECT_EQ(clip->audio.volumeMilli, 1500);
    EXPECT_EQ(clip->audio.panMilli, -200);

    controller.timeline()->setClipOpacity(clipId, 400);
    clip = findClip(controller.document()->project(), clipId);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->transform.scaleMilli, 1500);
    EXPECT_EQ(clip->transform.offsetXPermille, 250);
    EXPECT_EQ(clip->transform.offsetYPermille, -300);
    EXPECT_EQ(clip->transform.opacityMilli, 400);

    controller.timeline()->setClipFades(clipId, 200'000, 300'000);
    clip = findClip(controller.document()->project(), clipId);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->audio.fadeInUs, 200'000);
    EXPECT_EQ(clip->audio.fadeOutUs, 300'000);
    EXPECT_EQ(clip->audio.volumeMilli, 1500);
    EXPECT_EQ(clip->audio.panMilli, -200);

    controller.timeline()->selectClip(clipId);
    const QVariantMap selected = controller.timeline()->selectedClip();
    EXPECT_EQ(selected.value(QStringLiteral("scaleMilli")).toInt(), 1500);
    EXPECT_EQ(selected.value(QStringLiteral("offsetXPermille")).toInt(), 250);
    EXPECT_EQ(selected.value(QStringLiteral("offsetYPermille")).toInt(), -300);
    EXPECT_EQ(selected.value(QStringLiteral("opacityMilli")).toInt(), 400);
    EXPECT_EQ(selected.value(QStringLiteral("volumeMilli")).toInt(), 1500);
}

TEST(TimelineUi, EditsRefreshPreviewAndAudioWithoutRestart)
{
    AppController controller(nullptr, QString(), nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;

    controller.addAssetToTimeline(assetId);
    ASSERT_TRUE(controller.preview()->timelineMode());
    controller.seekTimeline(1'000'000);
    ASSERT_EQ(controller.preview()->positionMs(), 1000);
    const QString clipId = controller.document()->project().tracks.at(0)
                               .clips.first().id;

    // A second clip extends the project; the preview duration and the worker
    // snapshot must follow without reopening the timeline.
    ASSERT_FALSE(controller.timeline()
                     ->addAssetToTimeline(assetId, QStringLiteral("track-v1"),
                                          5'000'000)
                     .isEmpty());
    QTest::qWait(250);

    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 2);
    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_FALSE(controller.preview()->playing());
    EXPECT_EQ(controller.preview()->positionMs(), 1000)
        << "an edit must not reset the playhead";
    EXPECT_EQ(controller.preview()->timelineProject().tracks.at(0).clips.size(),
              2);
    EXPECT_EQ(controller.audioEngine()->projectSnapshot()
                  .tracks.at(0).clips.size(), 2);
    EXPECT_GT(controller.preview()->durationUs(), 5'000'000);

    // Moving an existing clip must also reach both snapshots.
    controller.timeline()->moveClip(clipId, 2'000'000);
    QTest::qWait(250);
    const Clip *previewClip =
        findClip(controller.preview()->timelineProject(), clipId);
    ASSERT_NE(previewClip, nullptr);
    EXPECT_EQ(previewClip->timelineStartUs, 2'000'000);
    const Clip *audioClip = findClip(
        controller.audioEngine()->projectSnapshot(), clipId);
    ASSERT_NE(audioClip, nullptr);
    EXPECT_EQ(audioClip->timelineStartUs, 2'000'000);
    EXPECT_EQ(controller.preview()->positionMs(), 1000);
    EXPECT_FALSE(controller.preview()->playing());
}

TEST(TimelineUi, VolumeKeyframesAddSortedAndUndoOneStepEach)
{
    ProjectDocument document;
    document.addAsset(audioAsset(10'000'000));
    TimelineController controller(document);
    const QString clipId = addAudioClip(controller);
    ASSERT_FALSE(clipId.isEmpty());
    controller.selectClip(clipId);

    EXPECT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             1'000'000, 1500));
    EXPECT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             3'000'000, 800));

    TimelineModel *model = controller.timelineModel();
    const int row = rowForItem(model, clipId);
    ASSERT_GE(row, 0);
    EXPECT_EQ(model->data(model->index(row, 0),
                          TimelineModel::KeyframeCountRole)
                  .toInt(),
              2);
    QVariantList list = keyframeList(model, row);
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(list.at(0).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              1'000'000);
    EXPECT_EQ(list.at(0).toMap().value(QStringLiteral("volumeMilli")).toInt(),
              1500);
    EXPECT_EQ(list.at(1).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              3'000'000);
    EXPECT_EQ(list.at(1).toMap().value(QStringLiteral("volumeMilli")).toInt(),
              800);

    const QVariantMap selected = controller.selectedClip();
    EXPECT_EQ(selected.value(QStringLiteral("keyframeCount")).toInt(), 2);
    EXPECT_EQ(selected.value(QStringLiteral("keyframes")).toList().size(), 2);

    // Each add is its own undo step: one undo drops the 3 s keyframe, the
    // second drops the 1 s keyframe.
    document.undo();
    list = keyframeList(model, rowForItem(model, clipId));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.first().toMap().value(QStringLiteral("timeUs")).toLongLong(),
              1'000'000);
    document.undo();
    EXPECT_TRUE(keyframeList(model, rowForItem(model, clipId)).isEmpty());

    document.redo();
    document.redo();
    list = keyframeList(model, rowForItem(model, clipId));
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(list.at(0).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              1'000'000);
    EXPECT_EQ(list.at(1).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              3'000'000);
}

TEST(TimelineUi, VolumeKeyframesReplaceRejectInvalidAndClampVolume)
{
    ProjectDocument document;
    document.addAsset(audioAsset(10'000'000));
    TimelineController controller(document);
    const QString clipId = addAudioClip(controller);
    ASSERT_FALSE(clipId.isEmpty());

    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             2'000'000, 1200));
    int undoIndex = document.undoStack()->index();
    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             2'000'000, 700));
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);
    QVariantList list = keyframeList(controller.timelineModel(),
                                     rowForItem(controller.timelineModel(),
                                                clipId));
    ASSERT_EQ(list.size(), 1) << "same-time add must replace, not duplicate";
    EXPECT_EQ(list.first().toMap().value(QStringLiteral("volumeMilli")).toInt(),
              700);

    document.undo();
    list = keyframeList(controller.timelineModel(),
                        rowForItem(controller.timelineModel(), clipId));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.first().toMap().value(QStringLiteral("volumeMilli")).toInt(),
              1200);

    // Out-of-range times are rejected without touching the undo stack.
    undoIndex = document.undoStack()->index();
    EXPECT_FALSE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                              -1, 1000));
    EXPECT_FALSE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                              5'000'001, 1000));
    EXPECT_EQ(document.undoStack()->index(), undoIndex);
    EXPECT_EQ(keyframeList(controller.timelineModel(),
                           rowForItem(controller.timelineModel(), clipId))
                  .size(),
              1);

    // Volume outside 0..2000 is clamped rather than allowed through.
    EXPECT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             2'000'000, 3000));
    list = keyframeList(controller.timelineModel(),
                        rowForItem(controller.timelineModel(), clipId));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.first().toMap().value(QStringLiteral("volumeMilli")).toInt(),
              2000);
}

TEST(TimelineUi, RemoveVolumeKeyframeKeepsOrderAndUndo)
{
    ProjectDocument document;
    document.addAsset(audioAsset(10'000'000));
    TimelineController controller(document);
    const QString clipId = addAudioClip(controller);
    ASSERT_FALSE(clipId.isEmpty());
    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             1'000'000, 1100));
    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             2'000'000, 1200));
    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             3'000'000, 1300));

    const int undoIndex = document.undoStack()->index();
    EXPECT_TRUE(controller.removeVolumeKeyframe(QStringLiteral("track-a1"),
                                                clipId, 1));
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);

    QVariantList list = keyframeList(controller.timelineModel(),
                                     rowForItem(controller.timelineModel(),
                                                clipId));
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(list.at(0).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              1'000'000);
    EXPECT_EQ(list.at(1).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              3'000'000);

    document.undo();
    list = keyframeList(controller.timelineModel(),
                        rowForItem(controller.timelineModel(), clipId));
    ASSERT_EQ(list.size(), 3);
    EXPECT_EQ(list.at(1).toMap().value(QStringLiteral("timeUs")).toLongLong(),
              2'000'000);
    document.redo();
    EXPECT_EQ(keyframeList(controller.timelineModel(),
                           rowForItem(controller.timelineModel(), clipId))
                  .size(),
              2);

    const int undoBeforeInvalid = document.undoStack()->index();
    EXPECT_FALSE(controller.removeVolumeKeyframe(QStringLiteral("track-a1"),
                                                 clipId, 9));
    EXPECT_EQ(document.undoStack()->index(), undoBeforeInvalid);
}

TEST(TimelineUi, VolumeKeyframesSurviveSerializerRoundTrip)
{
    ProjectDocument document;
    document.addAsset(audioAsset(10'000'000));
    TimelineController controller(document);
    const QString clipId = addAudioClip(controller);
    ASSERT_FALSE(clipId.isEmpty());
    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             500'000, 900));
    ASSERT_TRUE(controller.addVolumeKeyframe(QStringLiteral("track-a1"), clipId,
                                             4'000'000, 1800));

    QString error;
    const auto restored =
        ProjectSerializer::fromJson(ProjectSerializer::toJson(document.project()),
                                    &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    const Clip *clip = findClip(*restored, clipId);
    ASSERT_NE(clip, nullptr);
    ASSERT_EQ(clip->audio.keyframes.size(), 2);
    EXPECT_EQ(clip->audio.keyframes.at(0).timeUs, 500'000);
    EXPECT_EQ(clip->audio.keyframes.at(0).volumeMilli, 900);
    EXPECT_EQ(clip->audio.keyframes.at(1).timeUs, 4'000'000);
    EXPECT_EQ(clip->audio.keyframes.at(1).volumeMilli, 1800);
}

TEST(TimelineUi, CuePositionPreservesFieldsClampsAndUndoes)
{
    ProjectDocument document;
    TimelineController controller(document);
    const QString cueId = controller.addCueAtPlayhead();
    ASSERT_FALSE(cueId.isEmpty());
    controller.setCueText(cueId, QStringLiteral("测试字幕"));
    controller.applySubtitleStyle(QStringLiteral("bold-outline"), cueId);
    controller.selectCue(cueId);

    const SubtitleCue before = *findCue(document.project(), cueId);
    const int undoIndex = document.undoStack()->index();
    EXPECT_TRUE(controller.setCuePosition(QStringLiteral("track-s1"), cueId, 250,
                                          800));
    EXPECT_EQ(document.undoStack()->index(), undoIndex + 1);

    const SubtitleCue *cue = findCue(document.project(), cueId);
    ASSERT_NE(cue, nullptr);
    EXPECT_EQ(cue->positionXPermille, 250);
    EXPECT_EQ(cue->positionYPermille, 800);
    EXPECT_EQ(cue->text, before.text);
    EXPECT_EQ(cue->styleId, before.styleId);
    EXPECT_EQ(cue->id, before.id);
    EXPECT_EQ(cue->startUs, before.startUs);
    EXPECT_EQ(cue->endUs, before.endUs);

    const QVariantMap selected = controller.selectedCue();
    EXPECT_EQ(selected.value(QStringLiteral("positionXPermille")).toInt(), 250);
    EXPECT_EQ(selected.value(QStringLiteral("positionYPermille")).toInt(), 800);
    EXPECT_EQ(selected.value(QStringLiteral("text")).toString(),
              before.text);

    const int cueRow = rowForItem(controller.timelineModel(), cueId);
    ASSERT_GE(cueRow, 0);
    EXPECT_EQ(controller.timelineModel()
                  ->data(controller.timelineModel()->index(cueRow, 0),
                         TimelineModel::PositionXPermilleRole)
                  .toInt(),
              250);
    EXPECT_EQ(controller.timelineModel()
                  ->data(controller.timelineModel()->index(cueRow, 0),
                         TimelineModel::PositionYPermilleRole)
                  .toInt(),
              800);

    document.undo();
    cue = findCue(document.project(), cueId);
    ASSERT_NE(cue, nullptr);
    EXPECT_EQ(cue->positionXPermille, 500);
    EXPECT_EQ(cue->positionYPermille, 880);
    EXPECT_EQ(cue->text, before.text);
    EXPECT_EQ(cue->styleId, before.styleId);

    // Out-of-range positions are clamped to 0..1000.
    EXPECT_TRUE(controller.setCuePosition(QStringLiteral("track-s1"), cueId, -50,
                                          2000));
    cue = findCue(document.project(), cueId);
    ASSERT_NE(cue, nullptr);
    EXPECT_EQ(cue->positionXPermille, 0);
    EXPECT_EQ(cue->positionYPermille, 1000);
}
