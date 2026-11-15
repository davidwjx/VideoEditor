#include "AiProtocol.h"

#include <cmath>
#include <limits>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

namespace VideoEditor {

namespace {

bool isLowerHex64(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(text).hasMatch();
}

QString typeError(const char *field)
{
    return QStringLiteral("AI 任务描述字段 %1 类型无效。")
        .arg(QLatin1String(field));
}

bool readString(const QJsonObject &json, const char *field, QString *out,
                QString *error)
{
    if (!json.contains(QLatin1String(field)))
        return true;
    const QJsonValue value = json.value(QLatin1String(field));
    if (!value.isString()) {
        if (error)
            *error = typeError(field);
        return false;
    }
    *out = value.toString();
    return true;
}

bool readBool(const QJsonObject &json, const char *field, bool *out,
              QString *error)
{
    if (!json.contains(QLatin1String(field)))
        return true;
    const QJsonValue value = json.value(QLatin1String(field));
    if (!value.isBool()) {
        if (error)
            *error = typeError(field);
        return false;
    }
    *out = value.toBool();
    return true;
}

// JSON numbers are doubles. Reject fractional values and anything above the
// exact-integer range so no cast can silently overflow.
bool readInt64(const QJsonObject &json, const char *field, int64_t *out,
               QString *error)
{
    if (!json.contains(QLatin1String(field)))
        return true;
    const QJsonValue value = json.value(QLatin1String(field));
    if (!value.isDouble()) {
        if (error)
            *error = typeError(field);
        return false;
    }
    constexpr double kExactLimit = 9007199254740992.0; // 2^53
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number
        || std::abs(number) > kExactLimit) {
        if (error) {
            *error = QStringLiteral("AI 任务描述字段 %1 必须是 2^53 范围内的"
                                    "整数。")
                         .arg(QLatin1String(field));
        }
        return false;
    }
    *out = static_cast<int64_t>(number);
    return true;
}

bool readInt(const QJsonObject &json, const char *field, int *out,
             QString *error)
{
    if (!json.contains(QLatin1String(field)))
        return true;
    int64_t value = 0;
    if (!readInt64(json, field, &value, error))
        return false;
    if (value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max()) {
        if (error) {
            *error = QStringLiteral("AI 任务描述字段 %1 超出整数范围。")
                         .arg(QLatin1String(field));
        }
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool readObject(const QJsonObject &json, const char *field, QJsonObject *out,
                QString *error)
{
    if (!json.contains(QLatin1String(field)))
        return true;
    const QJsonValue value = json.value(QLatin1String(field));
    if (!value.isObject()) {
        if (error)
            *error = typeError(field);
        return false;
    }
    *out = value.toObject();
    return true;
}

bool maskKeyframeFromJson(const QJsonObject &json, MaskKeyframe *keyframe,
                          QString *error)
{
    if (!readString(json, "id", &keyframe->id, error)
        || !readInt64(json, "sourceTimeUs", &keyframe->sourceTimeUs, error)
        || !readString(json, "maskHash", &keyframe->maskHash, error)
        || !readInt(json, "featherPx", &keyframe->featherPx, error)
        || !readInt(json, "revision", &keyframe->revision, error)) {
        return false;
    }
    return true;
}

bool parametersFromJson(const QJsonObject &json, RemovalParameters *parameters,
                        QString *error)
{
    QString modeText;
    if (!readString(json, "mode", &modeText, error)
        || !readBool(json, "temporalStabilization",
                     &parameters->temporalStabilization, error)
        || !readInt(json, "stabilizationStrength",
                    &parameters->stabilizationStrength, error)
        || !readInt(json, "processingWidth", &parameters->processingWidth,
                    error)
        || !readInt(json, "processingHeight", &parameters->processingHeight,
                    error)) {
        return false;
    }
    if (json.contains(QStringLiteral("mode"))) {
        bool modeOk = false;
        parameters->mode = repairModeFromString(modeText, &modeOk);
        if (!modeOk) {
            if (error)
                *error = QStringLiteral("AI 任务描述字段 mode 无效。");
            return false;
        }
    }
    return true;
}

} // namespace

QJsonObject aiJobSpecToJson(const AiJobSpec &spec)
{
    QJsonArray keyframes;
    for (const MaskKeyframe &keyframe : spec.maskKeyframes) {
        keyframes.append(QJsonObject{
            {QStringLiteral("id"), keyframe.id},
            {QStringLiteral("sourceTimeUs"),
             static_cast<double>(keyframe.sourceTimeUs)},
            {QStringLiteral("maskHash"), keyframe.maskHash},
            {QStringLiteral("featherPx"), keyframe.featherPx},
            {QStringLiteral("revision"), keyframe.revision},
        });
    }

    return QJsonObject{
        {QStringLiteral("jobId"), spec.jobId},
        {QStringLiteral("clipId"), spec.clipId},
        {QStringLiteral("assetPath"), spec.assetPath},
        {QStringLiteral("sidecarRoot"), spec.sidecarRoot},
        {QStringLiteral("cacheDir"), spec.cacheDir},
        {QStringLiteral("cacheKey"), spec.cacheKey},
        {QStringLiteral("modelsManifestPath"), spec.modelsManifestPath},
        {QStringLiteral("trackingModelId"), spec.trackingModelId},
        {QStringLiteral("inpaintModelId"), spec.inpaintModelId},
        {QStringLiteral("sourceInUs"), static_cast<double>(spec.sourceInUs)},
        {QStringLiteral("sourceOutUs"), static_cast<double>(spec.sourceOutUs)},
        {QStringLiteral("width"), spec.width},
        {QStringLiteral("height"), spec.height},
        {QStringLiteral("fps"),
         QJsonObject{{QStringLiteral("num"), spec.fps.num},
                     {QStringLiteral("den"), spec.fps.den}}},
        {QStringLiteral("maskKeyframes"), keyframes},
        {QStringLiteral("parameters"),
         QJsonObject{
             {QStringLiteral("mode"), repairModeToString(spec.parameters.mode)},
             {QStringLiteral("temporalStabilization"),
              spec.parameters.temporalStabilization},
             {QStringLiteral("stabilizationStrength"),
              spec.parameters.stabilizationStrength},
             {QStringLiteral("processingWidth"),
              spec.parameters.processingWidth},
             {QStringLiteral("processingHeight"),
              spec.parameters.processingHeight}}},
        {QStringLiteral("redoFirstFrame"), spec.redoFirstFrame},
        {QStringLiteral("redoLastFrame"), spec.redoLastFrame},
        {QStringLiteral("resume"), spec.resume},
        {QStringLiteral("trackingOnly"), spec.trackingOnly},
    };
}

std::optional<AiJobSpec> aiJobSpecFromJson(const QJsonObject &json,
                                           QString *error)
{
    AiJobSpec spec;
    QString fieldError;

    if (!readString(json, "jobId", &spec.jobId, &fieldError)
        || !readString(json, "clipId", &spec.clipId, &fieldError)
        || !readString(json, "assetPath", &spec.assetPath, &fieldError)
        || !readString(json, "sidecarRoot", &spec.sidecarRoot, &fieldError)
        || !readString(json, "cacheDir", &spec.cacheDir, &fieldError)
        || !readString(json, "cacheKey", &spec.cacheKey, &fieldError)
        || !readString(json, "modelsManifestPath", &spec.modelsManifestPath,
                       &fieldError)
        || !readString(json, "trackingModelId", &spec.trackingModelId,
                       &fieldError)
        || !readString(json, "inpaintModelId", &spec.inpaintModelId,
                       &fieldError)
        || !readInt64(json, "sourceInUs", &spec.sourceInUs, &fieldError)
        || !readInt64(json, "sourceOutUs", &spec.sourceOutUs, &fieldError)
        || !readInt(json, "width", &spec.width, &fieldError)
        || !readInt(json, "height", &spec.height, &fieldError)
        || !readInt(json, "redoFirstFrame", &spec.redoFirstFrame, &fieldError)
        || !readInt(json, "redoLastFrame", &spec.redoLastFrame, &fieldError)
        || !readBool(json, "resume", &spec.resume, &fieldError)
        || !readBool(json, "trackingOnly", &spec.trackingOnly, &fieldError)) {
        if (error)
            *error = fieldError;
        return std::nullopt;
    }

    QJsonObject fps;
    if (!readObject(json, "fps", &fps, &fieldError)) {
        if (error)
            *error = fieldError;
        return std::nullopt;
    }
    if (!fps.isEmpty()) {
        if (!readInt(fps, "num", &spec.fps.num, &fieldError)
            || !readInt(fps, "den", &spec.fps.den, &fieldError)) {
            if (error)
                *error = fieldError;
            return std::nullopt;
        }
    }

    if (json.contains(QStringLiteral("parameters"))) {
        const QJsonValue value = json.value(QStringLiteral("parameters"));
        if (!value.isObject()) {
            if (error)
                *error = typeError("parameters");
            return std::nullopt;
        }
        if (!parametersFromJson(value.toObject(), &spec.parameters,
                                &fieldError)) {
            if (error)
                *error = fieldError;
            return std::nullopt;
        }
    }

    if (json.contains(QStringLiteral("maskKeyframes"))) {
        const QJsonValue value = json.value(QStringLiteral("maskKeyframes"));
        if (!value.isArray()) {
            if (error)
                *error = typeError("maskKeyframes");
            return std::nullopt;
        }
        for (const QJsonValue &entry : value.toArray()) {
            if (!entry.isObject()) {
                if (error) {
                    *error = QStringLiteral(
                        "AI 任务描述字段 maskKeyframes 元素类型无效。");
                }
                return std::nullopt;
            }
            MaskKeyframe keyframe;
            if (!maskKeyframeFromJson(entry.toObject(), &keyframe,
                                      &fieldError)) {
                if (error)
                    *error = fieldError;
                return std::nullopt;
            }
            spec.maskKeyframes.append(keyframe);
        }
    }

    if (error)
        error->clear();
    return spec;
}

QByteArray AiProtocol::encodeLine(const QJsonObject &object)
{
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

bool AiProtocol::parseLine(const QByteArray &line, QJsonObject *out,
                           QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QByteArray trimmed = line;
    while (trimmed.endsWith('\n') || trimmed.endsWith('\r'))
        trimmed.chop(1);

    if (trimmed.isEmpty())
        return fail(QStringLiteral("消息行为空。"));
    // A line of exactly kMaxLineBytes is allowed; one byte more is rejected.
    if (trimmed.size() > kMaxLineBytes)
        return fail(QStringLiteral("消息行超出长度上限。"));

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("JSON 解析失败：%1")
                        .arg(parseError.errorString()));
    if (!document.isObject())
        return fail(QStringLiteral("消息不是 JSON 对象。"));

    const QJsonObject object = document.object();
    const QJsonValue event = object.value(QStringLiteral("event"));
    const QJsonValue cmd = object.value(QStringLiteral("cmd"));
    if (!event.isUndefined() && !event.isString())
        return fail(QStringLiteral("event 必须是字符串。"));
    if (!cmd.isUndefined() && !cmd.isString())
        return fail(QStringLiteral("cmd 必须是字符串。"));
    if (event.isUndefined() && cmd.isUndefined())
        return fail(QStringLiteral("消息缺少 event 或 cmd。"));

    if (out)
        *out = object;
    if (error)
        error->clear();
    return true;
}

bool AiProtocol::validateJobSpec(const AiJobSpec &spec, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = QStringLiteral("AI 任务参数无效：") + reason;
        return false;
    };

    if (spec.jobId.isEmpty())
        return fail(QStringLiteral("jobId 为空。"));
    if (spec.assetPath.isEmpty())
        return fail(QStringLiteral("assetPath 为空。"));
    if (spec.sidecarRoot.isEmpty())
        return fail(QStringLiteral("sidecarRoot 为空。"));
    if (spec.cacheDir.isEmpty())
        return fail(QStringLiteral("cacheDir 为空。"));
    if (!isLowerHex64(spec.cacheKey))
        return fail(QStringLiteral("cacheKey 不是 64 位小写十六进制。"));
    if (spec.modelsManifestPath.isEmpty())
        return fail(QStringLiteral("modelsManifestPath 为空。"));
    if (spec.trackingModelId.isEmpty())
        return fail(QStringLiteral("trackingModelId 为空。"));
    if (spec.inpaintModelId.isEmpty())
        return fail(QStringLiteral("inpaintModelId 为空。"));
    // Beyond 2^53 a JSON double can no longer round-trip an int64 exactly;
    // reject here so the manager reports a parameter error instead of the
    // engine silently reading a different timestamp.
    constexpr int64_t kMaxExactUs = 9007199254740992LL; // 2^53
    if (spec.sourceInUs < 0)
        return fail(QStringLiteral("sourceInUs 为负数。"));
    if (spec.sourceInUs > kMaxExactUs || spec.sourceOutUs > kMaxExactUs)
        return fail(QStringLiteral("时间戳超出 2^53 微秒范围。"));
    if (spec.sourceOutUs <= spec.sourceInUs)
        return fail(QStringLiteral("sourceOutUs 必须大于 sourceInUs。"));
    if (spec.width < 1 || spec.width > 8192)
        return fail(QStringLiteral("width 超出 1..8192。"));
    if (spec.height < 1 || spec.height > 8192)
        return fail(QStringLiteral("height 超出 1..8192。"));
    if (spec.parameters.processingWidth < 1
        || spec.parameters.processingWidth > 8192) {
        return fail(QStringLiteral("processingWidth 超出 1..8192。"));
    }
    if (spec.parameters.processingHeight < 1
        || spec.parameters.processingHeight > 8192) {
        return fail(QStringLiteral("processingHeight 超出 1..8192。"));
    }
    if (spec.parameters.stabilizationStrength < 0
        || spec.parameters.stabilizationStrength > 100) {
        return fail(QStringLiteral("stabilizationStrength 超出 0..100。"));
    }
    if (spec.fps.num <= 0 || spec.fps.den <= 0)
        return fail(QStringLiteral("fps 无效。"));

    if (spec.maskKeyframes.isEmpty())
        return fail(QStringLiteral("缺少遮罩关键帧。"));
    QSet<QString> ids;
    int64_t previousTime = std::numeric_limits<int64_t>::min();
    for (int i = 0; i < spec.maskKeyframes.size(); ++i) {
        const MaskKeyframe &keyframe = spec.maskKeyframes.at(i);
        if (keyframe.id.isEmpty())
            return fail(QStringLiteral("遮罩关键帧 id 为空。"));
        if (ids.contains(keyframe.id))
            return fail(QStringLiteral("遮罩关键帧 id 重复。"));
        ids.insert(keyframe.id);
        if (i > 0 && keyframe.sourceTimeUs <= previousTime)
            return fail(QStringLiteral("遮罩关键帧时间必须严格递增。"));
        previousTime = keyframe.sourceTimeUs;
        if (keyframe.sourceTimeUs > kMaxExactUs
            || keyframe.sourceTimeUs < -kMaxExactUs) {
            return fail(QStringLiteral("遮罩关键帧时间戳超出 2^53 微秒范围。"));
        }
        // Sidecar filenames use lowercase hashes; an uppercase hash would
        // never resolve to an existing mask.
        if (!isLowerHex64(keyframe.maskHash))
            return fail(QStringLiteral(
                "遮罩关键帧 maskHash 不是 64 位小写十六进制。"));
        if (keyframe.featherPx < 0 || keyframe.featherPx > 64)
            return fail(QStringLiteral("遮罩关键帧 featherPx 超出 0..64。"));
    }

    if (spec.redoFirstFrame < -1 || spec.redoLastFrame < -1)
        return fail(QStringLiteral("重做范围无效。"));
    const bool firstSet = spec.redoFirstFrame >= 0;
    const bool lastSet = spec.redoLastFrame >= 0;
    if (firstSet != lastSet)
        return fail(QStringLiteral("重做范围必须同时设置首尾帧。"));
    if (firstSet) {
        if (spec.redoFirstFrame > spec.redoLastFrame)
            return fail(QStringLiteral("重做范围首帧晚于尾帧。"));
        const int64_t totalFrames =
            microsToFrames(spec.sourceOutUs - spec.sourceInUs, spec.fps);
        if (totalFrames <= 0)
            return fail(QStringLiteral("重做范围超出片段帧数。"));
        // Task 8's redoRange() is chunk-aligned (30 frames) and inclusive:
        // the last chunk of a 40-frame segment legitimately ends at frame 59.
        const int64_t paddedFrames = ((totalFrames + 29) / 30) * 30;
        if (spec.redoFirstFrame >= paddedFrames
            || spec.redoLastFrame >= paddedFrames) {
            return fail(QStringLiteral("重做范围超出片段帧数。"));
        }
    }

    if (error)
        error->clear();
    return true;
}

} // namespace VideoEditor
