#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_OPENCV) && VIDEOEDITOR_HAVE_OPENCV

#include <algorithm>

#include <QImage>

#include "FlowPropagator.h"

using namespace VideoEditor;

namespace {

// Deterministic per-pixel texture. The whole frame (including the object) is
// textured so DIS can estimate an interior flow field, not only edges.
uint32_t hashPixel(int x, int y)
{
    uint32_t value = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    value = (value ^ (value >> 13)) * 1274126177u;
    return value ^ (value >> 16);
}

QRect objectRect(int frameWidth, int frameHeight, int shiftX, int shiftY)
{
    Q_UNUSED(frameWidth)
    Q_UNUSED(frameHeight)
    return QRect(20 + shiftX, 24 + shiftY, 28, 22);
}

// Bright textured rectangle on a darker textured background.
QImage makeFrame(int shiftX, int shiftY)
{
    constexpr int kSize = 128;
    QImage image(kSize, kSize, QImage::Format_RGB32);
    const QRect object = objectRect(kSize, kSize, shiftX, shiftY);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const bool inside = object.contains(x, y);
            const int sampleX = inside ? x - object.x() : x;
            const int sampleY = inside ? y - object.y() : y;
            int value = 40 + int(hashPixel(sampleX, sampleY) % 70u);
            if (inside)
                value = std::min(255, value + 130);
            image.setPixel(x, y, qRgb(value, value, value));
        }
    }
    return image;
}

BinaryMask makeMask(const QRect &rect, int width, int height)
{
    BinaryMask mask;
    mask.width = width;
    mask.height = height;
    mask.bits.assign(size_t(width) * size_t(height), 0);
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x)
            mask.set(x, y, true);
    }
    return mask;
}

double maskIou(const BinaryMask &a, const BinaryMask &b)
{
    if (!a.isValid() || !b.isValid() || a.width != b.width
        || a.height != b.height) {
        return 0.0;
    }
    int64_t intersection = 0;
    int64_t unions = 0;
    for (size_t i = 0; i < a.bits.size(); ++i) {
        const bool inA = a.bits[i] != 0;
        const bool inB = b.bits[i] != 0;
        if (inA && inB)
            ++intersection;
        if (inA || inB)
            ++unions;
    }
    return unions > 0 ? double(intersection) / double(unions) : 0.0;
}

} // namespace

TEST(FlowPropagator, TracksMovingRectangle)
{
    constexpr int kSize = 128;
    const QImage first = makeFrame(0, 0);
    const QImage second = makeFrame(8, 6);
    const BinaryMask firstMask =
        makeMask(objectRect(kSize, kSize, 0, 0), kSize, kSize);
    const BinaryMask groundTruth =
        makeMask(objectRect(kSize, kSize, 8, 6), kSize, kSize);

    FlowPropagator propagator;
    const FlowPropagationResult result =
        propagator.propagate(first, second, firstMask);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.mask.width, kSize);
    EXPECT_EQ(result.mask.height, kSize);
    EXPECT_GT(result.areaPx, 0);
    EXPECT_GE(maskIou(result.mask, groundTruth), 0.8);
}

TEST(FlowPropagator, PropagatesBackwardOppositeToForward)
{
    constexpr int kSize = 128;
    const QImage first = makeFrame(0, 0);
    const QImage second = makeFrame(8, 6);
    const BinaryMask secondMask =
        makeMask(objectRect(kSize, kSize, 8, 6), kSize, kSize);
    const BinaryMask groundTruth =
        makeMask(objectRect(kSize, kSize, 0, 0), kSize, kSize);

    FlowPropagator propagator;
    const FlowPropagationResult result =
        propagator.propagateBackward(first, second, secondMask);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_GE(maskIou(result.mask, groundTruth), 0.8);
    // It must have moved back, not stayed at the current-frame position.
    EXPECT_LT(maskIou(result.mask, secondMask), 0.9);
}

TEST(FlowPropagator, StillObjectKeepsMask)
{
    constexpr int kSize = 128;
    const QImage first = makeFrame(0, 0);
    const QImage second = makeFrame(0, 0);
    const BinaryMask firstMask =
        makeMask(objectRect(kSize, kSize, 0, 0), kSize, kSize);

    FlowPropagator propagator;
    const FlowPropagationResult result =
        propagator.propagate(first, second, firstMask);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_GE(maskIou(result.mask, firstMask), 0.95);
    EXPECT_LT(result.meanFlowPx, 1.0f);
}

TEST(FlowPropagator, RejectsMismatchedInputs)
{
    const QImage first = makeFrame(0, 0);
    const QImage second = makeFrame(3, 3).scaled(64, 64);
    const BinaryMask mask = makeMask(objectRect(128, 128, 0, 0), 128, 128);

    FlowPropagator propagator;
    const FlowPropagationResult result =
        propagator.propagate(first, second, mask);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
}

#endif // VIDEOEDITOR_HAVE_OPENCV
