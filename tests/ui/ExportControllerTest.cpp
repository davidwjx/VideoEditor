#include <gtest/gtest.h>

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

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

    // A different preset changes the hash, so the old parts must be discarded.
    controller.applyPreset(QStringLiteral("mpeg4-1080p30"));
    EXPECT_FALSE(controller.canResume());
}
