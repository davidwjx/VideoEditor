#include "RemovalCacheKey.h"

#include <limits>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

namespace VideoEditor {

QString canonicalRemovalKeyJson(const RemovalKeyInput &input)
{
    QJsonObject root;
    root.insert(QStringLiteral("assetFingerprint"), input.assetFingerprint);
    root.insert(QStringLiteral("cacheKeyVersion"), 1);
    root.insert(QStringLiteral("colorSpace"), input.colorSpace);

    QJsonObject fps;
    fps.insert(QStringLiteral("den"), input.fps.den);
    fps.insert(QStringLiteral("num"), input.fps.num);
    root.insert(QStringLiteral("fps"), fps);

    root.insert(QStringLiteral("height"), input.height);

    QJsonObject inpaintModel;
    inpaintModel.insert(QStringLiteral("hash"), input.inpaintModelHash);
    inpaintModel.insert(QStringLiteral("id"), input.inpaintModelId);
    root.insert(QStringLiteral("inpaintModel"), inpaintModel);

    QJsonArray masks;
    for (const MaskKeyframe &keyframe : input.maskKeyframes) {
        QJsonObject mask;
        mask.insert(QStringLiteral("featherPx"), keyframe.featherPx);
        mask.insert(QStringLiteral("hash"), keyframe.maskHash);
        mask.insert(QStringLiteral("revision"), keyframe.revision);
        mask.insert(QStringLiteral("timeUs"), double(keyframe.sourceTimeUs));
        masks.append(mask);
    }
    root.insert(QStringLiteral("masks"), masks);

    QJsonObject parameters;
    parameters.insert(QStringLiteral("mode"),
                      repairModeToString(input.parameters.mode));
    parameters.insert(QStringLiteral("processingHeight"),
                      input.parameters.processingHeight);
    parameters.insert(QStringLiteral("processingWidth"),
                      input.parameters.processingWidth);
    parameters.insert(QStringLiteral("stabilizationStrength"),
                      input.parameters.stabilizationStrength);
    parameters.insert(QStringLiteral("temporalStabilization"),
                      input.parameters.temporalStabilization);
    root.insert(QStringLiteral("parameters"), parameters);

    root.insert(QStringLiteral("pipelineVersion"), input.pipelineVersion);
    root.insert(QStringLiteral("sourceInUs"), double(input.sourceInUs));
    root.insert(QStringLiteral("sourceOutUs"), double(input.sourceOutUs));

    QJsonObject trackingModel;
    trackingModel.insert(QStringLiteral("hash"), input.trackingModelHash);
    trackingModel.insert(QStringLiteral("id"), input.trackingModelId);
    root.insert(QStringLiteral("trackingModel"), trackingModel);

    root.insert(QStringLiteral("width"), input.width);
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString computeRemovalCacheKey(const RemovalKeyInput &input)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(canonicalRemovalKeyJson(input).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

qint64 removalCacheBudgetBytes(qint64 sourceBytes)
{
    if (sourceBytes <= 0)
        return 0;
    const qint64 max = std::numeric_limits<qint64>::max();
    if (sourceBytes > max / 2)
        return max;
    return sourceBytes * 2;
}

} // namespace VideoEditor
