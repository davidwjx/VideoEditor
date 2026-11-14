#include <gtest/gtest.h>

#include "MediaInfo.h"

using namespace VideoEditor;

TEST(MediaInfo, DefaultConstructedValuesAreEqual)
{
    const MediaInfo a;
    const MediaInfo b;
    EXPECT_EQ(a, b);
}

TEST(MediaInfo, StaysComparableWhenAllFieldsSet)
{
    const MediaInfo defaults;

    MediaInfo info;
    info.path = QStringLiteral("C:/media/sample.mp4");
    info.container = QStringLiteral("mp4");
    info.videoCodec = QStringLiteral("h264");
    info.audioCodec = QStringLiteral("aac");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30000, 1001};
    info.durationUs = 1'001'000;
    info.rotationDegrees = 90;
    info.hasAudio = true;
    info.supported = true;
    info.unsupportedReason = QStringLiteral("不支持的编码");

    EXPECT_NE(info, defaults);
}

TEST(MediaInfo, RoundTripsRepresentativeValuesByCopy)
{
    MediaInfo source;
    source.path = QStringLiteral("C:/media/badminton.mp4");
    source.container = QStringLiteral("mp4");
    source.videoCodec = QStringLiteral("mpeg4");
    source.audioCodec = QStringLiteral("mp3");
    source.width = 1920;
    source.height = 1080;
    source.frameRate = Rational{30000, 1001};
    source.durationUs = 1'001'000;
    source.rotationDegrees = 0;
    source.hasAudio = true;
    source.supported = true;
    source.unsupportedReason.clear();

    const MediaInfo copy = source;

    EXPECT_EQ(copy, source);
    EXPECT_EQ(copy.path, source.path);
    EXPECT_EQ(copy.width, 1920);
    EXPECT_EQ(copy.height, 1080);
    EXPECT_EQ(copy.frameRate, (Rational{30000, 1001}));
    EXPECT_EQ(copy.durationUs, 1'001'000);
    EXPECT_TRUE(copy.hasAudio);
    EXPECT_TRUE(copy.supported);
}
