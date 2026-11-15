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

TEST(SubtitleRasterizer, CacheKeyChangesWithText)
{
    const QString a = SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360);
    const QString b = SubtitleRasterizer::cacheKey({layer(QStringLiteral("B"))}, 640, 360);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360));
}
