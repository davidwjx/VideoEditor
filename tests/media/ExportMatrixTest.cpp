#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

#include "D3D11Context.h"
#include "ExportCheckpoint.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "ExportTestProject.h"

using namespace VideoEditor;

namespace {

bool gpuAvailable()
{
    QString error;
    return D3D11Context::create(true, &error) != nullptr
           || D3D11Context::create(false, &error) != nullptr;
}

} // namespace

// MPEG-4 and H.264 must actually produce playable MP4 files: the pipeline's
// own re-probe validation checks container, codecs, frame rate, packet count
// and both track durations (audio within one frame of video).
TEST(ExportMatrix, SoftwareCodecsProduceValidMp4)
{
    if (!gpuAvailable())
        GTEST_SKIP() << "no D3D11 device";
    const EncoderCatalog catalog = detectEncodersForTest(320, 180, Rational{30, 1});
    for (ExportVideoCodec codec : {ExportVideoCodec::Mpeg4Part2,
                                   ExportVideoCodec::H264}) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        ExportSettings settings;
        settings.codec = codec;
        settings.width = 320;
        settings.height = 180;
        settings.fps = Rational{30, 1};
        settings.rateControl = ExportRateControl::Bitrate;
        settings.bitrateKbps = 6000;
        settings.outputPath = dir.filePath(QStringLiteral("out.mp4"));
        settings.segmentTargetUs = 500'000;

        ExportPipeline pipeline;
        const ExportResult result = pipeline.run(
            makeExportProject(VIDEOEDITOR_FIXTURE_DIR), settings, catalog,
            false, {});
        ASSERT_TRUE(result.ok) << result.error.toStdString();
        EXPECT_TRUE(result.validation.ok) << result.validation.error.toStdString();
        EXPECT_EQ(result.validation.videoPacketCount, 60);
        const int64_t frameUs = 33'333;
        EXPECT_LE(std::llabs(result.validation.videoDurationUs - 2'000'000),
                  frameUs + 1);
        EXPECT_LE(std::llabs(result.validation.audioDurationUs
                             - result.validation.videoDurationUs),
                  frameUs + 1);
        std::cout << exportCodecDisplayName(codec).toStdString()
                  << ": encoder=" << result.encoderId.toStdString()
                  << " packets=" << result.validation.videoPacketCount
                  << " videoDurationUs=" << result.validation.videoDurationUs
                  << " audioDurationUs=" << result.validation.audioDurationUs
                  << std::endl;
    }
}

// 30000/1001 and 60 fps exercise the rational time base: the AAC track and the
// CFR video track must still end within one frame of each other.
TEST(ExportMatrix, FractionalAndHighFrameRatesKeepAvWithinOneFrame)
{
    if (!gpuAvailable())
        GTEST_SKIP() << "no D3D11 device";
    const EncoderCatalog catalog = detectEncodersForTest(320, 180, Rational{30, 1});
    for (const Rational fps : {Rational{30000, 1001}, Rational{60, 1}}) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        ExportSettings settings;
        settings.codec = ExportVideoCodec::Mpeg4Part2;
        settings.width = 320;
        settings.height = 180;
        settings.fps = fps;
        settings.rateControl = ExportRateControl::Bitrate;
        settings.bitrateKbps = 4000;
        settings.outputPath = dir.filePath(QStringLiteral("fps.mp4"));
        settings.segmentTargetUs = 500'000;
        ExportPipeline pipeline;
        const ExportResult result = pipeline.run(
            makeExportProject(VIDEOEDITOR_FIXTURE_DIR), settings, catalog,
            false, {});
        ASSERT_TRUE(result.ok) << result.error.toStdString();
        ASSERT_TRUE(result.validation.ok) << result.validation.error.toStdString();
        const int64_t frameUs = (1'000'000LL * fps.den + fps.num / 2) / fps.num;
        EXPECT_LE(std::llabs(result.validation.audioDurationUs
                             - result.validation.videoDurationUs),
                  frameUs + 1)
            << fps.num << "/" << fps.den;
        std::cout << fps.num << "/" << fps.den
                  << ": videoDurationUs=" << result.validation.videoDurationUs
                  << " audioDurationUs=" << result.validation.audioDurationUs
                  << " frameUs=" << frameUs << std::endl;
    }
}

// H.265 is hardware-only. When this machine has an HEVC hardware encoder the
// whole export must succeed through it; otherwise the test skips and says so
// in Chinese, using the catalog's own gate reason.
TEST(ExportMatrix, H265ProducesValidMp4WhenHardwareExists)
{
    if (!gpuAvailable())
        GTEST_SKIP() << "no D3D11 device";
    const EncoderCatalog catalog = detectEncodersForTest(320, 180, Rational{30, 1});
    bool hardware = false;
    QString reason;
    const QString encoder =
        catalog.selectEncoder(ExportVideoCodec::H265, true, &hardware, &reason);
    if (encoder.isEmpty() || !hardware) {
        GTEST_SKIP() << (reason.isEmpty()
                             ? QStringLiteral("没有可用的 H.265 硬件编码器")
                             : reason)
                            .toStdString();
    }
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H265;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 3000;
    settings.outputPath = dir.filePath(QStringLiteral("h265.mp4"));
    settings.segmentTargetUs = 500'000;
    ExportPipeline pipeline;
    const ExportResult result = pipeline.run(
        makeExportProject(VIDEOEDITOR_FIXTURE_DIR), settings, catalog, false,
        {});
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.usedHardware);
    EXPECT_TRUE(result.encoderId.endsWith(QStringLiteral("@hw")));
    EXPECT_TRUE(result.validation.ok) << result.validation.error.toStdString();
    std::cout << "H.265: encoder=" << result.encoderId.toStdString()
              << " packets=" << result.validation.videoPacketCount
              << " videoDurationUs=" << result.validation.videoDurationUs
              << " audioDurationUs=" << result.validation.audioDurationUs
              << std::endl;
}

// The gate itself must reject H.265 before any output or parts directory is
// written when the catalog has no HEVC hardware row.
TEST(ExportMatrix, H265IsExplicitlyDisabledWithoutHardware)
{
    const EncoderCatalog catalog = EncoderCatalog::withoutHardwareForTest();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H265;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.outputPath = dir.filePath(QStringLiteral("nope.mp4"));
    ExportPipeline pipeline;
    const ExportResult result = pipeline.run(
        makeExportProject(VIDEOEDITOR_FIXTURE_DIR), settings, catalog, false,
        {});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.indexOf(QStringLiteral("H.265")), -1)
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(settings.outputPath));
    EXPECT_FALSE(QFileInfo::exists(partsDirectoryFor(settings.outputPath)));
}

// Acceptance gate from the M3 plan: H.264 1080p30 hardware export must run at
// least at realtime (>= 1.0x). It skips cleanly when no H.264 hardware encoder
// is available; the threshold itself is never weakened.
TEST(ExportMatrix, HardwareH264IsAtLeastRealtimeAt1080p)
{
    if (!gpuAvailable())
        GTEST_SKIP() << "no D3D11 device";
    const EncoderCatalog catalog = detectEncodersForTest(1920, 1080, Rational{30, 1});
    bool hardware = false;
    QString reason;
    const QString encoder = catalog.selectEncoder(ExportVideoCodec::H264, true,
                                                  &hardware, &reason);
    if (encoder.isEmpty() || !hardware) {
        GTEST_SKIP() << "no H.264 hardware encoder: " << reason.toStdString();
    }
    Project project = makeExportProject(VIDEOEDITOR_FIXTURE_DIR);
    project.timeline.width = 1920;
    project.timeline.height = 1080;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H264;
    settings.width = 1920;
    settings.height = 1080;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 12'000;
    settings.outputPath = dir.filePath(QStringLiteral("perf.mp4"));
    settings.segmentTargetUs = 2'000'000;
    QElapsedTimer timer;
    timer.start();
    const ExportResult result =
        ExportPipeline().run(project, settings, catalog, false, {});
    const qint64 elapsed = timer.elapsed();
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_TRUE(result.usedHardware);
    const double speed = 2000.0 / double(std::max<qint64>(1, elapsed));
    RecordProperty("encoder", result.encoderId.toStdString());
    RecordProperty("elapsedMs", std::to_string(elapsed));
    RecordProperty("realtimeSpeed", std::to_string(speed));
    std::cout << "1080p30 hardware export: encoder=" << result.encoderId.toStdString()
              << " elapsed=" << elapsed << " ms, " << speed
              << "x realtime (gate >= 1.0x)" << std::endl;
    EXPECT_GE(speed, 1.0) << "hardware export slower than realtime";
}

