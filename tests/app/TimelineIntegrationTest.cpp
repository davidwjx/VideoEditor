#include <gtest/gtest.h>

#include <memory>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "AppController.h"
#include "AudioEngine.h"
#include "AudioSink.h"
#include "ProjectDocument.h"
#include "ProjectSerializer.h"
#include "TimelineController.h"
#include "TimelineEvaluator.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

// Keep integration tests off the real WASAPI device.
AudioEngine::SinkFactory nullSinkFactory()
{
    return [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
}

} // namespace

TEST(TimelineIntegration, EditsSurviveSaveAndReload)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    const QString video = fixture(QStringLiteral("h264.mp4"));
    const QString audio = fixture(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(video) || !QFileInfo::exists(audio))
        GTEST_SKIP() << "missing fixtures";

    controller.importUrls({QUrl::fromLocalFile(video), QUrl::fromLocalFile(audio)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 2);
    const QString videoId = controller.document()->project().assets.at(0).id;
    const QString audioId = controller.document()->project().assets.at(1).id;

    controller.timeline()->addAssetToTimeline(videoId, QStringLiteral("track-v1"), 0);
    controller.timeline()->addAssetToTimeline(audioId, QStringLiteral("track-a1"), 0);
    QString audioClipId;
    for (const Clip &clip : controller.document()->project().tracks.at(1).clips) {
        if (clip.assetId == audioId)
            audioClipId = clip.id;
    }
    ASSERT_FALSE(audioClipId.isEmpty());
    controller.timeline()->setClipVolume(audioClipId, 2000);
    controller.timeline()->setClipPan(audioClipId, -50);
    controller.timeline()->setClipFades(audioClipId, 2'000'000, 2'000'000);
    controller.timeline()->seekUs(1'000'000);
    controller.timeline()->setClipSpeed(
        controller.document()->project().tracks.at(0).clips.first().id, 1500, true);
    // UAT item 5 also covers authored keyframes and cue positions.
    ASSERT_TRUE(controller.timeline()->addVolumeKeyframe(
        QStringLiteral("track-a1"), audioClipId, 500'000, 900));
    controller.timeline()->addCueAtPlayhead();
    const QString cueId = controller.document()->project().tracks.at(2).cues.first().id;
    controller.timeline()->setCueText(cueId, QStringLiteral("保存测试"));
    EXPECT_TRUE(controller.timeline()->setCuePosition(
        QStringLiteral("track-s1"), cueId, 250, 700));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("project.veproj"));
    QString error;
    ASSERT_TRUE(controller.document()->save(path, &error)) << error.toStdString();

    ProjectDocument reopened;
    ASSERT_TRUE(reopened.load(path, &error)) << error.toStdString();
    EXPECT_EQ(reopened.project().tracks.size(), 3);
    EXPECT_EQ(reopened.project().tracks.at(0).clips.first().speedMilli, 1500);
    EXPECT_EQ(reopened.project().tracks.at(2).cues.first().text,
              QStringLiteral("保存测试"));
    EXPECT_EQ(reopened.project().assets.size(), 2);
    EXPECT_EQ(reopened.project().tracks.at(0).id, QStringLiteral("track-v1"));
    EXPECT_EQ(reopened.project().tracks.at(1).id, QStringLiteral("track-a1"));
    EXPECT_EQ(reopened.project().tracks.at(2).id, QStringLiteral("track-s1"));

    const Clip *reloadedBgm = nullptr;
    for (const Clip &clip : reopened.project().tracks.at(1).clips) {
        if (clip.assetId == audioId)
            reloadedBgm = &clip;
    }
    ASSERT_NE(reloadedBgm, nullptr);
    EXPECT_EQ(reloadedBgm->audio.volumeMilli, 2000);
    EXPECT_EQ(reloadedBgm->audio.panMilli, -50);
    EXPECT_EQ(reloadedBgm->audio.fadeInUs, 2'000'000);
    EXPECT_EQ(reloadedBgm->audio.fadeOutUs, 2'000'000);
    ASSERT_EQ(reloadedBgm->audio.keyframes.size(), 1);
    EXPECT_EQ(reloadedBgm->audio.keyframes.first().timeUs, 500'000);
    EXPECT_EQ(reloadedBgm->audio.keyframes.first().volumeMilli, 900);
    const SubtitleCue &reloadedCue =
        reopened.project().tracks.at(2).cues.first();
    EXPECT_EQ(reloadedCue.positionXPermille, 250);
    EXPECT_EQ(reloadedCue.positionYPermille, 700);

    const int64_t duration = TimelineEvaluator(reopened.project()).durationUs();
    EXPECT_GT(duration, 0);
}

TEST(TimelineIntegration, UndoHistoryDepthIsAtLeastOneHundred)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    const QString video = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(video))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(video)});
    const QString assetId = controller.document()->project().assets.first().id;
    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    for (int i = 1; i <= 105; ++i)
        controller.timeline()->moveClip(clipId, i * 10'000);
    EXPECT_EQ(controller.document()->undoStack()->undoLimit(), 100);
    controller.document()->undoStack()->undo();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.first().timelineStartUs,
              1'040'000);
}
