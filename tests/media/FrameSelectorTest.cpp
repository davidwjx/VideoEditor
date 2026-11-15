#include <gtest/gtest.h>

#include "FrameSelector.h"

using namespace VideoEditor;

TEST(FrameSelector, ReusesFrameInsideItsDisplayWindow)
{
    const FrameFetchPlan plan = planFrameFetch(1'000'000, 1'010'000, 33'333);
    EXPECT_EQ(plan.action, FrameFetchAction::Reuse);
}

TEST(FrameSelector, DecodesForwardAndCountsDrops)
{
    const FrameFetchPlan plan = planFrameFetch(1'000'000, 1'100'000, 33'333);
    EXPECT_EQ(plan.action, FrameFetchAction::Decode);
    EXPECT_EQ(plan.maxFramesToDrop, 2);
    EXPECT_LE(plan.maxFramesToDrop, 3);
}

TEST(FrameSelector, SeeksBackwards)
{
    EXPECT_EQ(planFrameFetch(2'000'000, 1'500'000, 33'333).action,
              FrameFetchAction::Seek);
}

TEST(FrameSelector, SeeksAcrossLargeJumps)
{
    EXPECT_EQ(planFrameFetch(0, 2'000'000, 33'333).action, FrameFetchAction::Seek);
    EXPECT_EQ(planFrameFetch(-1, 0, 33'333).action, FrameFetchAction::Seek);
}
