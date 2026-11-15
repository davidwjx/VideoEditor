#include <gtest/gtest.h>

#include "ExportDiskSpace.h"

using namespace VideoEditor;

TEST(ExportDiskSpace, RequiredIsPositiveAndIncludesHeadroom)
{
    ExportSettings settings;
    settings.width = 1920;
    settings.height = 1080;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 12'000;
    const ExportTiming timing =
        computeExportTiming(10'000'000, settings.fps, 48000, 8'000'000);
    const qint64 required = estimateExportBytes(settings, timing);
    // 12 Mbps * 10 s = 15 MB video + 0.24 MB audio + headroom.
    EXPECT_GE(required, 15LL * 1024 * 1024);
    EXPECT_LE(required, 64LL * 1024 * 1024);
}

TEST(ExportDiskSpace, EvaluatorExplainsShortfall)
{
    const auto ok = evaluateDiskSpace(1LL << 30, 100LL << 20);
    EXPECT_TRUE(ok.ok);
    const auto bad = evaluateDiskSpace(10LL << 20, 100LL << 20);
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.error.indexOf(QStringLiteral("磁盘空间")), -1);
    EXPECT_NE(bad.error.indexOf(QStringLiteral("100")), -1);
    EXPECT_NE(bad.error.indexOf(QStringLiteral("10")), -1);
}
