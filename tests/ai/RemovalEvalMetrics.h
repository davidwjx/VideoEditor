#pragma once

#include <cmath>

#include <QColor>
#include <QImage>
#include <QList>
#include <QtGlobal>

#include "ImageMetrics.h"
#include "MaskCodec.h"

namespace VideoEditor {

// Intersection-over-union of two binary masks. Two empty masks are identical
// (1.0); mismatched geometry is never silently scored as 0/1.
inline double maskIou(const BinaryMask &a, const BinaryMask &b)
{
    if (a.width != b.width || a.height != b.height || !a.isValid()
        || !b.isValid()) {
        return 0.0;
    }
    qint64 intersection = 0;
    qint64 unionPixels = 0;
    for (int i = 0; i < int(a.bits.size()); ++i) {
        const bool pa = a.bits[size_t(i)] != 0;
        const bool pb = b.bits[size_t(i)] != 0;
        if (pa && pb)
            ++intersection;
        if (pa || pb)
            ++unionPixels;
    }
    return unionPixels == 0 ? 1.0
                            : double(intersection) / double(unionPixels);
}

// PSNR over only the pixels selected by `mask`. A perfect match returns 100 dB
// instead of infinity so the value is printable and comparable.
inline double psnrInMask(const QImage &a, const QImage &b, const BinaryMask &mask)
{
    if (a.size() != b.size() || !mask.isValid() || mask.width != a.width()
        || mask.height != a.height()) {
        return 0.0;
    }
    double mse = 0.0;
    qint64 count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!mask.at(x, y))
                continue;
            const QColor ca = a.pixelColor(x, y);
            const QColor cb = b.pixelColor(x, y);
            const double dr = ca.red() - cb.red();
            const double dg = ca.green() - cb.green();
            const double db = ca.blue() - cb.blue();
            mse += dr * dr + dg * dg + db * db;
            count += 3;
        }
    }
    if (count == 0)
        return 100.0;
    return psnrFromMse(mse / double(count));
}

// Mean absolute per-channel difference between consecutive frames inside the
// mask. Used as a cheap flicker proxy for generated repair patches.
inline double temporalDeltaInMask(const QList<QImage> &frames,
                                  const BinaryMask &mask)
{
    if (frames.size() < 2)
        return 0.0;
    double total = 0.0;
    qint64 count = 0;
    for (int i = 1; i < frames.size(); ++i) {
        if (frames[i].size() != frames[i - 1].size())
            continue;
        for (int y = 0; y < mask.height; ++y) {
            for (int x = 0; x < mask.width; ++x) {
                if (!mask.at(x, y))
                    continue;
                const QColor a = frames[i - 1].pixelColor(x, y);
                const QColor b = frames[i].pixelColor(x, y);
                total += qAbs(a.red() - b.red())
                         + qAbs(a.green() - b.green())
                         + qAbs(a.blue() - b.blue());
                count += 3;
            }
        }
    }
    return count == 0 ? 0.0 : total / double(count);
}

} // namespace VideoEditor
