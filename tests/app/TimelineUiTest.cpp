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
    EXPECT_TRUE(controller.preview()->timelineMode());
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
