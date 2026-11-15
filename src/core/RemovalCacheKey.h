#pragma once

#include <cstdint>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "Removal.h"
#include "TimeBase.h"

namespace VideoEditor {

// Everything that changes the generated repair pixels. Timeline position is
// deliberately absent: moving a clip must not invalidate its cache.
struct RemovalKeyInput {
    QString assetFingerprint;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    QList<MaskKeyframe> maskKeyframes;
    RemovalParameters parameters;
    QString trackingModelId;
    QString trackingModelHash;
    QString inpaintModelId;
    QString inpaintModelHash;
    QString pipelineVersion = QStringLiteral("m4.0");
    QString colorSpace = QStringLiteral("BT.709");
    Rational fps{30, 1};
    int width = 1920;
    int height = 1080;
};

QString canonicalRemovalKeyJson(const RemovalKeyInput &input);
QString computeRemovalCacheKey(const RemovalKeyInput &input);

// Cache budget shared by the UI cache status and acceptance checks:
// twice the source segment, saturating at qint64::max().
qint64 removalCacheBudgetBytes(qint64 sourceBytes);

} // namespace VideoEditor
