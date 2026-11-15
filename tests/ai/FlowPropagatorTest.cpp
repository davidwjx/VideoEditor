#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_OPENCV) && VIDEOEDITOR_HAVE_OPENCV

#include <algorithm>
#include <iostream>

#include <QElapsedTimer>
#include <QImage>
#include <QList>

#include "FlowPropagator.h"
#include "RemovalEvalMetrics.h"

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

// The acceptance gate is DIS propagation at 1080p. Measure many warm pairs
// and gate on p50 (not one sample) so a single slow scheduling hiccup cannot
// make the result flaky.
TEST(FlowPropagator, Sustains1080pDisThroughput)
{
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    constexpr int kPairs = 12;
    const QRect object(800, 500, 200, 150);

    // One static textured background; p95 stays meaningful without rebuilding
    // 1080p noise for every frame.
    QImage background(kWidth, kHeight, QImage::Format_RGB32);
    for (int y = 0; y < kHeight; ++y) {
        auto *line = reinterpret_cast<QRgb *>(background.scanLine(y));
        for (int x = 0; x < kWidth; ++x) {
            const int value = 40 + int(hashPixel(x, y) % 70u);
            line[x] = qRgb(value, value, value);
        }
    }
    const auto frameAt = [&background, &object](int shift) {
        QImage image = background;
        for (int oy = 0; oy < object.height(); ++oy) {
            const int y = object.y() + oy;
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int ox = 0; ox < object.width(); ++ox) {
                const int x = object.x() + shift + ox;
                if (x < 0 || x >= kWidth)
                    continue;
                const int value =
                    std::min(255, 40 + int(hashPixel(ox, oy) % 70u) + 130);
                line[x] = qRgb(value, value, value);
            }
        }
        return image;
    };

    FlowPropagator propagator;
    for (int i = 0; i < 2; ++i) { // warm-up
        const int shift = i * 4;
        const QImage previous = frameAt(shift);
        const QImage current = frameAt(shift + 4);
        const BinaryMask mask =
            makeMask(object.translated(shift, 0), kWidth, kHeight);
        const FlowPropagationResult warm =
            propagator.propagate(previous, current, mask);
        ASSERT_TRUE(warm.ok) << warm.error.toStdString();
    }

    QList<double> timesMs;
    for (int i = 0; i < kPairs; ++i) {
        const int shift = i * 4;
        const QImage previous = frameAt(shift);
        const QImage current = frameAt(shift + 4);
        const BinaryMask mask =
            makeMask(object.translated(shift, 0), kWidth, kHeight);
        QElapsedTimer clock;
        clock.start();
        const FlowPropagationResult result =
            propagator.propagate(previous, current, mask);
        timesMs.append(double(clock.nsecsElapsed()) / 1'000'000.0);
        ASSERT_TRUE(result.ok) << result.error.toStdString();
        EXPECT_GT(result.areaPx, 0);
    }
    std::sort(timesMs.begin(), timesMs.end());
    const double p50 = timesMs.at(kPairs / 2);
    const double p95 = timesMs.at(int(double(kPairs) * 0.95) - 1);
    const double fps = p50 > 0.0 ? 1000.0 / p50 : 0.0;
    std::cout << "DIS 1080p propagate: p50=" << p50 << " ms (" << fps
              << " fps), p95=" << p95 << " ms over " << kPairs
              << " warm pairs" << std::endl;
    EXPECT_GE(fps, 20.0);
}

#endif // VIDEOEDITOR_HAVE_OPENCV
