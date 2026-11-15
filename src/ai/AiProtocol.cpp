#include "AiProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace VideoEditor {

namespace {

QJsonObject maskKeyframeToJson(const MaskKeyframe &keyframe)
{
    return QJsonObject{
        {QStringLiteral("id"), keyframe.id},
        {QStringLiteral("sourceTimeUs"), static_cast<double>(keyframe.sourceTimeUs)},
        {QStringLiteral("maskHash"), keyframe.maskHash},
        {QStringLiteral("featherPx"), keyframe.featherPx},
        {QStringLiteral("revision"), keyframe.revision},
    };
}

MaskKeyframe maskKeyframeFromJson(const QJsonObject &json)
{
    MaskKeyframe keyframe;
    keyframe.id = json.value(QStringLiteral("id")).toString();
    keyframe.sourceTimeUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceTimeUs")).toDouble(0));
    keyframe.maskHash = json.value(QStringLiteral("maskHash")).toString();
    keyframe.featherPx = json.value(QStringLiteral("featherPx")).toInt(4);
    keyframe.revision = json.value(QStringLiteral("revision")).toInt(1);
    return keyframe;
}

QJsonObject parametersToJson(const RemovalParameters &parameters)
{
    return QJsonObject{
        {QStringLiteral("mode"), repairModeToString(parameters.mode)},
        {QStringLiteral("temporalStabilization"),
         parameters.temporalStabilization},
        {QStringLiteral("stabilizationStrength"),
         parameters.stabilizationStrength},
        {QStringLiteral("processingWidth"), parameters.processingWidth},
        {QStringLiteral("processingHeight"), parameters.processingHeight},
    };
}

RemovalParameters parametersFromJson(const QJsonObject &json)
{
    RemovalParameters parameters;
    bool modeOk = false;
    parameters.mode = repairModeFromString(
        json.value(QStringLiteral("mode")).toString(), &modeOk);
    if (!modeOk)
        parameters.mode = RepairMode::Fast;
    parameters.temporalStabilization =
        json.value(QStringLiteral("temporalStabilization")).toBool(true);
    parameters.stabilizationStrength =
        json.value(QStringLiteral("stabilizationStrength")).toInt(50);
    parameters.processingWidth =
        json.value(QStringLiteral("processingWidth")).toInt(1920);
    parameters.processingHeight =
        json.value(QStringLiteral("processingHeight")).toInt(1080);
    return parameters;
}

} // namespace

QJsonObject aiJobSpecToJson(const AiJobSpec &spec)
{
    QJsonArray keyframes;
    for (const MaskKeyframe &keyframe : spec.maskKeyframes)
        keyframes.append(maskKeyframeToJson(keyframe));

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
        {QStringLiteral("parameters"), parametersToJson(spec.parameters)},
        {QStringLiteral("redoFirstFrame"), spec.redoFirstFrame},
        {QStringLiteral("redoLastFrame"), spec.redoLastFrame},
        {QStringLiteral("resume"), spec.resume},
        {QStringLiteral("trackingOnly"), spec.trackingOnly},
    };
}

std::optional<AiJobSpec> aiJobSpecFromJson(const QJsonObject &json,
                                           QString *error)
{
    if (json.isEmpty()) {
        if (error)
            *error = QStringLiteral("AI 任务描述为空。");
        return std::nullopt;
    }

    AiJobSpec spec;
    spec.jobId = json.value(QStringLiteral("jobId")).toString();
    spec.clipId = json.value(QStringLiteral("clipId")).toString();
    spec.assetPath = json.value(QStringLiteral("assetPath")).toString();
    spec.sidecarRoot = json.value(QStringLiteral("sidecarRoot")).toString();
    spec.cacheDir = json.value(QStringLiteral("cacheDir")).toString();
    spec.cacheKey = json.value(QStringLiteral("cacheKey")).toString();
    spec.modelsManifestPath =
        json.value(QStringLiteral("modelsManifestPath")).toString();
    spec.trackingModelId =
        json.value(QStringLiteral("trackingModelId")).toString();
    spec.inpaintModelId =
        json.value(QStringLiteral("inpaintModelId")).toString();
    spec.sourceInUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceInUs")).toDouble(0));
    spec.sourceOutUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceOutUs")).toDouble(0));
    spec.width = json.value(QStringLiteral("width")).toInt(1920);
    spec.height = json.value(QStringLiteral("height")).toInt(1080);

    const QJsonValue fpsValue = json.value(QStringLiteral("fps"));
    if (fpsValue.isObject()) {
        const QJsonObject fps = fpsValue.toObject();
        spec.fps.num = fps.value(QStringLiteral("num")).toInt(30);
        spec.fps.den = fps.value(QStringLiteral("den")).toInt(1);
    }

    const QJsonArray keyframes =
        json.value(QStringLiteral("maskKeyframes")).toArray();
    for (const QJsonValue &value : keyframes) {
        if (value.isObject())
            spec.maskKeyframes.append(maskKeyframeFromJson(value.toObject()));
    }

    spec.parameters = parametersFromJson(
        json.value(QStringLiteral("parameters")).toObject());
    spec.redoFirstFrame =
        json.value(QStringLiteral("redoFirstFrame")).toInt(-1);
    spec.redoLastFrame = json.value(QStringLiteral("redoLastFrame")).toInt(-1);
    spec.resume = json.value(QStringLiteral("resume")).toBool(false);
    spec.trackingOnly =
        json.value(QStringLiteral("trackingOnly")).toBool(false);

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
    if (!event.isString() && !cmd.isString()) {
        if (event.isUndefined() && cmd.isUndefined())
            return fail(QStringLiteral("消息缺少 event 或 cmd。"));
        return fail(QStringLiteral("event/cmd 必须是字符串。"));
    }

    if (out)
        *out = object;
    if (error)
        error->clear();
    return true;
}

} // namespace VideoEditor
