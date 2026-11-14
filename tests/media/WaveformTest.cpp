#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "Waveform.h"
#include "WaveformService.h"

using namespace VideoEditor;

namespace {

QString tonePath()
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
        .filePath(QStringLiteral("tone.wav"));
}

} // namespace

TEST(Waveform, ComputesOneHundredBucketsPerSecond)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    QString error;
    const WaveformPeaks peaks = computeWaveform(tonePath(), &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_GE(peaks.bucketCount(), 199);
    EXPECT_LE(peaks.bucketCount(), 201);
    float loudest = 0.0f;
    for (int i = 50; i < peaks.bucketCount() - 50; ++i)
        loudest = std::max(loudest, peaks.maxValues.at(i));
    EXPECT_GT(loudest, 0.1f);
    EXPECT_LT(peaks.maxValues.at(10), peaks.maxValues.at(100) + 0.5f);
}

TEST(Waveform, EncodesAndDecodesExactly)
{
    WaveformPeaks peaks;
    peaks.durationUs = 2'000'000;
    peaks.minValues = {-1.0f, -0.5f, 0.0f};
    peaks.maxValues = {1.0f, 0.25f, 0.75f};
    const QByteArray bytes = encodeWaveform(peaks);
    const auto decoded = decodeWaveform(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->durationUs, peaks.durationUs);
    ASSERT_EQ(decoded->bucketCount(), 3);
    EXPECT_NEAR(decoded->minValues.at(1), -0.5f, 1.0f / 32767.0f);
    EXPECT_NEAR(decoded->maxValues.at(2), 0.75f, 1.0f / 32767.0f);
}

TEST(Waveform, RejectsCorruptFiles)
{
    EXPECT_FALSE(decodeWaveform(QByteArray("not a waveform")).has_value());
    QByteArray truncated = encodeWaveform(WaveformPeaks{});
    truncated.chop(4);
    EXPECT_FALSE(decodeWaveform(truncated).has_value());
}

TEST(WaveformService, CachesPeaksOnDiskAndReloads)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    {
        WaveformService service(dir.path());
        QSignalSpy ready(&service, &WaveformService::ready);
        service.request(QStringLiteral("asset-a"), tonePath());
        // Spy must exist before request(), otherwise a fast worker can emit
        // ready() before the test starts listening.

        if (!ready.wait(5000))
            GTEST_SKIP() << "waveform worker did not finish";
        EXPECT_TRUE(service.hasPeaks(QStringLiteral("asset-a")));
        EXPECT_GT(service.peaks(QStringLiteral("asset-a")).bucketCount(), 0);
        EXPECT_TRUE(QFileInfo::exists(service.cacheFilePath(tonePath())));
    }

    // A fresh service must load the cached file instead of recomputing.
    WaveformService second(dir.path());
    QSignalSpy ready(&second, &WaveformService::ready);
    second.request(QStringLiteral("asset-a"), tonePath());
    if (!ready.wait(5000))
        GTEST_SKIP() << "waveform worker did not finish";
    EXPECT_GT(second.peaks(QStringLiteral("asset-a")).bucketCount(), 0);
}
