#pragma once

#include <QImage>
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

} // namespace VideoEditor
