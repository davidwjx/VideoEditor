#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

    QString cacheFile;
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
        cacheFile = service.cacheFilePath(tonePath());
        EXPECT_TRUE(QFileInfo::exists(cacheFile));

        QFile cached(cacheFile);
        ASSERT_TRUE(cached.open(QIODevice::ReadOnly));
        const auto decoded = decodeWaveform(cached.readAll());
        ASSERT_TRUE(decoded.has_value());
        EXPECT_GT(decoded->bucketCount(), 0);
    }

    // Seed the cache with a distinctive payload. A fresh service must load it
    // instead of recomputing from the source.
    WaveformPeaks seeded;
    seeded.durationUs = 123'456;
    seeded.minValues = {-0.25f};
    seeded.maxValues = {0.5f};
    {
        const QByteArray bytes = encodeWaveform(seeded);
        QFile cache(cacheFile);
        ASSERT_TRUE(cache.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(cache.write(bytes), qint64(bytes.size()));
    }

    WaveformService second(dir.path());
    QSignalSpy ready(&second, &WaveformService::ready);
    second.request(QStringLiteral("asset-a"), tonePath());
    if (!ready.wait(5000))
        GTEST_SKIP() << "waveform worker did not finish";
    const WaveformPeaks loaded = second.peaks(QStringLiteral("asset-a"));
    ASSERT_EQ(loaded.bucketCount(), 1);
    EXPECT_EQ(loaded.durationUs, seeded.durationUs);
    EXPECT_NEAR(loaded.maxValues.at(0), 0.5f, 1.0f / 32767.0f);
}

TEST(WaveformService, SurfacesFailureAndDoesNotRetryImplicitly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("missing.wav"));

    WaveformService service(dir.path());
    QSignalSpy ready(&service, &WaveformService::ready);
    QSignalSpy failed(&service, &WaveformService::failed);
    service.request(QStringLiteral("bad"), missing);
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_EQ(failed.count(), 1);
    EXPECT_TRUE(ready.isEmpty());
    EXPECT_FALSE(service.hasPeaks(QStringLiteral("bad")));
    const QString message = failed.first().at(1).toString();
    EXPECT_FALSE(message.isEmpty());
    EXPECT_EQ(service.lastError(QStringLiteral("bad")), message);

    // A repeated request is a no-op: failed() is not emitted again.
    service.request(QStringLiteral("bad"), missing);
    QCoreApplication::processEvents();
    EXPECT_EQ(failed.count(), 1);

    // The explicit retry API allows a second attempt.
    service.retry(QStringLiteral("bad"), missing);
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_EQ(failed.count(), 2);
}

TEST(WaveformService, KeepsGoodPeaksWhenLaterRequestFails)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    WaveformService service(dir.path());
    QSignalSpy ready(&service, &WaveformService::ready);
    service.request(QStringLiteral("asset-a"), tonePath());
    if (!ready.wait(5000))
        GTEST_SKIP() << "waveform worker did not finish";
    const WaveformPeaks before = service.peaks(QStringLiteral("asset-a"));
    ASSERT_FALSE(before.isEmpty());

    QSignalSpy failed(&service, &WaveformService::failed);
    service.request(QStringLiteral("asset-a"),
                    dir.filePath(QStringLiteral("missing.wav")));
    QCoreApplication::processEvents();
    EXPECT_TRUE(failed.isEmpty());
    EXPECT_TRUE(service.hasPeaks(QStringLiteral("asset-a")));
    EXPECT_EQ(service.peaks(QStringLiteral("asset-a")).bucketCount(),
              before.bucketCount());
    EXPECT_TRUE(service.lastError(QStringLiteral("asset-a")).isEmpty());
}

TEST(WaveformService, RegeneratesCorruptCache)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    WaveformService service(dir.path());
    const QString cacheFile = service.cacheFilePath(tonePath());
    ASSERT_FALSE(cacheFile.isEmpty());
    ASSERT_TRUE(QDir().mkpath(QFileInfo(cacheFile).absolutePath()));
    {
        QFile cache(cacheFile);
        ASSERT_TRUE(cache.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(cache.write("not a waveform"), 0);
    }

    QSignalSpy ready(&service, &WaveformService::ready);
    QSignalSpy failed(&service, &WaveformService::failed);
    service.request(QStringLiteral("asset-a"), tonePath());
    ASSERT_TRUE(ready.wait(5000));
    EXPECT_TRUE(failed.isEmpty());
    ASSERT_TRUE(service.hasPeaks(QStringLiteral("asset-a")));

    QFile cached(cacheFile);
    ASSERT_TRUE(cached.open(QIODevice::ReadOnly));
    const auto decoded = decodeWaveform(cached.readAll());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_GT(decoded->bucketCount(), 0);
}
