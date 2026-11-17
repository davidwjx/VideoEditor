#include "InpaintPipeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <QColor>

#include "ImageAlpha.h"
#include "LamaInpainter.h"
#include "MaskEditor.h"
#include "MiGanInpainter.h"

namespace VideoEditor {

namespace {

constexpr int kMinMargin = 64;
constexpr int kColorRingPx = 8;
constexpr int kMinRingPixels = 32;
constexpr float kChamferInfinity = 1.0e9f;

bool maskHasPixels(const BinaryMask &mask)
{
    return std::any_of(mask.bits.begin(), mask.bits.end(),
                       [](uint8_t value) { return value != 0; });
}

// 3-4 chamfer distance transform. Sources are zero; every other pixel gets the
// distance to the nearest source in units of 3 per pixel (4 for diagonals).
std::vector<float> chamferDistance(const BinaryMask &mask, bool sourcesInside)
{
    const int width = mask.width;
    const int height = mask.height;
    std::vector<float> distance(size_t(width) * size_t(height),
                                kChamferInfinity);
    if (width <= 0 || height <= 0)
        return distance;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = mask.at(x, y);
            if ((sourcesInside && inside) || (!sourcesInside && !inside))
                distance[size_t(y) * size_t(width) + size_t(x)] = 0.0f;
        }
    }

    const auto at = [&distance, width](int x, int y) -> float & {
        return distance[size_t(y) * size_t(width) + size_t(x)];
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float best = at(x, y);
            if (x > 0)
                best = std::min(best, at(x - 1, y) + 3.0f);
            if (y > 0)
                best = std::min(best, at(x, y - 1) + 3.0f);
            if (x > 0 && y > 0)
                best = std::min(best, at(x - 1, y - 1) + 4.0f);
            if (x + 1 < width && y > 0)
                best = std::min(best, at(x + 1, y - 1) + 4.0f);
            at(x, y) = best;
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            float best = at(x, y);
            if (x + 1 < width)
                best = std::min(best, at(x + 1, y) + 3.0f);
            if (y + 1 < height)
                best = std::min(best, at(x, y + 1) + 3.0f);
            if (x + 1 < width && y + 1 < height)
                best = std::min(best, at(x + 1, y + 1) + 4.0f);
            if (x > 0 && y + 1 < height)
                best = std::min(best, at(x - 1, y + 1) + 4.0f);
            at(x, y) = best;
        }
    }
    return distance;
}

QImage transparentCrop(QSize size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    return image;
}

} // namespace

InpaintPipeline::InpaintPipeline() = default;

InpaintPipeline::~InpaintPipeline() = default;

void InpaintPipeline::setInpainterForTest(std::unique_ptr<Inpainter> inpainter)
{
    setInpainter(std::move(inpainter));
}

void InpaintPipeline::setInpainter(std::unique_ptr<Inpainter> inpainter)
{
    m_injected = std::move(inpainter);
    m_owned.reset();
    m_hasOwnedMode = false;
}

void InpaintPipeline::setConfig(const InpaintConfig &config)
{
    m_config = config;
}

QString InpaintPipeline::provider() const
{
    if (m_injected)
        return m_injected->provider();
    if (m_owned)
        return m_owned->provider();
    return QString();
}

Inpainter *InpaintPipeline::activeInpainter(RepairMode mode, QString *error)
{
    if (m_injected)
        return m_injected.get();

    if (m_owned && m_hasOwnedMode && m_ownedMode == mode)
        return m_owned.get();

    m_owned.reset();
    m_hasOwnedMode = false;
    if (mode == RepairMode::Fast) {
        if (m_config.miganPath.isEmpty()) {
            if (error)
                *error = QStringLiteral("未配置 MI-GAN 模型路径。");
            return nullptr;
        }
        auto inpainter = std::make_unique<MiGanInpainter>(
            m_config.miganPath, m_config.deviceIndex, m_config.allowDml,
            m_config.allowCpu);
        if (!inpainter->isReady()) {
            if (error)
                *error = inpainter->lastError();
            return nullptr;
        }
        m_owned = std::move(inpainter);
    } else {
        if (m_config.lamaPath.isEmpty()) {
            if (error)
                *error = QStringLiteral("未配置 LaMa 模型路径。");
            return nullptr;
        }
        auto inpainter = std::make_unique<LamaInpainter>(
            m_config.lamaPath, m_config.deviceIndex, m_config.allowDml);
        if (!inpainter->isReady()) {
            if (error)
                *error = inpainter->lastError();
            return nullptr;
        }
        m_owned = std::move(inpainter);
    }
    m_ownedMode = mode;
    m_hasOwnedMode = true;
    return m_owned.get();
}

int InpaintPipeline::chooseTileSize(int cropW, int cropH,
                                    qint64 vramBudgetBytes)
{
    Q_UNUSED(cropW)
    Q_UNUSED(cropH)
    // Approximate working sets measured for the fixed models; the tile size
    // only affects LaMa tiling and MI-GAN's internal canvas.
    struct Candidate {
        int tile;
        qint64 requiredBytes;
    };
    static constexpr qint64 kMiB = 1024 * 1024;
    static const Candidate candidates[] = {
        {512, 1536 * kMiB},
        {384, 900 * kMiB},
        {256, 400 * kMiB},
        {128, 120 * kMiB},
    };
    for (const Candidate &candidate : candidates) {
        if (vramBudgetBytes <= 0 || vramBudgetBytes >= candidate.requiredBytes)
            return candidate.tile;
    }
    return 128;
}

QImage InpaintPipeline::featherAlpha(const BinaryMask &maskCrop, int featherPx)
{
    if (!maskCrop.isValid())
        return QImage();

    const int feather = std::clamp(featherPx, 0, 64);
    const std::vector<float> distance =
        chamferDistance(maskCrop, /*sourcesInside=*/false);
    QImage alpha(maskCrop.width, maskCrop.height, QImage::Format_ARGB32);
    for (int y = 0; y < maskCrop.height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(alpha.scanLine(y));
        for (int x = 0; x < maskCrop.width; ++x) {
            const size_t index =
                size_t(y) * size_t(maskCrop.width) + size_t(x);
            if (!maskCrop.at(x, y)) {
                line[x] = qRgba(255, 255, 255, 0);
                continue;
            }
            int value = 255;
            if (feather > 0) {
                const double pixels = double(distance[index]) / 3.0;
                const double scaled = 255.0 * pixels / double(feather);
                value = int(std::clamp(scaled, 0.0, 255.0) + 0.5);
            }
            line[x] = qRgba(255, 255, 255, value);
        }
    }
    return alpha;
}

bool InpaintPipeline::colorMatch(const QImage &sourceCrop, QImage *patch,
                                 const BinaryMask &maskCrop)
{
    if (!patch || patch->isNull() || sourceCrop.isNull()
        || patch->size() != sourceCrop.size() || !maskCrop.isValid()
        || maskCrop.width != sourceCrop.width()
        || maskCrop.height != sourceCrop.height()) {
        return false;
    }

    const QImage source = sourceCrop.convertToFormat(QImage::Format_RGB32);
    QImage destination = patch->convertToFormat(QImage::Format_ARGB32);
    const std::vector<float> distance =
        chamferDistance(maskCrop, /*sourcesInside=*/true);

    double sourceRed = 0.0, sourceGreen = 0.0, sourceBlue = 0.0;
    double patchRed = 0.0, patchGreen = 0.0, patchBlue = 0.0;
    int count = 0;
    for (int y = 0; y < maskCrop.height; ++y) {
        const QRgb *sourceLine =
            reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *patchLine = reinterpret_cast<QRgb *>(destination.scanLine(y));
        for (int x = 0; x < maskCrop.width; ++x) {
            if (maskCrop.at(x, y))
                continue;
            const size_t index =
                size_t(y) * size_t(maskCrop.width) + size_t(x);
            if (double(distance[index]) / 3.0 > double(kColorRingPx))
                continue;
            sourceRed += qRed(sourceLine[x]);
            sourceGreen += qGreen(sourceLine[x]);
            sourceBlue += qBlue(sourceLine[x]);
            patchRed += qRed(patchLine[x]);
            patchGreen += qGreen(patchLine[x]);
            patchBlue += qBlue(patchLine[x]);
            ++count;
        }
    }
    if (count < kMinRingPixels)
        return false;

    const double meanSourceRed = sourceRed / count;
    const double meanSourceGreen = sourceGreen / count;
    const double meanSourceBlue = sourceBlue / count;
    const double meanPatchRed = patchRed / count;
    const double meanPatchGreen = patchGreen / count;
    const double meanPatchBlue = patchBlue / count;
    const int deltaRed = int(std::lround(meanSourceRed - meanPatchRed));
    const int deltaGreen = int(std::lround(meanSourceGreen - meanPatchGreen));
    const int deltaBlue = int(std::lround(meanSourceBlue - meanPatchBlue));

    for (int y = 0; y < destination.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(destination.scanLine(y));
        for (int x = 0; x < destination.width(); ++x) {
            line[x] = qRgba(
                std::clamp(qRed(line[x]) + deltaRed, 0, 255),
                std::clamp(qGreen(line[x]) + deltaGreen, 0, 255),
                std::clamp(qBlue(line[x]) + deltaBlue, 0, 255),
                qAlpha(line[x]));
        }
    }
    *patch = destination;
    return true;
}

std::optional<QList<PatchResult>> InpaintPipeline::repairChunk(
    const QList<QImage> &frames, const QList<BinaryMask> &masks,
    const RemovalParameters &params, int featherPx,
    const std::function<bool(int, const QString &)> &progress, QString *error)
{
    m_cancelled = false;
    const auto fail = [error](const QString &message)
        -> std::optional<QList<PatchResult>> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    if (frames.size() != masks.size())
        return fail(QStringLiteral("修复帧与遮罩数量不一致。"));
    if (frames.isEmpty())
        return QList<PatchResult>{};

    const QSize frameSize = frames.first().size();
    if (frameSize.isEmpty())
        return fail(QStringLiteral("修复帧尺寸无效。"));

    int minX = frameSize.width();
    int minY = frameSize.height();
    int maxX = -1;
    int maxY = -1;
    bool anyMask = false;
    for (const BinaryMask &mask : masks) {
        if (!mask.isValid() || mask.width != frameSize.width()
            || mask.height != frameSize.height()) {
            return fail(QStringLiteral("修复遮罩尺寸与帧不一致。"));
        }
        if (!maskHasPixels(mask))
            continue;
        anyMask = true;
        for (int y = 0; y < mask.height; ++y) {
            for (int x = 0; x < mask.width; ++x) {
                if (!mask.at(x, y))
                    continue;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (!anyMask)
        return QList<PatchResult>{};

    const QRect bounds(minX, minY, maxX - minX + 1, maxY - minY + 1);
    const int margin =
        std::max({kMinMargin, bounds.width() / 4, bounds.height() / 4});
    QRect crop = bounds.adjusted(-margin, -margin, margin, margin);
    crop = crop.intersected(QRect(0, 0, frameSize.width(), frameSize.height()));
    if (crop.isEmpty())
        return fail(QStringLiteral("修复裁剪区域为空。"));

    // Round up to the MI-GAN-compatible multiple of 8, then clamp so the crop
    // never leaves the frame.
    const int x = crop.x();
    const int y = crop.y();
    int width = (crop.width() + 7) / 8 * 8;
    int height = (crop.height() + 7) / 8 * 8;
    width = std::min(width, frameSize.width() - x);
    height = std::min(height, frameSize.height() - y);
    if (width <= 0 || height <= 0)
        return fail(QStringLiteral("修复裁剪区域无效。"));
    crop = QRect(x, y, width, height);

    Inpainter *inpainter = activeInpainter(params.mode, error);
    if (!inpainter)
        return std::nullopt;

    QList<PatchResult> patches;
    patches.reserve(frames.size());
    for (int index = 0; index < frames.size(); ++index) {
        if (progress && !progress(index, QString())) {
            m_cancelled = true;
            return std::nullopt;
        }

        BinaryMask maskCrop;
        maskCrop.width = crop.width();
        maskCrop.height = crop.height();
        maskCrop.bits.assign(size_t(crop.width()) * size_t(crop.height()), 0);
        bool frameHasHole = false;
        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                if (masks.at(index).at(crop.x() + cx, crop.y() + cy)) {
                    maskCrop.set(cx, cy, true);
                    frameHasHole = true;
                }
            }
        }

        if (!frameHasHole) {
            // The frame has nothing to remove: keep the chunk geometry but
            // make the patch fully transparent so the compositor is a no-op.
            patches.append(PatchResult{transparentCrop(crop.size()), crop});
            continue;
        }

        const QImage cropImage = frames.at(index)
                                     .copy(crop)
                                     .convertToFormat(QImage::Format_ARGB32);
        QString repairError;
        const auto repaired = inpainter->repair(cropImage, maskCrop,
                                                 &repairError);
        if (!repaired) {
            return fail(repairError.isEmpty()
                            ? QStringLiteral("修复推理失败。")
                            : repairError);
        }

        QImage rgba = repaired->rgba.convertToFormat(QImage::Format_ARGB32);
        if (rgba.size() != crop.size())
            return fail(QStringLiteral("修复结果尺寸与裁剪区域不一致。"));

        const QImage alpha = featherAlpha(maskCrop, featherPx);
        for (int py = 0; py < rgba.height(); ++py) {
            auto *line = reinterpret_cast<QRgb *>(rgba.scanLine(py));
            const auto *alphaLine =
                reinterpret_cast<const QRgb *>(alpha.constScanLine(py));
            for (int px = 0; px < rgba.width(); ++px) {
                line[px] = qRgba(qRed(line[px]), qGreen(line[px]),
                                 qBlue(line[px]), qAlpha(alphaLine[px]));
            }
        }

        colorMatch(cropImage, &rgba, maskCrop);
        // Keep straight alpha (the compositor uses SRC_ALPHA) but drop RGB
        // under alpha == 0: it is never visible and only bloats the cache.
        clearRgbWhereTransparent(rgba);
        patches.append(PatchResult{rgba, crop});
    }

    if (error)
        error->clear();
    return patches;
}

} // namespace VideoEditor
