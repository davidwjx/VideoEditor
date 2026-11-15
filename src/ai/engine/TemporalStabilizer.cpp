#include "TemporalStabilizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <QColor>

namespace VideoEditor {

namespace {

constexpr int kWindowRadius = 2;
constexpr double kMadGate = 12.0; // in 0..255 units

int medianOf(std::array<int, 2 * kWindowRadius + 1> values, int count)
{
    std::sort(values.begin(), values.begin() + count);
    // Lower median: with an even window this biases toward the older sample,
    // which is what damps a two-frame alternation instead of preserving it.
    return values[(count - 1) / 2];
}

} // namespace

void TemporalStabilizer::stabilize(QList<QImage> &patches,
                                   const QList<QRect> &crops,
                                   const QList<QPoint> &centroids, int strength)
{
    const int count = patches.size();
    if (count == 0 || crops.size() != count || centroids.size() != count)
        return;

    const double blend =
        std::clamp(double(strength) / 100.0 * 0.6, 0.0, 0.6);
    if (blend <= 0.0)
        return;

    QList<QImage> frames;
    frames.reserve(count);
    for (const QImage &patch : patches)
        frames.append(patch.convertToFormat(QImage::Format_ARGB32));

    for (int i = 0; i < count; ++i) {
        const QImage &target = frames.at(i);
        QImage stabilized = target;
        const QPoint targetTopLeft = crops.at(i).topLeft();
        const QPoint targetCentroid = centroids.at(i);

        const int first = std::max(0, i - kWindowRadius);
        const int last = std::min(count - 1, i + kWindowRadius);

        for (int y = 0; y < target.height(); ++y) {
            auto *outLine = reinterpret_cast<QRgb *>(stabilized.scanLine(y));
            const auto *targetLine =
                reinterpret_cast<const QRgb *>(target.constScanLine(y));
            for (int x = 0; x < target.width(); ++x) {
                const QRgb targetColor = targetLine[x];
                if (qAlpha(targetColor) == 0)
                    continue;

                const QPoint targetFull = targetTopLeft + QPoint(x, y);
                std::array<int, 2 * kWindowRadius + 1> reds{};
                std::array<int, 2 * kWindowRadius + 1> greens{};
                std::array<int, 2 * kWindowRadius + 1> blues{};
                int samples = 0;

                for (int j = first; j <= last; ++j) {
                    const QPoint delta = targetCentroid - centroids.at(j);
                    const QPoint local =
                        targetFull - delta - crops.at(j).topLeft();
                    if (local.x() < 0 || local.y() < 0
                        || local.x() >= frames.at(j).width()
                        || local.y() >= frames.at(j).height()) {
                        continue;
                    }
                    const QRgb color = frames.at(j).pixel(local);
                    if (qAlpha(color) == 0)
                        continue;
                    reds[samples] = qRed(color);
                    greens[samples] = qGreen(color);
                    blues[samples] = qBlue(color);
                    ++samples;
                }
                if (samples < 2)
                    continue;

                const int medianRed = medianOf(reds, samples);
                const int medianGreen = medianOf(greens, samples);
                const int medianBlue = medianOf(blues, samples);

                double mad = 0.0;
                for (int s = 0; s < samples; ++s) {
                    mad += (std::abs(reds[s] - medianRed)
                            + std::abs(greens[s] - medianGreen)
                            + std::abs(blues[s] - medianBlue))
                        / 3.0;
                }
                mad /= double(samples);
                if (mad > kMadGate)
                    continue;

                const auto mix = [blend](int original, int median) {
                    return std::clamp(
                        int(std::lround((1.0 - blend) * original
                                        + blend * median)),
                        0, 255);
                };
                outLine[x] = qRgba(mix(qRed(targetColor), medianRed),
                                   mix(qGreen(targetColor), medianGreen),
                                   mix(qBlue(targetColor), medianBlue),
                                   qAlpha(targetColor));
            }
        }
        patches[i] = stabilized;
    }
}

} // namespace VideoEditor
