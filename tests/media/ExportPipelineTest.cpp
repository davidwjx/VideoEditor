#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "D3D11Context.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "ExportWorker.h"
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
    const QString checkpointPath =
        partsDirectoryFor(output) + QStringLiteral("/checkpoint.json");
    ASSERT_TRUE(QFileInfo::exists(checkpointPath));
    // A failed run must not claim the whole timeline is complete just because
    // closing the open parts succeeded.
    const auto checkpoint = ExportCheckpoint::load(checkpointPath);
    ASSERT_TRUE(checkpoint.has_value());
    EXPECT_GT(checkpoint->completedSegments, 0);
    EXPECT_LT(checkpoint->completedSegments, checkpoint->segmentCount);

    const ExportResult resumed =
        pipeline.run(project, settings, catalog, true, {});
    ASSERT_TRUE(resumed.ok) << resumed.error.toStdString();
    EXPECT_TRUE(resumed.resumed);
    EXPECT_TRUE(resumed.validation.ok);
    EXPECT_TRUE(QFileInfo::exists(output));
}

// A Cancel clicked between queueing the worker and dequeuing run() must not be
// erased by run(); otherwise the click is lost and the export completes.
TEST_F(ExportPipelineTest, WorkerCancelBeforeRunProducesNoOutput)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("precancel.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ExportWorker worker;
    worker.setCatalog(EncoderCatalog::withoutHardwareForTest());
    ExportResult result;
    bool finished = false;
    QObject::connect(&worker, &ExportWorker::finished, &worker,
                     [&result, &finished](const ExportResult &value) {
                         result = value;
                         finished = true;
                     });
    worker.cancel(); // clicked before run() is dequeued
    EXPECT_TRUE(worker.isCanceled());
    worker.run(project, settingsFor(ExportVideoCodec::Mpeg4Part2, output),
               false);
    ASSERT_TRUE(finished);
    EXPECT_TRUE(result.canceled);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.resumable);
    EXPECT_FALSE(QFileInfo::exists(output));
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(output)
                                   + QStringLiteral("/audio.mp4")));
}

// Reopening a hardware encoder can publish different SPS/PPS. The pipeline
// probes the new parameter sets against part 0 at resume: identical sets
// resume, different sets restart from scratch. Either way the retry must
// finish instead of wedging on an unassemblable parts directory.
TEST_F(ExportPipelineTest, HardwareH264ResumeFinishesAfterFailure)
{
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    const EncoderDescriptor *hardware = nullptr;
    for (const EncoderDescriptor &candidate : catalog.encoders) {
        if (candidate.codec == ExportVideoCodec::H264 && candidate.hardware
            && candidate.available) {
            hardware = &candidate;
            break;
        }
    }
    if (!hardware)
        GTEST_SKIP() << "no hardware H.264 encoder available";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("hw-resume.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ExportSettings settings = settingsFor(ExportVideoCodec::H264, output);
    settings.encoderId = hardware->key();
    settings.segmentTargetUs = 100'000;
    ExportPipeline::Hooks hooks;
    hooks.failAtSegment = 10;
    ExportPipeline pipeline;
    const ExportResult failed =
        pipeline.run(project, settings, catalog, false, hooks);
    EXPECT_FALSE(failed.ok) << failed.error.toStdString();
    EXPECT_TRUE(failed.resumable);

    const ExportResult resumed =
        pipeline.run(project, settings, catalog, true, {});
    ASSERT_TRUE(resumed.ok) << resumed.error.toStdString()
                            << " / warning=" << resumed.warning.toStdString()
                            << " / resumed=" << resumed.resumed;
    EXPECT_TRUE(resumed.validation.ok) << resumed.validation.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(output));
}

// A crash can land between closing a part and persisting the checkpoint. The
// complete parts past the claim are valid work; resume must adopt them and
// start at the first segment that is actually missing instead of re-rendering
// (and deleting) all of them.
TEST_F(ExportPipelineTest, ResumeAdoptsCompletePartsBeyondCheckpoint)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("adopt.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ExportSettings settings =
        settingsFor(ExportVideoCodec::Mpeg4Part2, output);
    settings.segmentTargetUs = 100'000; // 3 frames per segment
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});

    ExportPipeline::Hooks failHooks;
    failHooks.failAtSegment = 12;
    ExportPipeline pipeline;
    const ExportResult failed =
        pipeline.run(project, settings, catalog, false, failHooks);
    ASSERT_FALSE(failed.ok);

    const QString checkpointPath =
        QDir(partsDirectoryFor(output))
            .filePath(QStringLiteral("checkpoint.json"));
    const auto saved = ExportCheckpoint::load(checkpointPath);
    ASSERT_TRUE(saved.has_value());
    const int completeOnDisk = saved->completedSegments;
    ASSERT_GT(completeOnDisk, 3);
    ASSERT_LT(completeOnDisk, saved->segmentCount);

    // Rewind the claim: the parts for completeOnDisk - 3 .. completeOnDisk - 1
    // are still on disk but the checkpoint no longer mentions them.
    ExportCheckpoint stale = *saved;
    stale.completedSegments = completeOnDisk - 3;
    ASSERT_TRUE(stale.save(checkpointPath));

    int firstReported = -1;
    ExportPipeline::Hooks resumeHooks;
    resumeHooks.onProgress = [&firstReported](const ExportProgress &progress) {
        if (firstReported < 0)
            firstReported = progress.completedSegments;
    };
    const ExportResult resumed =
        pipeline.run(project, settings, catalog, true, resumeHooks);
    ASSERT_TRUE(resumed.ok) << resumed.error.toStdString();
    EXPECT_TRUE(resumed.resumed);
    EXPECT_TRUE(resumed.validation.ok);
    EXPECT_EQ(firstReported, completeOnDisk)
        << "resume must start at the first missing segment, not at the claim";
    EXPECT_TRUE(QFileInfo::exists(output));
}

// Completed segments must be durable while the video pass is still running: a
// hard crash between the last closeThrough and the end of the loop used to
// leave a checkpoint claiming zero segments.
TEST_F(ExportPipelineTest, CheckpointAdvancesWhileRendering)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("progress.mp4"));
    const Project project = makeProject(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR));
    ExportSettings settings =
        settingsFor(ExportVideoCodec::Mpeg4Part2, output);
    settings.segmentTargetUs = 100'000; // 3 frames per segment
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    const QString checkpointPath =
        QDir(partsDirectoryFor(output))
            .filePath(QStringLiteral("checkpoint.json"));

    int observedDuringRun = -1;
    ExportPipeline::Hooks hooks;
    hooks.failAtSegment = 15;
    hooks.onProgress = [&](const ExportProgress &progress) {
        if (progress.stage != ExportStage::Rendering
            || progress.completedFrames < 30) {
            return;
        }
        const auto midRun = ExportCheckpoint::load(checkpointPath);
        if (midRun)
            observedDuringRun = std::max(observedDuringRun,
                                         midRun->completedSegments);
    };
    ExportPipeline pipeline;
    const ExportResult result =
        pipeline.run(project, settings, catalog, false, hooks);
    EXPECT_FALSE(result.ok); // the injected failure ends the run
    EXPECT_GT(observedDuringRun, 0)
        << "the checkpoint must advance before the video pass finishes";
}

// Finalization replaces the destination. An output or SRT path that is one of
// the imported assets (or the project file) must be rejected before any part
// is written, otherwise the source media would be deleted.
TEST(ExportPipeline, RejectsDestinationsThatWouldOverwriteSource)
{
    const QString fixtureDir = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("source.mp4"));
    {
        QFile file(output);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("source-media");
    }
    const auto contents = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    };

    Project project = makeProject(fixtureDir);
    project.assets[1].path = output; // pretend the target is imported media
    const EncoderCatalog catalog = EncoderCatalog::withoutHardwareForTest();
    ExportPipeline pipeline;

    ExportSettings settings = settingsFor(ExportVideoCodec::Mpeg4Part2, output);
    ExportResult result = pipeline.run(project, settings, catalog, false, {});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.indexOf(QStringLiteral("素材")), -1)
        << result.error.toStdString();
    EXPECT_EQ(contents(output), QByteArray("source-media"));
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(output)));

    // The SRT sidecar may not replace an asset either.
    const QString srtTarget = dir.filePath(QStringLiteral("captions.srt"));
    {
        QFile file(srtTarget);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("previous-captions");
    }
    project = makeProject(fixtureDir);
    project.assets[1].path = srtTarget;
    settings = settingsFor(ExportVideoCodec::Mpeg4Part2,
                           dir.filePath(QStringLiteral("movie.mp4")));
    settings.writeSrt = true;
    settings.srtPath = srtTarget;
    result = pipeline.run(project, settings, catalog, false, {});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.indexOf(QStringLiteral("SRT")), -1)
        << result.error.toStdString();
    EXPECT_EQ(contents(srtTarget), QByteArray("previous-captions"));
    EXPECT_FALSE(QFileInfo::exists(
        partsDirectoryFor(settings.outputPath)));

    // The project file itself is equally protected.
    const QString projectPath = dir.filePath(QStringLiteral("project.mp4"));
    project = makeProject(fixtureDir);
    project.assets[1].path = QDir(fixtureDir).filePath(
        QStringLiteral("tone.wav"));
    settings = settingsFor(ExportVideoCodec::Mpeg4Part2, projectPath);
    settings.writeSrt = false;
    result = pipeline.run(project, settings, catalog, false, {}, projectPath);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.indexOf(QStringLiteral("工程")), -1)
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(projectPath));
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(projectPath)));
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
