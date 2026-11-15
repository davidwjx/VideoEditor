#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include "Confidence.h"

using namespace VideoEditor;

TEST(Confidence, ClassifiesBoundaries)
{
    EXPECT_EQ(classifyConfidence({0.90f, 0.10f, 1000, 1000}),
              FrameConfidence::Good);
    EXPECT_EQ(classifyConfidence({0.70f, 0.10f, 1000, 1000}),
              FrameConfidence::Fair);
    EXPECT_EQ(classifyConfidence({0.50f, 0.10f, 1000, 1000}),
              FrameConfidence::Low);
    EXPECT_EQ(classifyConfidence({0.99f, 0.80f, 1000, 1000}),
              FrameConfidence::Missing);
    // Good IoU but a 40% area change: Fair, never Good/memory-worthy.
    EXPECT_EQ(classifyConfidence({0.90f, 0.10f, 1400, 1000}),
              FrameConfidence::Fair);
    // Occlusion 0.20 is still Good; 0.21 drops to Fair even with a high IoU.
    EXPECT_EQ(classifyConfidence({0.90f, 0.20f, 1000, 1000}),
              FrameConfidence::Good);
    EXPECT_EQ(classifyConfidence({0.90f, 0.21f, 1000, 1000}),
              FrameConfidence::Fair);
}

TEST(Confidence, MemoryGatingRejectsLowAndMissing)
{
    EXPECT_TRUE(shouldWriteMemory(FrameConfidence::Good, 5.0f));
    EXPECT_TRUE(shouldWriteMemory(FrameConfidence::Fair, 40.0f));
    EXPECT_FALSE(shouldWriteMemory(FrameConfidence::Low, 10.0f));
    EXPECT_FALSE(shouldWriteMemory(FrameConfidence::Missing, 0.0f));
    EXPECT_FALSE(shouldWriteMemory(FrameConfidence::Fair, 61.0f));
}

TEST(Confidence, SummaryUsesEnglishStateTokens)
{
    ConfidenceSample good;
    good.timeUs = 1000;
    good.predictedIou = 0.9f;
    good.occlusion = 0.0f;
    good.areaPx = 100;
    good.previousAreaPx = 100;

    ConfidenceSample missing;
    missing.timeUs = 2000;
    missing.occlusion = 0.9f;

    const QJsonObject summary = confidenceSummary({good, missing});
    const QJsonArray frames = summary.value(QStringLiteral("frames")).toArray();
    ASSERT_EQ(frames.size(), 2);
    EXPECT_EQ(frames.at(0).toObject().value(QStringLiteral("state")).toString(),
              QStringLiteral("good"));
    EXPECT_EQ(frames.at(0).toObject().value(QStringLiteral("timeUs")).toInt(),
              1000);
    EXPECT_EQ(frames.at(1).toObject().value(QStringLiteral("state")).toString(),
              QStringLiteral("missing"));
}
