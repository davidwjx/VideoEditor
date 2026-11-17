#include "Sam2Tracker.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QJsonValue>

#include "OnnxExecutor.h"

namespace VideoEditor {

namespace {

constexpr const char *kShapeMismatch =
    "跟踪模型输出形状不符合预期。";

bool shapeMatches(const std::vector<int64_t> &actual,
                  std::initializer_list<int64_t> expected)
{
    if (actual.size() != expected.size())
        return false;
    size_t index = 0;
    for (int64_t value : expected) {
        if (value >= 0 && actual[index] != value)
            return false;
        ++index;
    }
    return true;
}

int64_t product(const std::vector<int64_t> &shape)
{
    int64_t total = 1;
    for (int64_t value : shape) {
        if (value <= 0)
            return 0;
        total *= value;
    }
    return total;
}

} // namespace

struct Sam2Tracker::Impl {
    OnnxExecutor executor;
    TrackingConfig config;
    QString imageInput = QStringLiteral("image");
    QString imageEmbedding = QStringLiteral("vision_features");
    QString imagePosEmbedding = QStringLiteral("vision_pos_enc");
    QString maskLogits = QStringLiteral("masks");
    QString iouScore = QStringLiteral("iou_pred");
    QString objectScore;
    bool initialized = false;

    struct Preprocessed {
        std::vector<float> data;
        int sourceWidth = 0;
        int sourceHeight = 0;
        int newWidth = 0;
        int newHeight = 0;
        int padX = 0;
        int padY = 0;
        int inputSize = 0;
    };

    QString resolveName(const QString &part, bool input,
                        const QString &preferred,
                        std::initializer_list<int64_t> expected,
                        int fallbackIndex) const
    {
        const int count =
            input ? executor.inputCount(part) : executor.outputCount(part);
        if (count <= 0)
            return {};
        if (!preferred.isEmpty()) {
            for (int i = 0; i < count; ++i) {
                const QString name = input ? executor.inputName(part, i)
                                           : executor.outputName(part, i);
                if (name == preferred)
                    return name;
            }
        }
        for (int i = 0; i < count; ++i) {
            std::vector<int64_t> shape;
            const bool ok = input ? executor.inputShape(part, i, &shape)
                                  : executor.outputShape(part, i, &shape);
            if (ok && shapeMatches(shape, expected)) {
                return input ? executor.inputName(part, i)
                             : executor.outputName(part, i);
            }
        }
        if (fallbackIndex >= 0 && fallbackIndex < count)
            return input ? executor.inputName(part, fallbackIndex)
                         : executor.outputName(part, fallbackIndex);
        return {};
    }

    static Preprocessed preprocess(const QImage &image, int inputSize,
                                   QString *error)
    {
        Preprocessed out;
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            if (error)
                *error = QStringLiteral("跟踪输入图像为空。");
            return out;
        }
        const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull()) {
            if (error)
                *error = QStringLiteral("无法转换跟踪输入图像。");
            return out;
        }
        out.sourceWidth = rgb.width();
        out.sourceHeight = rgb.height();
        out.inputSize = inputSize;
        const double scale =
            double(inputSize) / double(std::max(rgb.width(), rgb.height()));
        out.newWidth = std::max(1, int(std::lround(rgb.width() * scale)));
        out.newHeight = std::max(1, int(std::lround(rgb.height() * scale)));
        out.padX = (inputSize - out.newWidth) / 2;
        out.padY = (inputSize - out.newHeight) / 2;

        const QImage scaled =
            rgb.scaled(out.newWidth, out.newHeight, Qt::IgnoreAspectRatio,
                       Qt::SmoothTransformation);

        const float mean[3] = {0.485f, 0.456f, 0.406f};
        const float stddev[3] = {0.229f, 0.224f, 0.225f};
        out.data.assign(size_t(3) * size_t(inputSize) * size_t(inputSize),
                        0.0f);

        for (int y = 0; y < out.newHeight; ++y) {
            const uchar *line = scaled.constScanLine(y);
            const int canvasY = out.padY + y;
            for (int x = 0; x < out.newWidth; ++x) {
                const int canvasX = out.padX + x;
                const float r = line[x * 3 + 0] / 255.0f;
                const float g = line[x * 3 + 1] / 255.0f;
                const float b = line[x * 3 + 2] / 255.0f;
                const float values[3] = {r, g, b};
                for (int c = 0; c < 3; ++c) {
                    const size_t index =
                        (size_t(c) * size_t(inputSize) + size_t(canvasY))
                            * size_t(inputSize)
                        + size_t(canvasX);
                    out.data[index] = (values[c] - mean[c]) / stddev[c];
                }
            }
        }
        return out;
    }

    static float toInputX(const Preprocessed &pre, double normalizedX)
    {
        const double value = pre.padX + normalizedX * pre.newWidth;
        return float(std::clamp(value, 0.0, double(pre.inputSize - 1)));
    }

    static float toInputY(const Preprocessed &pre, double normalizedY)
    {
        const double value = pre.padY + normalizedY * pre.newHeight;
        return float(std::clamp(value, 0.0, double(pre.inputSize - 1)));
    }
};

Sam2Tracker::Sam2Tracker()
    : m_impl(std::make_unique<Impl>())
{
}

Sam2Tracker::~Sam2Tracker() = default;

bool Sam2Tracker::initialize(const TrackingConfig &config, QString *error)
{
    Impl &impl = *m_impl;
    impl.config = config;
    impl.initialized = false;

    if (config.inputSize <= 0) {
        if (error)
            *error = QStringLiteral("跟踪模型输入尺寸无效。");
        return false;
    }
    const QStringList required = {QStringLiteral("image_encoder"),
                                  QStringLiteral("prompt_encoder"),
                                  QStringLiteral("mask_decoder")};
    for (const QString &part : required) {
        if (!config.partPaths.contains(part)
            || config.partPaths.value(part).isEmpty()) {
            if (error) {
                *error = QStringLiteral("跟踪模型缺少组件：%1。")
                             .arg(part);
            }
            return false;
        }
    }

    EpConfig ep;
    ep.deviceIndex = config.deviceIndex;
    ep.allowDml = config.allowDml;
    ep.allowCpu = config.allowCpu;
    // EfficientTAM has a few shape ops ORT assigns to CPU; allow the hybrid
    // session so the GPU handles the heavy nodes. provider() still reports dml.
    ep.requirePureDml = false;
    QMap<QString, QString> paths;
    for (auto it = config.partPaths.constBegin();
         it != config.partPaths.constEnd(); ++it) {
        paths.insert(it.key(), it.value());
    }
    if (!impl.executor.initialize(required, paths, ep, error))
        return false;

    const auto io = config.io;
    const auto readName = [&io](const char *key, const QString &fallback) {
        const QJsonValue value = io.value(QLatin1String(key));
        return value.isString() && !value.toString().isEmpty()
            ? value.toString()
            : fallback;
    };
    impl.imageInput =
        readName("imageInput", QStringLiteral("image"));
    impl.imageEmbedding =
        readName("imageEmbedding", QStringLiteral("vision_features"));
    impl.imagePosEmbedding =
        readName("imagePosEmbedding", QStringLiteral("vision_pos_enc"));
    impl.maskLogits = readName("maskLogits", QStringLiteral("masks"));
    impl.iouScore = readName("iouScore", QStringLiteral("iou_pred"));
    impl.objectScore = readName("objectScore", QString());

    impl.initialized = true;
    if (error)
        error->clear();
    return true;
}

TrackingPrompt Sam2Tracker::promptFromMask(const BinaryMask &mask)
{
    TrackingPrompt prompt;
    if (!mask.isValid())
        return prompt;

    int minX = mask.width;
    int minY = mask.height;
    int maxX = -1;
    int maxY = -1;
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
    if (maxX < minX || maxY < minY)
        return prompt;

    const double width = double(mask.width);
    const double height = double(mask.height);
    const double left = double(minX) / width;
    const double top = double(minY) / height;
    const double right = double(maxX + 1) / width;
    const double bottom = double(maxY + 1) / height;
    prompt.box = QRectF(left, top, right - left, bottom - top);
    // SAM box prompt: two corner points with labels 2 (top-left) and 3
    // (bottom-right). Do not append a centroid: the prompt encoder emits the
    // three sparse tokens the decoder expects from these two points.
    prompt.points.append(QPointF(left, top));
    prompt.points.append(QPointF(right, bottom));
    prompt.labels = {2, 3};
    return prompt;
}

std::optional<TrackingFrameResult> Sam2Tracker::segment(
    const QImage &image, const TrackingPrompt &prompt, QString *error)
{
    Impl &impl = *m_impl;
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return std::optional<TrackingFrameResult>{};
    };
    if (!impl.initialized) {
        return fail(QStringLiteral("跟踪模型尚未初始化。"));
    }
    if (image.isNull())
        return fail(QStringLiteral("跟踪输入图像为空。"));

    const int inputSize = impl.config.inputSize;
    const Impl::Preprocessed pre =
        Impl::preprocess(image, inputSize, error);
    if (pre.data.empty())
        return fail(error && !error->isEmpty()
                        ? *error
                        : QStringLiteral("跟踪输入预处理失败。"));

    // Resolve a SAM-style prompt: a 2-point box (labels 2/3) is preferred; a
    // legacy 3-point foreground prompt is still accepted. Cap at three input
    // points so the prompt encoder emits at most four sparse tokens.
    QList<QPointF> points = prompt.points;
    QList<int> labels = prompt.labels;
    if (points.size() < 2) {
        points.clear();
        labels.clear();
        if (prompt.box.isValid()) {
            points.append(prompt.box.topLeft());
            points.append(prompt.box.bottomRight());
            labels = {2, 3};
        }
    }
    if (points.size() < 2)
        return fail(QStringLiteral("跟踪提示为空。"));
    if (points.size() > 3)
        points = points.mid(0, 3);
    while (labels.size() < points.size()) {
        // Default labels: a 2-point prompt without labels is a box prompt.
        labels.append(points.size() == 2 ? (labels.isEmpty() ? 2 : 3) : 1);
    }

    const QString imageInputName = impl.resolveName(
        QStringLiteral("image_encoder"), true, impl.imageInput,
        {1, 3, inputSize, inputSize}, 0);
    if (imageInputName.isEmpty())
        return fail(QStringLiteral("跟踪模型 image_encoder 缺少图像输入。"));

    QMap<QString, OnnxTensor> encoderInputs;
    OnnxTensor imageTensor;
    imageTensor.shape = {1, 3, inputSize, inputSize};
    imageTensor.floatData = pre.data;
    encoderInputs.insert(imageInputName, imageTensor);

    QMap<QString, OnnxOutput> encoderOutputs;
    if (!impl.executor.run(QStringLiteral("image_encoder"), encoderInputs,
                           {impl.imageEmbedding, impl.imagePosEmbedding},
                           &encoderOutputs, error)) {
        return std::nullopt;
    }
    if (!encoderOutputs.contains(impl.imageEmbedding)
        || !encoderOutputs.contains(impl.imagePosEmbedding)) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }
    const OnnxOutput &visionFeatures = encoderOutputs[impl.imageEmbedding];
    if (!shapeMatches(visionFeatures.shape, {1, -1, -1, -1})
        || visionFeatures.floatData.empty()) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }
    const OnnxOutput &visionPos = encoderOutputs[impl.imagePosEmbedding];
    if (!shapeMatches(visionPos.shape, {1, -1, -1, -1})
        || visionPos.floatData.empty()) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }

    const int pointCount = points.size();
    OnnxTensor pointCoords;
    pointCoords.shape = {1, pointCount, 2};
    pointCoords.floatData.resize(size_t(pointCount) * 2);
    OnnxTensor pointLabels;
    pointLabels.type = OnnxTensor::DataType::Int32;
    pointLabels.shape = {1, pointCount};
    pointLabels.int32Data.resize(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        const QPointF &point = points.at(i);
        pointCoords.floatData[i * 2] = Impl::toInputX(pre, point.x());
        pointCoords.floatData[i * 2 + 1] = Impl::toInputY(pre, point.y());
        // Preserve SAM's box labels 2/3; foreground point prompts use 1.
        pointLabels.int32Data[i] = labels.at(i);
    }
    OnnxTensor maskInput;
    maskInput.shape = {1, 1, 128, 128};
    maskInput.floatData.assign(size_t(128) * 128, 0.0f);
    OnnxTensor hasMask;
    hasMask.shape = {1};
    hasMask.floatData = {0.0f};

    QMap<QString, OnnxTensor> promptInputs;
    promptInputs.insert(QStringLiteral("point_coords"), pointCoords);
    promptInputs.insert(QStringLiteral("point_labels"), pointLabels);
    promptInputs.insert(QStringLiteral("mask_input"), maskInput);
    promptInputs.insert(QStringLiteral("has_mask"), hasMask);

    QMap<QString, OnnxOutput> promptOutputs;
    if (!impl.executor.run(
            QStringLiteral("prompt_encoder"), promptInputs,
            {QStringLiteral("sparse_prompt_embeddings"),
             QStringLiteral("dense_prompt_embeddings")},
            &promptOutputs, error)) {
        return std::nullopt;
    }
    if (!promptOutputs.contains(QStringLiteral("sparse_prompt_embeddings"))
        || !promptOutputs.contains(QStringLiteral("dense_prompt_embeddings"))) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }
    const OnnxOutput &sparse =
        promptOutputs[QStringLiteral("sparse_prompt_embeddings")];
    // A 2-point box prompt encodes to exactly three sparse tokens (two real +
    // padding) and is used as-is. A legacy 3-point prompt encodes to
    // [1,4,256]; the decoder consumes exactly three, so the trailing padding
    // token is dropped. Both cases therefore take the first three tokens.
    if (sparse.shape.size() != 3 || sparse.shape.at(1) < 3
        || sparse.shape.at(2) <= 0 || sparse.floatData.empty()) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }
    const int64_t tokenCount = sparse.shape.at(1);
    const int64_t tokenDim = sparse.shape.at(2);
    if (sparse.floatData.size() < size_t(tokenCount * tokenDim))
        return fail(QString::fromUtf8(kShapeMismatch));
    const OnnxOutput &dense =
        promptOutputs[QStringLiteral("dense_prompt_embeddings")];
    if (!shapeMatches(dense.shape, {1, -1, -1, -1})
        || dense.floatData.empty()) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }

    QMap<QString, OnnxTensor> decoderInputs;
    OnnxTensor imageEmbeddingTensor;
    imageEmbeddingTensor.shape = visionFeatures.shape;
    imageEmbeddingTensor.floatData = visionFeatures.floatData;
    decoderInputs.insert(QStringLiteral("image_embeddings"),
                         imageEmbeddingTensor);
    OnnxTensor sparseTensor;
    sparseTensor.shape = {1, 3, tokenDim};
    sparseTensor.floatData.assign(sparse.floatData.begin(),
                                  sparse.floatData.begin()
                                      + size_t(3 * tokenDim));
    decoderInputs.insert(QStringLiteral("sparse_prompt_embeddings"),
                         sparseTensor);
    OnnxTensor denseTensor;
    denseTensor.shape = dense.shape;
    denseTensor.floatData = dense.floatData;
    decoderInputs.insert(QStringLiteral("dense_prompt_embeddings"),
                         denseTensor);

    QStringList decoderOutputs{impl.maskLogits, impl.iouScore};
    if (!impl.objectScore.isEmpty())
        decoderOutputs.append(impl.objectScore);

    QMap<QString, OnnxOutput> decoderResults;
    if (!impl.executor.run(QStringLiteral("mask_decoder"), decoderInputs,
                           decoderOutputs, &decoderResults, error)) {
        return std::nullopt;
    }
    if (!decoderResults.contains(impl.maskLogits)
        || !decoderResults.contains(impl.iouScore)) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }
    const OnnxOutput &maskOutput = decoderResults[impl.maskLogits];
    if (!shapeMatches(maskOutput.shape, {1, 3, inputSize, inputSize})
        || maskOutput.floatData.size()
               != size_t(product(maskOutput.shape))) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }
    const OnnxOutput &iouOutput = decoderResults[impl.iouScore];
    if (!shapeMatches(iouOutput.shape, {1, 3})
        || iouOutput.floatData.size() != 3) {
        return fail(QString::fromUtf8(kShapeMismatch));
    }

    int bestChannel = 0;
    for (int channel = 1; channel < 3; ++channel) {
        if (iouOutput.floatData[size_t(channel)]
            > iouOutput.floatData[size_t(bestChannel)]) {
            bestChannel = channel;
        }
    }

    const int sourceWidth = pre.sourceWidth;
    const int sourceHeight = pre.sourceHeight;
    BinaryMask mask;
    mask.width = sourceWidth;
    mask.height = sourceHeight;
    mask.bits.assign(size_t(sourceWidth) * size_t(sourceHeight), 0);
    int area = 0;
    const int planeStride = inputSize * inputSize;
    const float *logits =
        maskOutput.floatData.data() + size_t(bestChannel) * planeStride;
    const double scaleX = double(pre.newWidth) / double(sourceWidth);
    const double scaleY = double(pre.newHeight) / double(sourceHeight);
    for (int y = 0; y < sourceHeight; ++y) {
        const int outY = pre.padY + int((y + 0.5) * scaleY);
        if (outY < 0 || outY >= inputSize)
            continue;
        for (int x = 0; x < sourceWidth; ++x) {
            const int outX = pre.padX + int((x + 0.5) * scaleX);
            if (outX < 0 || outX >= inputSize)
                continue;
            if (logits[size_t(outY) * inputSize + outX] > 0.0f) {
                mask.bits[size_t(y) * sourceWidth + x] = 255;
                ++area;
            }
        }
    }

    TrackingFrameResult result;
    result.mask = std::move(mask);
    result.predictedIou =
        std::clamp(iouOutput.floatData[size_t(bestChannel)], 0.0f, 1.0f);
    result.areaPx = area;
    result.occlusion = 0.0f;
    if (!impl.objectScore.isEmpty()
        && decoderResults.contains(impl.objectScore)) {
        const OnnxOutput &objectOutput = decoderResults[impl.objectScore];
        if (!objectOutput.floatData.empty()) {
            const float score = objectOutput.floatData.front();
            const float probability = 1.0f / (1.0f + std::exp(-score));
            result.occlusion = std::clamp(1.0f - probability, 0.0f, 1.0f);
        }
    }

    if (error)
        error->clear();
    return result;
}

std::optional<TrackingFrameResult> Sam2Tracker::propagate(const QImage &,
                                                          QString *error)
{
    if (error)
        *error = QStringLiteral("逐帧记忆传播未启用。");
    return std::nullopt;
}

void Sam2Tracker::resetMemory()
{
    // Keyframe-only mode keeps no memory bank; this is a no-op kept so the
    // SAM2-compatible interface and future task 6 wiring stay stable.
}

QString Sam2Tracker::provider() const { return m_impl->executor.provider(); }

bool Sam2Tracker::usingCpuFallback() const
{
    return m_impl->executor.usingCpuFallback();
}

} // namespace VideoEditor
