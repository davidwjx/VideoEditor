#include "MaskEditor.h"

#include <QtGlobal>
#include <algorithm>

namespace VideoEditor {

namespace {

void stampCircle(BinaryMask &mask, int cx, int cy, int radius, bool erase)
{
    // Clamp absurd radii so the bounding-box arithmetic below cannot overflow.
    const int r = qBound(1, radius, qMax(mask.width, mask.height));
    if (cx + r < 0 || cy + r < 0 || cx - r >= mask.width || cy - r >= mask.height)
        return;

    const int r2 = r * r;
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r2)
                mask.set(x, y, !erase);
        }
    }
}

} // namespace

void MaskEditor::paintStroke(BinaryMask &mask, QPoint from, QPoint to,
                             int radiusPx, bool erase)
{
    if (!mask.isValid())
        return;

    const int dx = to.x() - from.x();
    const int dy = to.y() - from.y();
    const int steps = qMax(qAbs(dx), qAbs(dy));
    if (steps == 0) {
        stampCircle(mask, from.x(), from.y(), radiusPx, erase);
        return;
    }

    for (int i = 0; i <= steps; ++i) {
        const int x = int(from.x() + (qint64(dx) * i) / steps);
        const int y = int(from.y() + (qint64(dy) * i) / steps);
        stampCircle(mask, x, y, radiusPx, erase);
    }
}

void MaskEditor::paintBox(BinaryMask &mask, QRect rect, bool erase)
{
    if (!mask.isValid())
        return;

    const QRect normalized = rect.normalized();
    if (normalized.isEmpty())
        return;

    const int left = qMax(0, normalized.left());
    const int top = qMax(0, normalized.top());
    const int right = qMin(mask.width - 1, normalized.right());
    const int bottom = qMin(mask.height - 1, normalized.bottom());
    if (left > right || top > bottom)
        return;

    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x)
            mask.set(x, y, !erase);
}

QRect MaskEditor::contentBounds(const BinaryMask &mask)
{
    if (!mask.isValid())
        return QRect();

    int minX = mask.width;
    int minY = mask.height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (!mask.at(x, y))
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

QImage MaskEditor::toOverlay(const BinaryMask &mask, QRgb inside)
{
    if (!mask.isValid())
        return QImage();

    QImage image(mask.width, mask.height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < mask.height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < mask.width; ++x) {
            if (mask.at(x, y))
                line[x] = inside;
        }
    }
    return image;
}

} // namespace VideoEditor
