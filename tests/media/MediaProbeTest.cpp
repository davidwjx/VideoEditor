#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "MediaProbe.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

bool requireFixture(const QString &path)
{
    if (QFileInfo::exists(path))
        return true;
    // GTEST_SKIP() expands to a void return, so report the skip from a void
    // lambda and let the caller return from the test.
    [&path] { GTEST_SKIP() << "missing fixture: " << path.toStdString(); }();
    return false;
}

void expectSupported(const MediaInfo &info)
{
    EXPECT_TRUE(info.supported) << info.unsupportedReason.toStdString();
    EXPECT_GT(info.width, 0);
    EXPECT_GT(info.height, 0);
    EXPECT_GT(info.durationUs, 0);
    EXPECT_TRUE(info.frameRate.isValid());
}

} // namespace

TEST(MediaProbe, ProbesMpeg4Mp4)
{
    const QString path = fixture(QStringLiteral("mpeg4.mp4"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->container, QStringLiteral("MP4"));
    EXPECT_EQ(info->videoCodec, QStringLiteral("mpeg4"));
    EXPECT_EQ(info->width, 640);
    EXPECT_EQ(info->height, 360);
    EXPECT_TRUE(info->hasAudio);
    expectSupported(*info);
}

TEST(MediaProbe, ProbesH264AndH265)
{
    for (const QString name : {QStringLiteral("h264.mp4"), QStringLiteral("h265.mp4")}) {
        const QString path = fixture(name);
        if (!requireFixture(path)) continue;
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        ASSERT_TRUE(info.has_value()) << error.toStdString();
        EXPECT_EQ(info->videoCodec, name.startsWith(QStringLiteral("h264"))
                                        ? QStringLiteral("h264")
                                        : QStringLiteral("hevc"));
        expectSupported(*info);
    }
}

TEST(MediaProbe, ProbesWmvContainer)
{
    const QString path = fixture(QStringLiteral("wmv2.wmv"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->container, QStringLiteral("WMV"));
    EXPECT_TRUE(info->supported);
    expectSupported(*info);
}

TEST(MediaProbe, ProbesOptionalVc1Sample)
{
    // FFmpeg 没有 VC-1 编码器，无法自动生成素材。放入真实样本后此测试生效。
    const QString path = fixture(QStringLiteral("manual/vc1.wmv"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->videoCodec, QStringLiteral("vc1"));
    EXPECT_TRUE(info->supported);
}

TEST(MediaProbe, ReadsRotationMetadata)
{
    const QString path = fixture(QStringLiteral("rotated.mp4"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->rotationDegrees, 90);
}

TEST(MediaProbe, HandlesUnicodePath)
{
    const QString path = fixture(QStringLiteral("中文 文件名.mp4"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->videoCodec, QStringLiteral("h264"));
}

TEST(MediaProbe, HandlesNoAudioAndMultipleVideoStreams)
{
    const QString noAudio = fixture(QStringLiteral("noaudio.mp4"));
    if (requireFixture(noAudio)) {
        const auto info = MediaProbe::probe(noAudio, nullptr);
        ASSERT_TRUE(info.has_value());
        EXPECT_FALSE(info->hasAudio);
        EXPECT_TRUE(info->supported);
    }

    const QString twoVideo = fixture(QStringLiteral("twovideo.mp4"));
    if (requireFixture(twoVideo)) {
        const auto info = MediaProbe::probe(twoVideo, nullptr);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->width, 320);
        EXPECT_EQ(info->height, 240);
        EXPECT_TRUE(info->supported);
    }
}

TEST(MediaProbe, HandlesVfrDuration)
{
    const QString path = fixture(QStringLiteral("vfr.mp4"));
    if (!requireFixture(path)) return;
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->supported);
    EXPECT_GT(info->durationUs, 2'000'000);
    EXPECT_LT(info->durationUs, 4'000'000);
}

TEST(MediaProbe, ReportsCorruptAndMissingFiles)
{
    QString error;
    EXPECT_FALSE(MediaProbe::probe(fixture(QStringLiteral("corrupt.mp4")), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("无法打开")));

    error.clear();
    EXPECT_FALSE(MediaProbe::probe(fixture(QStringLiteral("missing.mp4")), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("不存在")));
}
