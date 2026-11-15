#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_ORT) && VIDEOEDITOR_HAVE_ORT

#include <QColor>

#include "TemporalStabilizer.h"

using namespace VideoEditor;

namespace {

qreal temporalVariation(const QList<QImage> &frames, const QRect &hole)
{
    qreal total = 0.0;
    int count = 0;
    for (int i = 1; i < frames.size(); ++i) {
        for (int y = hole.top(); y <= hole.bottom(); ++y) {
            for (int x = hole.left(); x <= hole.right(); ++x) {
                const QColor a = frames[i - 1].pixelColor(x, y);
                const QColor b = frames[i].pixelColor(x, y);
                total += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green())
                       + qAbs(a.blue() - b.blue());
                count += 3;
            }
        }
    }
    return count == 0 ? 0.0 : total / count;
}

QImage holePatch(const QRect &hole, const QColor &inside)
{
    QImage image(64, 64, QImage::Format_ARGB32);
    image.fill(QColor(40, 40, 40, 0));
    for (int y = hole.top(); y <= hole.bottom(); ++y)
        for (int x = hole.left(); x <= hole.right(); ++x)
            image.setPixelColor(x, y, QColor(inside.red(), inside.green(),
                                             inside.blue(), 255));
    return image;
}

} // namespace

TEST(TemporalStabilizer, ReducesFrameToFrameVariationInHole)
{
    QList<QImage> patches;
    QList<QRect> crops;
    QList<QPoint> centroids;
    for (int i = 0; i < 5; ++i) {
        QImage image(64, 64, QImage::Format_ARGB32);
        image.fill(QColor(40, 40, 40, 255));
        const int offset = (i % 2 == 0) ? -12 : 12;
        for (int y = 20; y < 44; ++y)
            for (int x = 20; x < 44; ++x)
                image.setPixelColor(x, y, QColor(128 + offset, 128, 128, 255));
        patches.append(image);
        crops.append(QRect(0, 0, 64, 64));
        centroids.append(QPoint(32, 32));
    }
    const qreal before = temporalVariation(patches, QRect(20, 20, 24, 24));
    TemporalStabilizer::stabilize(patches, crops, centroids, 100);
    const qreal after = temporalVariation(patches, QRect(20, 20, 24, 24));
    EXPECT_LT(after, before * 0.8);
}

TEST(TemporalStabilizer, MadGateLeavesInconsistentNeighboursUntouched)
{
    const QRect hole(20, 20, 24, 24);
    QList<QImage> patches;
    QList<QImage> originals;
    QList<QRect> crops;
    QList<QPoint> centroids;
    for (int i = 0; i < 5; ++i) {
        // Alternating black/white samples: MAD is far above the 12/255 gate,
        // so the target must not be blended toward the median.
        QImage image = holePatch(hole, (i % 2 == 0) ? QColor(0, 0, 0)
                                                   : QColor(255, 255, 255));
        patches.append(image);
        originals.append(image.copy());
        crops.append(QRect(0, 0, 64, 64));
        centroids.append(QPoint(32, 32));
    }

    TemporalStabilizer::stabilize(patches, crops, centroids, 100);
    for (int i = 0; i < patches.size(); ++i)
        for (int y = hole.top(); y <= hole.bottom(); ++y)
            for (int x = hole.left(); x <= hole.right(); ++x)
                EXPECT_EQ(patches.at(i).pixel(x, y),
                          originals.at(i).pixel(x, y));
}

TEST(TemporalStabilizer, AlphaIsPreserved)
{
    const QRect hole(20, 20, 24, 24);
    QList<QImage> patches;
    QList<QRect> crops;
    QList<QPoint> centroids;
    for (int i = 0; i < 3; ++i) {
        patches.append(holePatch(hole, QColor(100 + i, 100, 100)));
        crops.append(QRect(0, 0, 64, 64));
        centroids.append(QPoint(32, 32));
    }
    TemporalStabilizer::stabilize(patches, crops, centroids, 50);
    EXPECT_EQ(qAlpha(patches.at(1).pixel(32, 32)), 255);
    EXPECT_EQ(qAlpha(patches.at(1).pixel(2, 2)), 0);
}

#endif // VIDEOEDITOR_HAVE_ORT
