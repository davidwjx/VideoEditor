#include <gtest/gtest.h>

#include "FrameRequestCoalescer.h"

using namespace VideoEditor;

// Simulates a slow render worker: several ticks arrive while the first frame
// is still in flight; only the latest target must be dispatched on completion.
TEST(FrameRequestCoalescer, DropsIntermediateRequestsWhileWorkerIsBusy)
{
    FrameRequestCoalescer gate;

    const auto first = gate.request(10);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 10);
    EXPECT_TRUE(gate.pending());

    EXPECT_FALSE(gate.request(20).has_value());
    EXPECT_FALSE(gate.request(30).has_value());
    EXPECT_EQ(gate.desired(), 30);

    const auto next = gate.complete(10);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 30);
    EXPECT_TRUE(gate.pending());

    EXPECT_FALSE(gate.complete(30).has_value());
    EXPECT_FALSE(gate.pending());
}

TEST(FrameRequestCoalescer, CompletionRacingNewerTargetDispatchesOnlyThatTarget)
{
    FrameRequestCoalescer gate;
    ASSERT_TRUE(gate.request(10).has_value());
    EXPECT_FALSE(gate.request(20).has_value());

    const auto next = gate.complete(10); // still renders frame 20 next
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 20);

    // A seek lands while frame 20 is in flight; it must not queue.
    EXPECT_FALSE(gate.request(25).has_value());
    const auto afterSeek = gate.complete(20);
    ASSERT_TRUE(afterSeek.has_value());
    EXPECT_EQ(*afterSeek, 25);
    EXPECT_FALSE(gate.complete(25).has_value());
}

TEST(FrameRequestCoalescer, ResetClearsPendingState)
{
    FrameRequestCoalescer gate;
    ASSERT_TRUE(gate.request(10).has_value());
    gate.reset();
    EXPECT_FALSE(gate.pending());
    EXPECT_EQ(gate.desired(), -1);

    const auto fresh = gate.request(20);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(*fresh, 20);
}
