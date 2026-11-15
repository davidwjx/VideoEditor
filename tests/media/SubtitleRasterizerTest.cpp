#include <gtest/gtest.h>

#include <QColor>
#include <QImage>

#include "Project.h"
#include "SubtitleRasterizer.h"
#include "TimelineEvaluator.h"

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

namespace {

const SubtitleStyle &builtinStyle(const QString &id)
{
    static const QList<SubtitleStyle> styles = builtinSubtitleStyles();
    for (const SubtitleStyle &style : styles) {
        if (style.id == id)
            return style;
    }
    static const SubtitleStyle fallback;
    return fallback;
}

int differingPixels(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return -1;
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                ++count;
        }
    }
    return count;
}

} // namespace

TEST(SubtitleRasterizer, BuiltinPresetsRenderVisuallyDifferentPixels)
{
    SubtitleLayer value = layer(QStringLiteral("测试字幕"));
    value.style = builtinStyle(QStringLiteral("default"));
    const QImage fallback = SubtitleRasterizer::render(
        {value}, 640, 360, builtinSubtitleStyles());

    value.style = builtinStyle(QStringLiteral("boxed"));
    const QImage boxed = SubtitleRasterizer::render(
        {value}, 640, 360, builtinSubtitleStyles());

    value.style = builtinStyle(QStringLiteral("bold-outline"));
    const QImage bold = SubtitleRasterizer::render(
        {value}, 640, 360, builtinSubtitleStyles());

    EXPECT_GT(differingPixels(fallback, boxed), 100)
        << "boxed must add its black background";
    EXPECT_GT(differingPixels(fallback, bold), 100)
        << "bold-outline must change font size/outline";
    EXPECT_GT(differingPixels(boxed, bold), 100);
}

TEST(SubtitleRasterizer, EvaluatedBuiltinPresetReachesRasterizer)
{
    // End-to-end: a cue referencing "boxed" on a project that only stored the
    // default style must still resolve to the builtin preset and paint a
    // background. This is the regression for presets silently rendering as
    // the default.
    Project project;
    project.ensureDefaultTracks();
    project.subtitleStyles = {SubtitleStyle{}};
    Track &subtitleTrack = project.tracks[2];
    SubtitleCue cue;
    cue.id = QStringLiteral("cue-boxed");
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("黑底白字");
    cue.styleId = QStringLiteral("boxed");
    subtitleTrack.cues.append(cue);

    const FrameComposition frame =
        TimelineEvaluator(project).frameAt(500'000);
    ASSERT_EQ(frame.subtitles.size(), 1);
    const QImage image = SubtitleRasterizer::render(
        frame.subtitles, 320, 180, project.subtitleStyles);

    // The boxed background is translucent black across the text block; the
    // default preset leaves those pixels transparent.
    int boxedAlpha = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 0 && color.red() < 60 && color.green() < 60
                && color.blue() < 60) {
                ++boxedAlpha;
            }
        }
    }
    EXPECT_GT(boxedAlpha, 20) << "boxed background was not rasterized";
}

TEST(SubtitleRasterizer, CacheKeyChangesWithAlignment)
{
    SubtitleLayer left = layer(QStringLiteral("A"));
    left.style.alignment = 1;
    SubtitleLayer right = layer(QStringLiteral("A"));
    right.style.alignment = 3;
    EXPECT_NE(SubtitleRasterizer::cacheKey({left}, 640, 360),
              SubtitleRasterizer::cacheKey({right}, 640, 360));
}

TEST(SubtitleRasterizer, CacheKeyChangesWithText)
{
    const QString a = SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360);
    const QString b = SubtitleRasterizer::cacheKey({layer(QStringLiteral("B"))}, 640, 360);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360));
}
