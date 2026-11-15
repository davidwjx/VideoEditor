#pragma once

#include <QImage>
#include <QRect>
#include <QtGlobal>

namespace VideoEditor {

// Straight-alpha invariant for repair patches: pixels with alpha == 0 are
// fully transparent and their RGB is never composited (the preview/export use
// SRC_ALPHA blending). Leaving textured RGB under alpha == 0 defeats PNG
// compression and inflates the repair cache, so clear it to (0,0,0,0).
// Visible pixels are untouched: this never premultiplies or scales colour.
inline void clearRgbWhereTransparent(QImage &image)
{
    if (image.isNull())
        return;
    if (image.format() != QImage::Format_ARGB32)
        return; // callers normalize the format first
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                line[x] = 0;
        }
    }
}

// Tight bounding box of pixels with alpha > 0 (the only pixels the compositor
// uses). Returns an empty rect for a fully transparent image.
inline QRect alphaBounds(const QImage &image)
{
    if (image.isNull() || image.format() != QImage::Format_ARGB32)
        return QRect();
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line =
            reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            minX = qMin(minX, x);
            minY = qMin(minY, y);
            maxX = qMax(maxX, x);
            maxY = qMax(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY)
        return QRect();
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

} // namespace VideoEditor
