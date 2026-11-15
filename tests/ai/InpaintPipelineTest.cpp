#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_ORT) && VIDEOEDITOR_HAVE_ORT

#include <QColor>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "InpaintPipeline.h"
#include "RemovalCache.h"

using namespace VideoEditor;

namespace {

class SolidInpainter : public Inpainter {
public:
    std::optional<PatchResult> repair(const QImage &crop,
                                      const BinaryMask &maskCrop,
                                      QString *) override
    {
        QImage out = crop.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x)
                if (maskCrop.at(x, y))
                    out.setPixelColor(x, y, QColor(200, 30, 30, 255));
        return PatchResult{out, QRect()};
    }
};

BinaryMask rectangularMask(int width, int height, const QRect &rect)
{
    BinaryMask mask;
    mask.width = width;
    mask.height = height;
    mask.bits.assign(size_t(width) * size_t(height), 0);
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x)
            mask.set(x, y, true);
    return mask;
}

QImage solidFrame(int width, int height, const QColor &color)
{
    QImage frame(width, height, QImage::Format_RGB32);
    frame.fill(color);
    return frame;
}

} // namespace

TEST(InpaintPipeline, CropIsPaddedClampedAndFeathered)
{
    QImage frame(320, 240, QImage::Format_RGB32);
    frame.fill(Qt::darkGreen);
    BinaryMask mask;
    mask.width = 320;
    mask.height = 240;
    mask.bits.assign(320 * 240, 0);
    for (int y = 100; y < 140; ++y)
        for (int x = 140; x < 180; ++x)
            mask.set(x, y, true);

    InpaintPipeline pipeline;
    pipeline.setInpainterForTest(std::make_unique<SolidInpainter>());
    RemovalParameters params;
    params.mode = RepairMode::Fast;
    params.processingWidth = 320;
    params.processingHeight = 240;

    QString error;
    const auto patches = pipeline.repairChunk({frame}, {mask}, params, 8,
                                              [](int, const QString &) { return true; },
                                              &error);
    ASSERT_TRUE(patches.has_value()) << error.toStdString();
    ASSERT_EQ(patches->size(), 1);
    const PatchResult &patch = patches->first();
    EXPECT_FALSE(patch.crop.isEmpty());
    EXPECT_TRUE(QRect(0, 0, 320, 240).contains(patch.crop));
    EXPECT_EQ(patch.rgba.format(), QImage::Format_ARGB32);

    // The mask centre is opaque; outside the hole alpha is zero.
    const QPoint center = patch.crop.center() - patch.crop.topLeft();
    EXPECT_EQ(qAlpha(patch.rgba.pixel(center)), 255);
    EXPECT_EQ(qAlpha(patch.rgba.pixel(0, 0)), 0);
}

TEST(InpaintPipeline, ChoosesSmallerTileWhenVramBudgetIsTight)
{
    EXPECT_EQ(InpaintPipeline::chooseTileSize(512, 512, 4ll * 1024 * 1024 * 1024), 512);
    EXPECT_LE(InpaintPipeline::chooseTileSize(512, 512, 512ll * 1024 * 1024), 384);
    EXPECT_LE(InpaintPipeline::chooseTileSize(512, 512, 128ll * 1024 * 1024), 256);
}

TEST(InpaintPipeline, EmptyMaskProducesNoPatch)
{
    QImage frame(64, 64, QImage::Format_RGB32);
    frame.fill(Qt::black);
    BinaryMask mask;
    mask.width = mask.height = 64;
    mask.bits.assign(64 * 64, 0);
    InpaintPipeline pipeline;
    pipeline.setInpainterForTest(std::make_unique<SolidInpainter>());
    QString error;
    const auto patches = pipeline.repairChunk({frame}, {mask}, RemovalParameters{}, 4,
                                              [](int, const QString &) { return true; },
                                              &error);
    ASSERT_TRUE(patches.has_value()) << error.toStdString();
    EXPECT_TRUE(patches->isEmpty());
}

TEST(InpaintPipeline, FrameWithoutMaskYieldsTransparentPatch)
{
    QImage first = solidFrame(96, 96, Qt::darkGreen);
    QImage second = solidFrame(96, 96, Qt::darkGreen);
    const BinaryMask hole = rectangularMask(96, 96, QRect(30, 30, 24, 24));
    BinaryMask empty;
    empty.width = empty.height = 96;
    empty.bits.assign(96 * 96, 0);

    InpaintPipeline pipeline;
    pipeline.setInpainterForTest(std::make_unique<SolidInpainter>());
    QString error;
    const auto patches = pipeline.repairChunk(
        {first, second}, {hole, empty}, RemovalParameters{}, 4,
        [](int, const QString &) { return true; }, &error);
    ASSERT_TRUE(patches.has_value()) << error.toStdString();
    ASSERT_EQ(patches->size(), 2);
    EXPECT_EQ(patches->at(1).crop, patches->at(0).crop);
    for (int y = 0; y < patches->at(1).rgba.height(); ++y)
        for (int x = 0; x < patches->at(1).rgba.width(); ++x)
            EXPECT_EQ(qAlpha(patches->at(1).rgba.pixel(x, y)), 0);
}

TEST(InpaintPipeline, FeatherAlphaRampsFromBoundaryToCentre)
{
    const BinaryMask mask = rectangularMask(64, 64, QRect(16, 16, 32, 32));
    const QImage alpha = InpaintPipeline::featherAlpha(mask, 8);
    ASSERT_EQ(alpha.size(), QSize(64, 64));

    // Just inside the boundary the alpha ramps; at least 8 px in it saturates.
    EXPECT_LT(qAlpha(alpha.pixel(16, 32)), 255);
    EXPECT_GT(qAlpha(alpha.pixel(17, 32)), qAlpha(alpha.pixel(16, 32)));
    EXPECT_EQ(qAlpha(alpha.pixel(24, 32)), 255);
    EXPECT_EQ(qAlpha(alpha.pixel(15, 32)), 0);
    EXPECT_EQ(qAlpha(alpha.pixel(0, 0)), 0);

    // featherPx = 0 hardens the edge to the mask.
    const QImage hard = InpaintPipeline::featherAlpha(mask, 0);
    EXPECT_EQ(qAlpha(hard.pixel(16, 32)), 255);
    EXPECT_EQ(qAlpha(hard.pixel(15, 32)), 0);
}

TEST(InpaintPipeline, ColorMatchShiftsPatchTowardSourceRing)
{
    QImage source(64, 64, QImage::Format_RGB32);
    source.fill(QColor(100, 110, 120));
    const BinaryMask mask = rectangularMask(64, 64, QRect(20, 20, 24, 24));

    QImage patch(64, 64, QImage::Format_ARGB32);
    patch.fill(QColor(0, 0, 0, 255));

    ASSERT_TRUE(InpaintPipeline::colorMatch(source, &patch, mask));
    const QColor center = patch.pixelColor(32, 32);
    EXPECT_GE(center.red(), 95);
    EXPECT_GE(center.green(), 105);
    EXPECT_GE(center.blue(), 115);
    EXPECT_EQ(center.alpha(), 255);
}

TEST(InpaintPipeline, ChunkRoundTripsThroughRemovalCacheWithAlpha)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString key(64, QLatin1Char('a'));
    RemovalCache cache(temp.path());

    QImage frame = solidFrame(64, 64, Qt::darkGreen);
    const BinaryMask mask = rectangularMask(64, 64, QRect(20, 20, 24, 24));
    InpaintPipeline pipeline;
    pipeline.setInpainterForTest(std::make_unique<SolidInpainter>());

    QString error;
    const auto patches = pipeline.repairChunk(
        {frame}, {mask}, RemovalParameters{}, 8,
        [](int, const QString &) { return true; }, &error);
    ASSERT_TRUE(patches.has_value()) << error.toStdString();
    ASSERT_EQ(patches->size(), 1);

    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, {patches->at(0).rgba},
                                 {patches->at(0).crop}, &index, &error))
        << error.toStdString();

    RemovalCacheManifest manifest;
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("clip-roundtrip");
    manifest.width = 64;
    manifest.height = 64;
    manifest.fps = Rational{30, 1};
    manifest.pipelineVersion = QStringLiteral("m4.0");
    manifest.status = QStringLiteral("complete");
    manifest.totalFrames = 1;
    manifest.chunks.append(
        RemovalCacheChunk{0, 0, 1, index.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();
    ASSERT_TRUE(cache.verify(key, &error)) << error.toStdString();

    ASSERT_EQ(index.frames.size(), 1);
    const QImage stored = cache.readFrameImage(key, index.frames.first(),
                                               &error);
    ASSERT_FALSE(stored.isNull()) << error.toStdString();
    EXPECT_EQ(stored.format(), QImage::Format_ARGB32);
    const QPoint holeCenter = QPoint(32, 32) - patches->at(0).crop.topLeft();
    EXPECT_EQ(qAlpha(stored.pixel(holeCenter)), 255);
    const QPoint outside = QPoint(2, 2) - patches->at(0).crop.topLeft();
    EXPECT_EQ(qAlpha(stored.pixel(outside)), 0);
}

#endif // VIDEOEDITOR_HAVE_ORT
