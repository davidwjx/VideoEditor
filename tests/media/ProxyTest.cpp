#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "AssetFingerprint.h"
#include "CacheStore.h"
#include "D3D11Context.h"
#include "IVideoDecoder.h"
#include "MediaProbe.h"
#include "ProxyGenerator.h"
#include "ProxyPlanner.h"
#include "VideoDecoderFactory.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(ProxyPlanner, TriggersOnlyAbove1080p)
{
    MediaInfo info;
    info.width = 1920;
    info.height = 1080;
    EXPECT_FALSE(planProxy(info).needed);

    info.width = 3840;
    info.height = 2160;
    const ProxyPlan plan = planProxy(info);
    EXPECT_TRUE(plan.needed);
    EXPECT_EQ(plan.height, 540);
}

TEST(ProxyPlanner, NeedsGenerationWhenProxyMissing)
{
    MediaInfo highres;
    highres.width = 2560;
    highres.height = 1440;
    EXPECT_TRUE(needsProxyGeneration(highres, QString()));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString existing = dir.filePath(QStringLiteral("proxy.mp4"));
    QFile file(existing);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("proxy");
    file.close();
    EXPECT_FALSE(needsProxyGeneration(highres, existing));
    EXPECT_TRUE(needsProxyGeneration(
        highres, dir.filePath(QStringLiteral("missing.mp4"))));

    MediaInfo fullHd;
    fullHd.width = 1920;
    fullHd.height = 1080;
    EXPECT_FALSE(needsProxyGeneration(fullHd, QString()));
}

TEST(ProxyBundling, ShipsFfmpegNextToApp)
{
    // media_tests and VideoEditor are emitted into the same configuration
    // directory, so the app's bundled CLI is visible here once it is built.
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("ffmpeg.exe"));
    if (!QFileInfo::exists(bundled))
        GTEST_SKIP() << "VideoEditor app target was not built: "
                     << bundled.toStdString();
    EXPECT_TRUE(QFileInfo(bundled).isExecutable());
}

TEST(AssetFingerprint, ChangesWithContentAndMtime)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.bin"));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello");
    file.close();

    const QString first = fingerprintFile(path);
    EXPECT_FALSE(first.isEmpty());

    ASSERT_TRUE(file.open(QIODevice::Append));
    file.write("world");
    file.close();
    EXPECT_NE(fingerprintFile(path), first);
}

TEST(CacheStore, BuildsProxyPathsUnderRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CacheStore cache(dir.path());
    const QString path = cache.proxyPath(QStringLiteral("abc-h540"));
    EXPECT_TRUE(path.startsWith(QDir(dir.path()).absolutePath()));
    EXPECT_TRUE(path.endsWith(QStringLiteral(".mp4")));
    EXPECT_TRUE(cache.clear());
}

TEST(ProxyGenerator, Generates540pProxy)
{
    const QString source = fixture(QStringLiteral("highres.mp4"));
    if (!QFile::exists(source))
        GTEST_SKIP() << "missing highres fixture";

    const QString ffmpeg = qEnvironmentVariable("VIDEOEDITOR_FFMPEG",
                                                QStringLiteral("ffmpeg"));
    if (QStandardPaths::findExecutable(ffmpeg).isEmpty())
        GTEST_SKIP() << "ffmpeg not found";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("proxy.mp4"));

    ProxyGenerator generator(ffmpeg);
    QSignalSpy finished(&generator, &ProxyGenerator::finished);
    QSignalSpy failed(&generator, &ProxyGenerator::failed);
    generator.start(source, output, 540);

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 120000);
    ASSERT_EQ(failed.count(), 0);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_TRUE(QFileInfo::exists(output));

    const auto info = MediaProbe::probe(output, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->height, 540);
}

TEST(ProxyGenerator, GeneratedProxyDecodesThroughFactory)
{
    const QString source = fixture(QStringLiteral("highres.mp4"));
    if (!QFile::exists(source))
        GTEST_SKIP() << "missing highres fixture";

    const QString ffmpeg = qEnvironmentVariable("VIDEOEDITOR_FFMPEG",
                                                QStringLiteral("ffmpeg"));
    if (QStandardPaths::findExecutable(ffmpeg).isEmpty())
        GTEST_SKIP() << "ffmpeg not found";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("proxy.mp4"));

    ProxyGenerator generator(ffmpeg);
    QSignalSpy failed(&generator, &ProxyGenerator::failed);
    generator.start(source, output, 540);
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0 || QFileInfo::exists(output),
                             120000);
    ASSERT_EQ(failed.count(), 0);

    QString contextError;
    auto context = D3D11Context::create(true, &contextError);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    // The MJPEG proxy has no D3D11VA decoder. The factory must fall back to
    // software instead of opening a hardware decoder whose first packet then
    // fails with EINVAL (the bug this test guards).
    const auto sourceInfo = MediaProbe::probe(source, nullptr);
    ASSERT_TRUE(sourceInfo.has_value());
    auto result = createVideoDecoder(output, *context, *sourceInfo);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    EXPECT_FALSE(result.decoder->isHardware());

    DecodedFrame frame;
    QString decodeError;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &decodeError))
        << decodeError.toStdString();
    EXPECT_EQ(frame.width, 960);
    EXPECT_EQ(frame.height, 540);
}
