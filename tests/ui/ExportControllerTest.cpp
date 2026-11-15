#include <gtest/gtest.h>

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "ExportCheckpoint.h"
#include "ExportController.h"
#include "ExportSettings.h"
#include "ProjectDocument.h"

using namespace VideoEditor;

namespace {

// Rebuilds exactly what the controller derives from the h264-720p30 preset so
// a checkpoint hash can be produced without exposing controller internals.
ExportSettings h264_720p30Settings(const QString &outputPath)
{
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H264;
    settings.width = 1280;
    settings.height = 720;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.quality = ExportQuality::Medium;
    settings.bitrateKbps = 6000;
    settings.preferHardware = true;
    settings.burnSubtitles = true;
    settings.writeSrt = false;
    settings.outputPath = outputPath;
    settings.srtPath = defaultSrtPathFor(outputPath);
    return settings;
}

} // namespace

TEST(ExportController, PresetSelectionUpdatesSettingsAndH265Gate)
{
    ProjectDocument document;
    document.setProject(Project{});
    ExportController controller(document, QString());
    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    controller.refreshCapabilities();

    EXPECT_FALSE(controller.h265Available());
    EXPECT_FALSE(controller.h265UnavailableReason().isEmpty());

    // The WRITE presetId property has the same effect as applyPreset.
    controller.setPresetId(QStringLiteral("h264-720p30"));
    EXPECT_EQ(controller.width(), 1280);
    EXPECT_EQ(controller.presetId(), QStringLiteral("h264-720p30"));

    controller.applyPreset(QStringLiteral("h264-720p30"));
    EXPECT_EQ(controller.width(), 1280);
    EXPECT_EQ(controller.height(), 720);
    EXPECT_EQ(controller.codec(), QStringLiteral("h264"));
    EXPECT_EQ(controller.bitrateKbps(), 6000);

    controller.applyPreset(QStringLiteral("h265-1080p30"));
    EXPECT_EQ(controller.codec(), QStringLiteral("h265"));
    EXPECT_FALSE(controller.validate().isEmpty());
    EXPECT_NE(controller.validate().indexOf(QStringLiteral("H.265")), -1);
}

TEST(ExportController, ValidationRejectsMissingOutputAndOddSize)
{
    ProjectDocument document;
    document.setProject(Project{});
    ExportController controller(document, QString());
    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    controller.refreshCapabilities();
    controller.applyPreset(QStringLiteral("h264-720p30"));
    // The constructor suggests a timestamped Movies path; clear it explicitly
    // so the missing-output rule is what is under test here.
    controller.setOutputPath(QString());
    EXPECT_FALSE(controller.validate().isEmpty());
    controller.setOutputPath(QStringLiteral("D:/tmp/out"));
    EXPECT_TRUE(controller.outputPath().endsWith(QStringLiteral(".mp4")));
    controller.setWidth(1279);
    EXPECT_FALSE(controller.validate().isEmpty());

    // file:// URLs, including UNC shares, become local paths.
    controller.setOutputPath(QStringLiteral("file:///D:/tmp/out2"));
    EXPECT_EQ(controller.outputPath(), QStringLiteral("D:/tmp/out2.mp4"));
    controller.setOutputPath(QStringLiteral("file://server/share/out"));
    EXPECT_TRUE(controller.outputPath().endsWith(QStringLiteral(".mp4")));
    EXPECT_TRUE(controller.outputPath().contains(QStringLiteral("server")));
}

TEST(ExportController, RejectsDestinationThatWouldOverwriteSource)
{
    ProjectDocument document;
    Project project;
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    Asset video;
    video.id = QStringLiteral("v1");
    video.path = QStringLiteral("D:/media/Source Video.MP4");
    project.assets.append(video);
    project.ensureDefaultTracks();
    document.setProject(project, QStringLiteral("D:/projects/成片.mp4"));

    ExportController controller(document, QString());
    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    controller.refreshCapabilities();
    controller.applyPreset(QStringLiteral("h264-720p30"));

    // Case-insensitive comparison: Windows would treat these as one file.
    controller.setOutputPath(QStringLiteral("d:/MEDIA/source video.mp4"));
    EXPECT_NE(controller.validate().indexOf(QStringLiteral("素材")), -1)
        << controller.validate().toStdString();

    controller.setOutputPath(QStringLiteral("D:/projects/成片.mp4"));
    EXPECT_NE(controller.validate().indexOf(QStringLiteral("工程")), -1)
        << controller.validate().toStdString();

    // The SRT sidecar is guarded too, but only when it will be written.
    controller.setOutputPath(QStringLiteral("D:/exports/movie.mp4"));
    controller.setSrtPath(QStringLiteral("D:/media/source video.mp4"));
    EXPECT_TRUE(controller.validate().isEmpty())
        << "an unwritten SRT path must not block the export";
    controller.setWriteSrt(true);
    EXPECT_NE(controller.validate().indexOf(QStringLiteral("SRT")), -1)
        << controller.validate().toStdString();

    controller.setWriteSrt(false);
    EXPECT_TRUE(controller.validate().isEmpty());
}

TEST(ExportController, ValidationStaysSilentUntilTheProbeCompletes)
{
    ProjectDocument document;
    document.setProject(Project{});

    ExportController controller(document, QString());
    controller.applyPreset(QStringLiteral("h264-720p30"));
    controller.setOutputPath(QStringLiteral("D:/tmp/pre-probe.mp4"));

    // The first dialog open kicks off an async probe; before it finishes the
    // catalog is empty and must not be reported as "no H.264 encoder".
    EXPECT_FALSE(controller.capabilitiesReady());
    EXPECT_TRUE(controller.validate().isEmpty())
        << controller.validate().toStdString();

    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    EXPECT_TRUE(controller.capabilitiesReady());
    EXPECT_TRUE(controller.validate().isEmpty());
}

TEST(ExportController, SyncRunnerDrivesProgressAndFinish)
{
    ProjectDocument document;
    Project project;
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    project.ensureDefaultTracks();
    document.setProject(project);

    int calls = 0;
    ExportController controller(document, QString());
    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    controller.refreshCapabilities();
    controller.applyPreset(QStringLiteral("h264-720p30"));
    controller.setOutputPath(QStringLiteral("D:/tmp/controller-test.mp4"));
    controller.setSyncRunnerForTest(
        [&calls](const Project &, const ExportSettings &, bool resume,
                 const std::function<void(const ExportProgress &)> &progress) {
            ++calls;
            ExportProgress p;
            p.stage = ExportStage::Rendering;
            p.completedSegments = 1;
            p.totalSegments = 2;
            p.percent = 50;
            if (progress)
                progress(p);
            ExportResult result;
            result.ok = true;
            result.resumed = resume;
            result.outputPath = QStringLiteral("D:/tmp/controller-test.mp4");
            result.encoderId = QStringLiteral("h264_mf@sw");
            result.validation.ok = true;
            return result;
        });

    QSignalSpy finished(&controller, &ExportController::finished);
    controller.startExport();
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_TRUE(finished.at(0).at(0).toBool());
    EXPECT_DOUBLE_EQ(controller.progress(), 1.0);
    EXPECT_FALSE(controller.running());
    // The result reports EncoderDescriptor::key(); the UI must show the
    // friendly display name, not "h264_mf@sw".
    EXPECT_EQ(controller.encoderName(),
              QStringLiteral("Media Foundation H.264（软件）"));

    // A finished run's progress bar is cleared the next time the dialog opens.
    controller.refreshCapabilities();
    EXPECT_DOUBLE_EQ(controller.progress(), 0.0);
}

TEST(ExportController, CanResumeOnlyForMatchingCheckpoint)
{
    ProjectDocument document;
    Project project;
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    project.ensureDefaultTracks();
    document.setProject(project);

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString outputPath =
        QDir(temp.path()).filePath(QStringLiteral("成片.mp4"));

    ExportController controller(document, QString());
    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    controller.refreshCapabilities();
    controller.applyPreset(QStringLiteral("h264-720p30"));
    controller.setOutputPath(outputPath);
    EXPECT_FALSE(controller.canResume());

    ExportCheckpoint checkpoint;
    checkpoint.projectHash =
        exportProjectHash(document.project(), h264_720p30Settings(outputPath));
    checkpoint.width = 1280;
    checkpoint.height = 720;
    checkpoint.fps = Rational{30, 1};
    checkpoint.sampleRate = 48000;
    checkpoint.totalFrames = 30;
    checkpoint.totalSamples = 48000;
    checkpoint.segmentSamples = 48000;
    checkpoint.segmentCount = 1;
    checkpoint.completedSegments = 1;
    const QString partsDir = partsDirectoryFor(outputPath);
    ASSERT_TRUE(QDir().mkpath(partsDir));
    ASSERT_TRUE(checkpoint.save(
        QDir(partsDir).filePath(QStringLiteral("checkpoint.json"))));

    controller.refreshCapabilities();
    EXPECT_TRUE(controller.canResume());

    // A plain start begins a new export; only the explicit resume action
    // passes resume=true to the pipeline.
    bool plainStartResumed = true;
    bool resumeActionResumed = false;
    int runs = 0;
    controller.setSyncRunnerForTest(
        [&](const Project &, const ExportSettings &, bool resume,
            const std::function<void(const ExportProgress &)> &) {
            ++runs;
            if (runs == 1)
                plainStartResumed = resume;
            else
                resumeActionResumed = resume;
            ExportResult result;
            result.ok = true;
            result.outputPath = outputPath;
            result.validation.ok = true;
            return result;
        });
    QSignalSpy finished(&controller, &ExportController::finished);
    controller.startExport();
    ASSERT_EQ(runs, 1);
    EXPECT_FALSE(plainStartResumed);
    EXPECT_TRUE(controller.canResume());
    controller.resumeExport();
    ASSERT_EQ(runs, 2);
    EXPECT_TRUE(resumeActionResumed);
    EXPECT_EQ(finished.count(), 2);

    // A different preset changes the hash, so the old parts must be discarded.
    controller.applyPreset(QStringLiteral("mpeg4-1080p30"));
    EXPECT_FALSE(controller.canResume());
}

TEST(ExportController, WorkerThreadFailsFastAndTearsDown)
{
    ProjectDocument document;
    Project project; // empty timeline: the pipeline fails before encoding
    document.setProject(project);

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString outputPath =
        QDir(temp.path()).filePath(QStringLiteral("async.mp4"));

    ExportController controller(document, QString());
    controller.setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    controller.refreshCapabilities();
    controller.applyPreset(QStringLiteral("h264-720p30"));
    controller.setOutputPath(outputPath);
    ASSERT_TRUE(controller.validate().isEmpty());

    QSignalSpy finished(&controller, &ExportController::finished);
    controller.startExport();
    ASSERT_TRUE(controller.running());
    EXPECT_TRUE(controller.canCancel());

    // The result must arrive through a queued cross-thread connection, and the
    // controller must tear the worker/thread down without a UI-thread block.
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 15000);
    EXPECT_FALSE(controller.running());
    EXPECT_FALSE(controller.canCancel());
    EXPECT_FALSE(finished.at(0).at(0).toBool());
    EXPECT_TRUE(controller.error().contains(QStringLiteral("时间线为空")))
        << controller.error().toStdString();
    EXPECT_FALSE(controller.canResume());

    // A second run gets a fresh worker; a canceled/used worker is never reused.
    finished.clear();
    controller.startExport();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 15000);
    EXPECT_FALSE(controller.running());
}
