#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include "D3D11Context.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "MediaProbe.h"
#include "Project.h"

using namespace VideoEditor;

namespace {

Project makeProject(const QString &fixtureDir, bool withAudio = true)
{
    Project project;
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    Asset video;
    video.id = QStringLiteral("v1");
    video.path = QDir(fixtureDir).filePath(QStringLiteral("h264.mp4"));
    video.info = *MediaProbe::probe(video.path);
    project.assets.append(video);
    if (withAudio) {
        Asset audio;
        audio.id = QStringLiteral("a1");
        audio.path = QDir(fixtureDir).filePath(QStringLiteral("tone.wav"));
        audio.info = *MediaProbe::probe(audio.path);
        project.assets.append(audio);
    }
    project.ensureDefaultTracks();
    for (Track &track : project.tracks) {
        if (track.type == TrackType::Video) {
            Clip clip;
            clip.id = QStringLiteral("vc1");
            clip.assetId = video.id;
            clip.sourceInUs = 0;
            clip.sourceOutUs = 2'000'000;
            clip.timelineStartUs = 0;
            track.clips.append(clip);
        } else if (track.type == TrackType::Audio && withAudio) {
            Clip clip;
            clip.id = QStringLiteral("ac1");
            clip.assetId = QStringLiteral("a1");
            clip.sourceInUs = 0;
            clip.sourceOutUs = 2'000'000;
            clip.timelineStartUs = 0;
            track.clips.append(clip);
        }
    }
    return project;
}

ExportSettings settingsFor(ExportVideoCodec codec, const QString &output)
{
    ExportSettings settings;
    settings.codec = codec;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 4000;
    settings.outputPath = output;
    settings.segmentTargetUs = 500'000;
    return settings;
}

} // namespace

// Every ExportPipeline test renders real frames through D3D11.
class ExportPipelineTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        QString error;
        if (!D3D11Context::create(true, &error)
            && !D3D11Context::create(false, &error)) {
            GTEST_SKIP() << error.toStdString();
        }
    }
};

TEST_F(ExportPipelineTest, Mpeg4AndH264ProduceValidMp4)
{
    const QString fixtureDir = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR);
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    for (ExportVideoCodec codec :
         {ExportVideoCodec::Mpeg4Part2, ExportVideoCodec::H264}) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString output = dir.filePath(QStringLiteral("out.mp4"));
        const Project project = makeProject(fixtureDir);
        ExportPipeline pipeline;
        const ExportResult result =
            pipeline.run(project, settingsFor(codec, output), catalog, false, {});
        ASSERT_TRUE(result.ok) << result.error.toStdString();
        EXPECT_TRUE(result.validation.ok) << result.validation.error.toStdString();
        EXPECT_FALSE(result.validation.decodedFirstFrame == false);
        const int64_t frameUs = 33'333;
        EXPECT_LE(std::llabs(result.validation.videoDurationUs - 2'000'000),
                  frameUs + 1);
        EXPECT_LE(std::llabs(result.validation.audioDurationUs
                             - result.validation.videoDurationUs),
                  frameUs + 1);
        EXPECT_EQ(result.validation.videoPacketCount, 60);
    }
}

TEST_F(ExportPipelineTest, SilentProjectStillGetsAacTrack)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("silent.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR),
                                        false);
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    ExportPipeline pipeline;
    const ExportResult result =
        pipeline.run(project, settingsFor(ExportVideoCodec::H264, output),
                     catalog, false, {});
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.validation.info.hasAudio);
    EXPECT_EQ(result.validation.info.audioCodec, QStringLiteral("aac"));
    // Raw AAC packets include priming/padding; the container duration check
    // above is the authoritative ≤1-frame assertion.
    EXPECT_NEAR(double(result.validation.audioSampleCount), 60.0 * 1600.0, 1024.0);
}

TEST_F(ExportPipelineTest, MissingAssetBecomesBlackGapWithWarning)
{
    const QString fixtureDir = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR);
    Project project = makeProject(fixtureDir);
    for (Track &track : project.tracks) {
        for (Clip &clip : track.clips) {
            if (clip.assetId == QStringLiteral("v1"))
                clip.assetId = QStringLiteral("missing");
        }
    }
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    ExportPipeline pipeline;
    const ExportResult result = pipeline.run(
        project, settingsFor(ExportVideoCodec::Mpeg4Part2,
                             dir.filePath(QStringLiteral("gap.mp4"))),
        catalog, false, {});
    EXPECT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.warning.isEmpty());
}

TEST_F(ExportPipelineTest, CancelKeepsCheckpointAndResumeCompletes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("resume.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ExportSettings settings =
        settingsFor(ExportVideoCodec::Mpeg4Part2, output);
    // Many short segments so the router closes at least one full segment
    // before the cancellation checks fire.
    settings.segmentTargetUs = 100'000;
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    // 2 s at 480 samples/block = 200 audio-pass cancellation checks; cancel
    // after those so at least one full video segment has been closed.
    int cancelChecks = 0;
    ExportPipeline::Hooks hooks;
    hooks.isCanceled = [&cancelChecks] { return ++cancelChecks > 220; };
    ExportPipeline pipeline;
    const ExportResult canceled = pipeline.run(project, settings, catalog, false,
                                               hooks);
    EXPECT_TRUE(canceled.canceled);
    EXPECT_TRUE(canceled.resumable);
    EXPECT_TRUE(QFileInfo::exists(partsDirectoryFor(output)
                                  + QStringLiteral("/checkpoint.json")));

    const ExportResult resumed =
        pipeline.run(project, settings, catalog, true, {});
    ASSERT_TRUE(resumed.ok) << resumed.error.toStdString();
    EXPECT_TRUE(resumed.validation.ok);
    EXPECT_TRUE(resumed.resumed);
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(output)));
}

// A failure mid-export (the injected segment failure stands in for a hardware
// encoder dying after a few seconds) must keep the completed parts and the
// checkpoint so a retry resumes instead of losing the work.
TEST_F(ExportPipelineTest, SegmentFailureKeepsResumableCheckpoint)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("failed.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ExportSettings settings =
        settingsFor(ExportVideoCodec::Mpeg4Part2, output);
    settings.segmentTargetUs = 100'000;
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    ExportPipeline::Hooks hooks;
    hooks.failAtSegment = 10;
    ExportPipeline pipeline;
    const ExportResult failed =
        pipeline.run(project, settings, catalog, false, hooks);
    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.canceled);
    EXPECT_TRUE(failed.resumable);
    EXPECT_FALSE(failed.error.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(partsDirectoryFor(output)
                                  + QStringLiteral("/checkpoint.json")));

    const ExportResult resumed =
        pipeline.run(project, settings, catalog, true, {});
    ASSERT_TRUE(resumed.ok) << resumed.error.toStdString();
    EXPECT_TRUE(resumed.resumed);
    EXPECT_TRUE(resumed.validation.ok);
    EXPECT_TRUE(QFileInfo::exists(output));
}

TEST(ExportPipeline, H265WithoutHardwareFailsBeforeWritingAnything)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("h265.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    const EncoderCatalog catalog = EncoderCatalog::withoutHardwareForTest();
    ExportPipeline pipeline;
    const ExportResult result =
        pipeline.run(project, settingsFor(ExportVideoCodec::H265, output),
                     catalog, false, {});
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_NE(result.error.indexOf(QStringLiteral("H.265")), -1);
    EXPECT_FALSE(QFileInfo::exists(output));
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(output)));
}

TEST(ExportPipeline, InsufficientDiskSpaceFailsWithoutOutput)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("full.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    ExportPipeline::Hooks hooks;
    hooks.freeBytes = [](const QString &) { return qint64(1024); };
    ExportPipeline pipeline;
    const ExportResult result =
        pipeline.run(project, settingsFor(ExportVideoCodec::Mpeg4Part2, output),
                     catalog, false, hooks);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.indexOf(QStringLiteral("磁盘空间")), -1);
    EXPECT_FALSE(QFileInfo::exists(output));
}

TEST_F(ExportPipelineTest, UnicodeOutputPathIsSupported)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("中文 成片.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    ExportPipeline pipeline;
    const ExportResult result =
        pipeline.run(project, settingsFor(ExportVideoCodec::Mpeg4Part2, output),
                     catalog, false, {});
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(output));
}
