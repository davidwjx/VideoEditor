#include <gtest/gtest.h>

#include "TimeBase.h"

using namespace VideoEditor;

TEST(TimeBase, ConvertsFramesAtThirtyFps)
{
    EXPECT_EQ(framesToMicros(30, Rational{30, 1}), 1'000'000);
    EXPECT_EQ(framesToMicros(1, Rational{30, 1}), 33'333);
}

TEST(TimeBase, ConvertsNtscRateWithoutDrift)
{
    EXPECT_EQ(framesToMicros(30, Rational{30000, 1001}), 1'001'000);
    EXPECT_EQ(microsToFrames(1'001'000, Rational{30000, 1001}), 30);
}

TEST(TimeBase, FormatsTimecodeAndDuration)
{
    EXPECT_EQ(formatTimecode(3'661'000'000, Rational{30, 1}),
              QStringLiteral("01:01:01:00"));
    EXPECT_EQ(formatDuration(3'661'000'000),
              QStringLiteral("01:01:01.000"));
}

TEST(TimeBase, ClampsNegativeValues)
{
    EXPECT_EQ(formatTimecode(-5, Rational{30, 1}), QStringLiteral("00:00:00:00"));
    EXPECT_EQ(formatDuration(-5), QStringLiteral("00:00:00.000"));
}

TEST(TimeBase, FormatsDropFrameTimecode)
{
    const Rational ntsc{30000, 1001};
    EXPECT_EQ(formatTimecodeAtRate(1'800 * 1'001'000LL / 30, ntsc, true),
              QStringLiteral("00:01:00;02"));
    EXPECT_EQ(formatTimecodeAtRate(17'982 * 1'001'000LL / 30, ntsc, true),
              QStringLiteral("00:10:00;00"));
    EXPECT_EQ(formatTimecodeAtRate(0, ntsc, true), QStringLiteral("00:00:00;00"));
    EXPECT_EQ(formatTimecodeAtRate(1'000'000, Rational{30, 1}, false),
              QStringLiteral("00:00:01:00"));
}

TEST(TimeBase, FormatsDropFrameTimecodeAtSixtyNtsc)
{
    const Rational ntsc60{60000, 1001};
    EXPECT_EQ(formatTimecodeAtRate(3'600 * 1'001'000LL / 60, ntsc60, true),
              QStringLiteral("00:01:00;04"));
    EXPECT_EQ(formatTimecodeAtRate(35'964 * 1'001'000LL / 60, ntsc60, true),
              QStringLiteral("00:10:00;00"));
}

TEST(TimeBase, ParsesTimecodeEntry)
{
    const Rational fps{30, 1};
    EXPECT_EQ(parseTimecode(QStringLiteral("00:00:01:15"), fps),
              std::optional<int64_t>(1'500'000));
    EXPECT_EQ(parseTimecode(QStringLiteral("00:00:01;15"), fps),
              std::optional<int64_t>(1'500'000));
    EXPECT_EQ(parseTimecode(QStringLiteral("00:01:02.250"), fps),
              std::optional<int64_t>(62'250'000));
    EXPECT_EQ(parseTimecode(QStringLiteral("01:02.250"), fps),
              std::optional<int64_t>(62'250'000));
    EXPECT_EQ(parseTimecode(QStringLiteral("2.5"), fps),
              std::optional<int64_t>(2'500'000));
    EXPECT_EQ(parseTimecode(QStringLiteral(" 2 "), fps),
              std::optional<int64_t>(2'000'000));

    EXPECT_FALSE(parseTimecode(QStringLiteral(""), fps).has_value());
    EXPECT_FALSE(parseTimecode(QStringLiteral("00:00:01:30"), fps).has_value());
    EXPECT_FALSE(parseTimecode(QStringLiteral("00:xx:01"), fps).has_value());
    EXPECT_FALSE(parseTimecode(QStringLiteral("-1"), fps).has_value());
    EXPECT_FALSE(parseTimecode(QStringLiteral("1:2:3:4:5"), fps).has_value());
}

TEST(TimeBase, IgnoresDropFrameForIntegerRates)
{
    // Drop-frame is only defined for NTSC 29.97/59.94. An integer rate must
    // keep every frame label even when the flag is set.
    EXPECT_EQ(formatTimecodeAtRate(60'000'000, Rational{30, 1}, true),
              QStringLiteral("00:01:00:00"));
    EXPECT_EQ(formatTimecodeAtRate(60'000'000, Rational{60, 1}, true),
              QStringLiteral("00:01:00:00"));
}
