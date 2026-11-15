#include "MiGanInpainter.h"

#include <algorithm>
#include <cstdint>

#include <QColor>

#include "OnnxExecutor.h"

namespace VideoEditor {

namespace {

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

int roundUp8(int value)
{
    return (value + 7) / 8 * 8;
}

} // namespace

struct MiGanInpainter::Impl {
    OnnxExecutor executor;
    QString inputImageName;
    QString inputMaskName;
    QString outputName;
    QString error;
    QString provider;
    bool ready = false;
};

MiGanInpainter::MiGanInpainter(const QString &modelPath, int deviceIndex,
                               bool allowDml, bool allowCpu)
    : m_impl(std::make_unique<Impl>())
{
    if (modelPath.isEmpty()) {
        m_impl->error = QStringLiteral("MI-GAN 模型路径为空。");
        return;
    }

    EpConfig config;
    config.deviceIndex = deviceIndex;
    config.allowDml = allowDml;
    config.allowCpu = allowCpu;
    QString error;
    if (!m_impl->executor.createSession(QStringLiteral("migan"), modelPath,
                                        config, &error)) {
        m_impl->error = QStringLiteral("无法创建 MI-GAN 会话：%1").arg(error);
        return;
    }
    if (m_impl->executor.inputCount(QStringLiteral("migan")) < 2
        || m_impl->executor.outputCount(QStringLiteral("migan")) < 1) {
        m_impl->error = QStringLiteral("MI-GAN 模型输入输出不符合预期。");
        return;
    }
    m_impl->inputImageName =
        m_impl->executor.inputName(QStringLiteral("migan"), 0);
    m_impl->inputMaskName =
        m_impl->executor.inputName(QStringLiteral("migan"), 1);
    m_impl->outputName =
        m_impl->executor.outputName(QStringLiteral("migan"), 0);
    m_impl->provider = m_impl->executor.provider();
    m_impl->ready = true;
}

MiGanInpainter::~MiGanInpainter() = default;

bool MiGanInpainter::isReady() const { return m_impl->ready; }

QString MiGanInpainter::lastError() const { return m_impl->error; }

QString MiGanInpainter::provider() const { return m_impl->provider; }

bool MiGanInpainter::usingCpuFallback() const
{
    return m_impl->executor.usingCpuFallback();
}

QString MiGanInpainter::cpuFallbackReason() const
{
    return m_impl->executor.cpuFallbackReason();
}

std::optional<PatchResult> MiGanInpainter::repair(const QImage &crop,
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
        return fail(QStringLiteral("MI-GAN 输入裁剪为空。"));
    if (!maskCrop.isValid() || maskCrop.width != crop.width()
        || maskCrop.height != crop.height()) {
        return fail(QStringLiteral("MI-GAN 遮罩尺寸与裁剪不一致。"));
    }

    const QImage rgb = crop.convertToFormat(QImage::Format_RGB32);
    const int width = rgb.width();
    const int height = rgb.height();
    const int paddedWidth = roundUp8(width);
    const int paddedHeight = roundUp8(height);

    OnnxTensor imageTensor;
    imageTensor.type = OnnxTensor::DataType::UInt8;
    imageTensor.shape = {1, 3, paddedHeight, paddedWidth};
    imageTensor.uint8Data.resize(size_t(3) * size_t(paddedWidth)
                                 * size_t(paddedHeight));

    OnnxTensor maskTensor;
    maskTensor.type = OnnxTensor::DataType::UInt8;
    maskTensor.shape = {1, 1, paddedHeight, paddedWidth};
    maskTensor.uint8Data.resize(size_t(paddedWidth) * size_t(paddedHeight));

    const size_t plane = size_t(paddedWidth) * size_t(paddedHeight);
    for (int y = 0; y < paddedHeight; ++y) {
        const int sourceY = reflectIndex(y, height);
        for (int x = 0; x < paddedWidth; ++x) {
            const int sourceX = reflectIndex(x, width);
            const QRgb color = rgb.pixel(sourceX, sourceY);
            const size_t index = size_t(y) * size_t(paddedWidth) + size_t(x);
            imageTensor.uint8Data[index] = uint8_t(qRed(color));
            imageTensor.uint8Data[plane + index] = uint8_t(qGreen(color));
            imageTensor.uint8Data[2 * plane + index] = uint8_t(qBlue(color));
            // Padded pixels are declared known so no artificial hole is
            // introduced outside the real crop.
            const bool outside = x >= width || y >= height;
            const bool hole = !outside && maskCrop.at(sourceX, sourceY);
            maskTensor.uint8Data[index] = hole ? uint8_t(0) : uint8_t(255);
        }
    }

    QMap<QString, OnnxTensor> inputs;
    inputs.insert(m_impl->inputImageName, imageTensor);
    inputs.insert(m_impl->inputMaskName, maskTensor);
    QMap<QString, OnnxOutput> outputs;
    QString runError;
    if (!m_impl->executor.run(QStringLiteral("migan"), inputs,
                              {m_impl->outputName}, &outputs, &runError)) {
        return fail(QStringLiteral("MI-GAN 推理失败：%1").arg(runError));
    }
    const OnnxOutput output = outputs.value(m_impl->outputName);
    if (output.type != OnnxOutput::DataType::UInt8 || output.shape.size() != 4
        || output.shape.at(1) < 3
        || output.shape.at(2) < paddedHeight
        || output.shape.at(3) < paddedWidth) {
        return fail(QStringLiteral("MI-GAN 输出形状或类型不符合预期。"));
    }
    const int outWidth = int(output.shape.at(3));
    const int outHeight = int(output.shape.at(2));
    const size_t outPlane = size_t(outWidth) * size_t(outHeight);
    if (output.uint8Data.size() < 3 * outPlane)
        return fail(QStringLiteral("MI-GAN 输出数据长度不足。"));

    QImage result(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t index = size_t(y) * size_t(outWidth) + size_t(x);
            line[x] = qRgba(output.uint8Data[index],
                            output.uint8Data[outPlane + index],
                            output.uint8Data[2 * outPlane + index], 255);
        }
    }

    if (error)
        error->clear();
    return PatchResult{result, QRect()};
}

} // namespace VideoEditor
