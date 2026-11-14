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
