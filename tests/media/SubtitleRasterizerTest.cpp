#include <gtest/gtest.h>

#include <QImage>

#include "SubtitleRasterizer.h"

using namespace VideoEditor;

namespace {

SubtitleLayer layer(const QString &text)
{
    SubtitleLayer value;
    value.trackId = QStringLiteral("s1");
    value.cueId = QStringLiteral("cue-1");
    value.text = text;
    value.style = SubtitleStyle{};
    value.positionXPermille = 500;
    value.positionYPermille = 880;
    return value;
}

} // namespace

TEST(SubtitleRasterizer, EmptyListProducesTransparentImage)
{
    const QImage image = SubtitleRasterizer::render({}, 640, 360, {SubtitleStyle{}});
    ASSERT_EQ(image.size(), QSize(640, 360));
    for (int y = 0; y < image.height(); y += 13) {
        for (int x = 0; x < image.width(); x += 13)
            ASSERT_EQ(qAlpha(image.pixel(x, y)), 0);
    }
}

TEST(SubtitleRasterizer, DrawsChineseTextNearBottomCenter)
{
    const QImage image = SubtitleRasterizer::render(
        {layer(QStringLiteral("测试字幕"))}, 640, 360, {SubtitleStyle{}});
    ASSERT_EQ(image.size(), QSize(640, 360));
    int ink = 0;
    int minX = 640;
    int maxX = 0;
    int minY = 360;
    int maxY = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) == 0)
                continue;
            ++ink;
            minX = qMin(minX, x);
            maxX = qMax(maxX, x);
            minY = qMin(minY, y);
            maxY = qMax(maxY, y);
        }
    }
    EXPECT_GT(ink, 50);
    EXPECT_GT(minY, 200);          // lower third
    EXPECT_GT((minX + maxX) / 2, 200);
    EXPECT_LT((minX + maxX) / 2, 440);
}

TEST(SubtitleRasterizer, HonorsHorizontalAlignment)
{
    const auto horizontalCenter = [](const QImage &image) {
        int minX = image.width();
        int maxX = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) == 0)
                    continue;
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
            }
        }
        return maxX < 0 ? -1 : (minX + maxX) / 2;
    };

    auto aligned = [](int alignment) {
        SubtitleLayer value = layer(QStringLiteral("测试字幕"));
        value.style.alignment = alignment;
        return value;
    };

    const QImage left = SubtitleRasterizer::render({aligned(1)}, 640, 360, {});
    const QImage right = SubtitleRasterizer::render({aligned(3)}, 640, 360, {});
    EXPECT_GT(horizontalCenter(left), 360);
    EXPECT_LT(horizontalCenter(right), 280);
}

TEST(SubtitleRasterizer, CacheKeyChangesWithText)
{
    const QString a = SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360);
    const QString b = SubtitleRasterizer::cacheKey({layer(QStringLiteral("B"))}, 640, 360);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360));
}
