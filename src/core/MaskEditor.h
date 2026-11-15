#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>

#include "MaskCodec.h" // BinaryMask

namespace VideoEditor {

class MaskEditor {
public:
    static void paintStroke(BinaryMask &mask, QPoint from, QPoint to,
                            int radiusPx, bool erase);
    static void paintBox(BinaryMask &mask, QRect rect, bool erase);
    static QRect contentBounds(const BinaryMask &mask);
    static QImage toOverlay(const BinaryMask &mask, QRgb inside = 0x80FF3B30);
};

} // namespace VideoEditor
