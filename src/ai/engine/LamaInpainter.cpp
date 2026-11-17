#include "LamaInpainter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "OnnxExecutor.h"

namespace VideoEditor {

namespace {

constexpr int kLamaSize = 512;
constexpr int kLamaOverlap = 64;

int reflectIndex(int index, int size)
{
    if (size <= 1)
        return 0;
    while (index < 0 || index >= size) {
        if (index < 0)
            index = -index - 1;
        else
            index = 2 * size - index - 1;
    }
    return index;
}

// Never upscale small crops beyond the model canvas but always fill it so the
// fixed 512 graph receives valid geometry.
int scaledSize(int source, int scaleNumerator, int scaleDenominator)
{
    if (source <= 0 || scaleDenominator <= 0)
        return 1;
    const int value = int(std::lround(double(source) * double(scaleNumerator)
                                      / double(scaleDenominator)));
    return std::clamp(value, 1, kLamaSize);
}

BinaryMask scaleMaskNearest(const BinaryMask &mask, int width, int height)
{
    BinaryMask scaled;
    scaled.width = width;
    scaled.height = height;
    scaled.bits.assign(size_t(width) * size_t(height), 0);
    for (int y = 0; y < height; ++y) {
        const int sourceY =
            std::min(mask.height - 1, y * mask.height / height);
        for (int x = 0; x < width; ++x) {
            const int sourceX =
                std::min(mask.width - 1, x * mask.width / width);
            if (mask.at(sourceX, sourceY))
                scaled.set(x, y, true);
        }
    }
    return scaled;
}

std::vector<int> tileStarts(int length, int tile)
{
    std::vector<int> starts;
    if (length <= tile) {
        starts.push_back(0);
        return starts;
    }
    const int step = tile - kLamaOverlap;
    for (int offset = 0;; offset += step) {
        if (offset + tile >= length) {
            starts.push_back(length - tile);
            break;
        }
        starts.push_back(offset);
    }
    return starts;
}

} // namespace

struct LamaInpainter::Impl {
    OnnxExecutor executor;
    QString inputName;
    QString outputName;
    QString error;
    QString provider;
    bool ready = false;

    std::optional<QImage> runCanvas(const QImage &canvasRgb,
                                    const BinaryMask &canvasMask,
                                    QString *error)
    {
        const int width = canvasRgb.width();
        const int height = canvasRgb.height();
        const size_t plane = size_t(width) * size_t(height);

        OnnxTensor tensor;
        tensor.type = OnnxTensor::DataType::Float32;
        tensor.shape = {1, 4, height, width};
        tensor.floatData.assign(size_t(4) * plane, 0.0f);

        const QImage rgb = canvasRgb.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const size_t index = size_t(y) * size_t(width) + size_t(x);
                const bool hole = canvasMask.isValid()
                    && canvasMask.at(x, y);
                tensor.floatData[index] = hole ? 0.0f : qRed(line[x]) / 255.0f;
                tensor.floatData[plane + index] =
                    hole ? 0.0f : qGreen(line[x]) / 255.0f;
                tensor.floatData[2 * plane + index] =
                    hole ? 0.0f : qBlue(line[x]) / 255.0f;
                tensor.floatData[3 * plane + index] =
                    hole ? 1.0f : 0.0f;
            }
        }

        QMap<QString, OnnxTensor> inputs;
        inputs.insert(inputName, tensor);
        QMap<QString, OnnxOutput> outputs;
        QString runError;
        if (!executor.run(QStringLiteral("lama"), inputs, {outputName},
                          &outputs, &runError)) {
            if (error)
                *error = QStringLiteral("LaMa 推理失败：%1").arg(runError);
            return std::nullopt;
        }
        const OnnxOutput output = outputs.value(outputName);
        if (output.type != OnnxOutput::DataType::Float32
            || output.shape.size() != 4 || output.shape.at(1) < 3
            || output.shape.at(2) <= 0 || output.shape.at(3) <= 0) {
            if (error)
                *error = QStringLiteral("LaMa 输出形状或类型不符合预期。");
            return std::nullopt;
        }
        const int outWidth = int(output.shape.at(3));
        const int outHeight = int(output.shape.at(2));
        const size_t outPlane = size_t(outWidth) * size_t(outHeight);
        if (output.floatData.size() < 3 * outPlane) {
            if (error)
                *error = QStringLiteral("LaMa 输出数据长度不足。");
            return std::nullopt;
        }

        QImage result(outWidth, outHeight, QImage::Format_ARGB32);
        const auto clampByte = [](float value) {
            return int(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        for (int y = 0; y < outHeight; ++y) {
            auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
            for (int x = 0; x < outWidth; ++x) {
                const size_t index = size_t(y) * size_t(outWidth) + size_t(x);
                line[x] = qRgba(clampByte(output.floatData[index]),
                                clampByte(output.floatData[outPlane + index]),
                                clampByte(
                                    output.floatData[2 * outPlane + index]),
                                255);
            }
        }
        return result;
    }

    std::optional<QImage> repairDirect(const QImage &rgb,
                                       const BinaryMask &mask,
                                       QString *error)
    {
        const int width = rgb.width();
        const int height = rgb.height();
        const int scale = std::min(kLamaSize / std::max(1, width),
                                   kLamaSize / std::max(1, height));
        int targetWidth = width;
        int targetHeight = height;
        if (scale >= 1) {
            // Integer upscale keeps the aspect ratio without inventing pixels.
            targetWidth = std::min(kLamaSize, width * scale);
            targetHeight = std::min(kLamaSize, height * scale);
        } else {
            targetWidth = scaledSize(width, kLamaSize,
                                     std::max(width, height));
            targetHeight = scaledSize(height, kLamaSize,
                                      std::max(width, height));
        }
        QImage resized = rgb;
        if (targetWidth != width || targetHeight != height) {
            resized = rgb.scaled(targetWidth, targetHeight,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        const BinaryMask resizedMask = (targetWidth == width
                                        && targetHeight == height)
            ? mask
            : scaleMaskNearest(mask, targetWidth, targetHeight);

        QImage canvas(kLamaSize, kLamaSize, QImage::Format_RGB32);
        BinaryMask canvasMask;
        canvasMask.width = kLamaSize;
        canvasMask.height = kLamaSize;
        canvasMask.bits.assign(size_t(kLamaSize) * size_t(kLamaSize), 0);
        for (int y = 0; y < kLamaSize; ++y) {
            auto *line = reinterpret_cast<QRgb *>(canvas.scanLine(y));
            const int sourceY = reflectIndex(y, targetHeight);
            for (int x = 0; x < kLamaSize; ++x) {
                const int sourceX = reflectIndex(x, targetWidth);
                line[x] = resized.pixel(sourceX, sourceY);
                if (x < targetWidth && y < targetHeight
                    && resizedMask.at(x, y)) {
                    canvasMask.set(x, y, true);
                }
            }
        }

        auto repaired = runCanvas(canvas, canvasMask, error);
        if (!repaired)
            return std::nullopt;
        QImage cropped = repaired->copy(0, 0, targetWidth, targetHeight);
        if (targetWidth != width || targetHeight != height) {
            cropped = cropped.scaled(width, height, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
        }
        return cropped.convertToFormat(QImage::Format_ARGB32);
    }

    std::optional<QImage> repairTiled(const QImage &rgb,
                                      const BinaryMask &mask,
                                      QString *error)
    {
        const int width = rgb.width();
        const int height = rgb.height();
        const std::vector<int> xs = tileStarts(width, kLamaSize);
        const std::vector<int> ys = tileStarts(height, kLamaSize);

        std::vector<double> accumRed(size_t(width) * size_t(height), 0.0);
        std::vector<double> accumGreen(size_t(width) * size_t(height), 0.0);
        std::vector<double> accumBlue(size_t(width) * size_t(height), 0.0);
        std::vector<double> accumWeight(size_t(width) * size_t(height), 0.0);

        QImage canvas(kLamaSize, kLamaSize, QImage::Format_RGB32);
        BinaryMask canvasMask;
        canvasMask.width = kLamaSize;
        canvasMask.height = kLamaSize;
        canvasMask.bits.assign(size_t(kLamaSize) * size_t(kLamaSize), 0);

        for (int tileY : ys) {
            for (int tileX : xs) {
                for (int y = 0; y < kLamaSize; ++y) {
                    auto *line =
                        reinterpret_cast<QRgb *>(canvas.scanLine(y));
                    const int sourceY = reflectIndex(tileY + y, height);
                    for (int x = 0; x < kLamaSize; ++x) {
                        const int sourceX = reflectIndex(tileX + x, width);
                        line[x] = rgb.pixel(sourceX, sourceY);
                        canvasMask.set(x, y, mask.at(sourceX, sourceY));
                    }
                }

                auto repaired = runCanvas(canvas, canvasMask, error);
                if (!repaired)
                    return std::nullopt;

                for (int y = 0; y < kLamaSize; ++y) {
                    const int frameY = tileY + y;
                    if (frameY >= height)
                        continue;
                    const auto *line =
                        reinterpret_cast<const QRgb *>(repaired->constScanLine(y));
                    // Linear feather so overlapping tiles blend smoothly.
                    const double wy = double(std::min(y + 1, kLamaSize - y));
                    for (int x = 0; x < kLamaSize; ++x) {
                        const int frameX = tileX + x;
                        if (frameX >= width)
                            continue;
                        const double wx =
                            double(std::min(x + 1, kLamaSize - x));
                        const double weight = wx * wy;
                        const size_t index =
                            size_t(frameY) * size_t(width) + size_t(frameX);
                        accumRed[index] += qRed(line[x]) * weight;
                        accumGreen[index] += qGreen(line[x]) * weight;
                        accumBlue[index] += qBlue(line[x]) * weight;
                        accumWeight[index] += weight;
                    }
                }
            }
        }

        QImage result(width, height, QImage::Format_ARGB32);
        for (int y = 0; y < height; ++y) {
            auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const size_t index = size_t(y) * size_t(width) + size_t(x);
                const double weight = accumWeight[index];
                if (weight <= 0.0) {
                    line[x] = rgb.pixel(x, y);
                    continue;
                }
                line[x] = qRgba(
                    int(std::clamp(accumRed[index] / weight, 0.0, 255.0)
                        + 0.5),
                    int(std::clamp(accumGreen[index] / weight, 0.0, 255.0)
                        + 0.5),
                    int(std::clamp(accumBlue[index] / weight, 0.0, 255.0)
                        + 0.5),
                    255);
            }
        }
        return result;
    }
};

LamaInpainter::LamaInpainter(const QString &modelPath, int deviceIndex,
                             bool allowDml)
    : m_impl(std::make_unique<Impl>())
{
    if (modelPath.isEmpty()) {
        m_impl->error = QStringLiteral("LaMa 模型路径为空。");
        return;
    }

    EpConfig config;
    config.deviceIndex = deviceIndex;
    // DirectML is faster on a dGPU; the iGPU/CPU fallback keeps the old path.
    config.allowDml = allowDml;
    config.allowCpu = true;
    QString error;
    if (!m_impl->executor.createSession(QStringLiteral("lama"), modelPath,
                                        config, &error)) {
        m_impl->error = QStringLiteral("无法创建 LaMa 会话：%1").arg(error);
        return;
    }
    if (m_impl->executor.inputCount(QStringLiteral("lama")) < 1
        || m_impl->executor.outputCount(QStringLiteral("lama")) < 1) {
        m_impl->error = QStringLiteral("LaMa 模型输入输出不符合预期。");
        return;
    }
    m_impl->inputName = m_impl->executor.inputName(QStringLiteral("lama"), 0);
    m_impl->outputName = m_impl->executor.outputName(QStringLiteral("lama"), 0);
    m_impl->provider = m_impl->executor.provider();
    m_impl->ready = true;
}

LamaInpainter::~LamaInpainter() = default;

bool LamaInpainter::isReady() const { return m_impl->ready; }

QString LamaInpainter::lastError() const { return m_impl->error; }

QString LamaInpainter::provider() const { return m_impl->provider; }

std::optional<PatchResult> LamaInpainter::repair(const QImage &crop,
                                                 const BinaryMask &maskCrop,
                                                 QString *error)
{
    const auto fail = [error](const QString &message)
        -> std::optional<PatchResult> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    if (!m_impl->ready)
        return fail(m_impl->error);
    if (crop.isNull())
        return fail(QStringLiteral("LaMa 输入裁剪为空。"));
    if (!maskCrop.isValid() || maskCrop.width != crop.width()
        || maskCrop.height != crop.height()) {
        return fail(QStringLiteral("LaMa 遮罩尺寸与裁剪不一致。"));
    }

    const QImage rgb = crop.convertToFormat(QImage::Format_RGB32);
    std::optional<QImage> repaired;
    if (rgb.width() <= kLamaSize && rgb.height() <= kLamaSize)
        repaired = m_impl->repairDirect(rgb, maskCrop, error);
    else
        repaired = m_impl->repairTiled(rgb, maskCrop, error);
    if (!repaired)
        return std::nullopt;

    if (error)
        error->clear();
    return PatchResult{*repaired, QRect()};
}

} // namespace VideoEditor
