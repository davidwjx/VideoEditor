#include <gtest/gtest.h>
#include "RemovalCacheKey.h"

using namespace VideoEditor;

namespace {
RemovalKeyInput baseInput()
{
    RemovalKeyInput in;
    in.assetFingerprint = QString(64, QLatin1Char('a'));
    in.sourceInUs = 0;
    in.sourceOutUs = 10'000'000;
    in.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('b')), 4, 1});
    in.parameters.mode = RepairMode::Fast;
    in.parameters.temporalStabilization = true;
    in.parameters.stabilizationStrength = 50;
    in.parameters.processingWidth = 1920;
    in.parameters.processingHeight = 1080;
    in.trackingModelId = QStringLiteral("sam2.1-hiera-tiny");
    in.trackingModelHash = QString(64, QLatin1Char('c'));
    in.inpaintModelId = QStringLiteral("migan-512");
    in.inpaintModelHash = QString(64, QLatin1Char('d'));
    in.pipelineVersion = QStringLiteral("m4.0");
    in.colorSpace = QStringLiteral("BT.709");
    in.fps = Rational{30, 1};
    in.width = 1920;
    in.height = 1080;
    return in;
}
} // namespace

TEST(RemovalCacheKey, IsStableAndExcludesTimelinePosition)
{
    const RemovalKeyInput in = baseInput();
    const QString first = computeRemovalCacheKey(in);
    EXPECT_EQ(first, computeRemovalCacheKey(in));
    EXPECT_EQ(first.size(), 64);
    const QString json = canonicalRemovalKeyJson(in);
    EXPECT_FALSE(json.contains(QStringLiteral("timeline")));
    EXPECT_FALSE(json.contains(QStringLiteral("timelineStart")));
}

TEST(RemovalCacheKey, ChangesForMaskParametersModelAndRange)
{
    const QString base = computeRemovalCacheKey(baseInput());

    RemovalKeyInput mask = baseInput();
    mask.maskKeyframes[0].maskHash = QString(64, QLatin1Char('e'));
    EXPECT_NE(base, computeRemovalCacheKey(mask));

    RemovalKeyInput feather = baseInput();
    feather.maskKeyframes[0].featherPx = 12;
    EXPECT_NE(base, computeRemovalCacheKey(feather));

    RemovalKeyInput params = baseInput();
    params.parameters.mode = RepairMode::Quality;
    EXPECT_NE(base, computeRemovalCacheKey(params));

    RemovalKeyInput model = baseInput();
    model.inpaintModelHash = QString(64, QLatin1Char('f'));
    EXPECT_NE(base, computeRemovalCacheKey(model));

    RemovalKeyInput range = baseInput();
    range.sourceOutUs = 9'000'000;
    EXPECT_NE(base, computeRemovalCacheKey(range));

    RemovalKeyInput fps = baseInput();
    fps.fps = Rational{60, 1};
    EXPECT_NE(base, computeRemovalCacheKey(fps));
}
