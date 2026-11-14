#include <gtest/gtest.h>

#include <QDir>
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

} // namespace

TEST(AudioProbe, ProbesAllBackgroundMusicFormats)
{
    for (const QString name : {QStringLiteral("tone.wav"), QStringLiteral("bgm.mp3"),
                               QStringLiteral("bgm.flac"), QStringLiteral("bgm.m4a")}) {
        const QString path = fixture(name);
        if (!requireFixture(path))
            continue;
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        ASSERT_TRUE(info.has_value()) << name.toStdString() << ": "
                                      << error.toStdString();
        EXPECT_EQ(info->kind, MediaKind::Audio) << name.toStdString();
        EXPECT_FALSE(info->hasVideo) << name.toStdString();
        EXPECT_TRUE(info->hasAudio) << name.toStdString();
        EXPECT_TRUE(info->supported) << info->unsupportedReason.toStdString();
        EXPECT_GT(info->audioSampleRate, 0);
        EXPECT_EQ(info->audioChannels, 1);
        EXPECT_GT(info->audioDurationUs, 0);
        EXPECT_GT(info->durationUs, 0);
    }
}

TEST(AudioProbe, KeepsVideoProbeBehaviour)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!requireFixture(path))
        return;
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, MediaKind::Video);
    EXPECT_TRUE(info->hasVideo);
    EXPECT_TRUE(info->hasAudio);
    EXPECT_GT(info->width, 0);
}

TEST(AudioProbe, ReadsSyncFixtureAudioAndVideo)
{
    const QString path = fixture(QStringLiteral("sync.mp4"));
    if (!requireFixture(path))
        return;
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, MediaKind::Video);
    EXPECT_TRUE(info->hasAudio);
    EXPECT_EQ(info->videoCodec, QStringLiteral("h264"));
    EXPECT_GT(info->audioSampleRate, 0);
}
