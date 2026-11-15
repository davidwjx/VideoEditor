// src/core/Removal.cpp
#include "Removal.h"

#include "Project.h"

namespace VideoEditor {

QString removalTrackingStateToString(RemovalTrackingState s)
{
    switch (s) {
    case RemovalTrackingState::NotTracked:
        return QStringLiteral("notTracked");
    case RemovalTrackingState::Tracking:
        return QStringLiteral("tracking");
    case RemovalTrackingState::Partial:
        return QStringLiteral("partial");
    case RemovalTrackingState::Complete:
        return QStringLiteral("complete");
    case RemovalTrackingState::LowConfidence:
        return QStringLiteral("lowConfidence");
    case RemovalTrackingState::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

RemovalTrackingState removalTrackingStateFromString(const QString &text, bool *ok)
{
    const QString value = text.trimmed();
    if (ok)
        *ok = true;
    if (value == QStringLiteral("notTracked"))
        return RemovalTrackingState::NotTracked;
    if (value == QStringLiteral("tracking"))
        return RemovalTrackingState::Tracking;
    if (value == QStringLiteral("partial"))
        return RemovalTrackingState::Partial;
    if (value == QStringLiteral("complete"))
        return RemovalTrackingState::Complete;
    if (value == QStringLiteral("lowConfidence"))
        return RemovalTrackingState::LowConfidence;
    if (value == QStringLiteral("failed"))
        return RemovalTrackingState::Failed;
    // Unknown values are treated as a failed tracking state rather than
    // silently upgrading to "complete".
    if (ok)
        *ok = false;
    return RemovalTrackingState::Failed;
}

QString repairModeToString(RepairMode mode)
{
    switch (mode) {
    case RepairMode::Fast:
        return QStringLiteral("fast");
    case RepairMode::Quality:
        return QStringLiteral("quality");
    }
    return QStringLiteral("fast");
}

RepairMode repairModeFromString(const QString &text, bool *ok)
{
    const QString value = text.trimmed();
    if (value == QStringLiteral("quality")) {
        if (ok)
            *ok = true;
        return RepairMode::Quality;
    }
    if (value == QStringLiteral("fast")) {
        if (ok)
            *ok = true;
        return RepairMode::Fast;
    }
    if (ok)
        *ok = false;
    return RepairMode::Fast;
}

QSize displaySizeForAsset(const Asset &asset)
{
    const MediaInfo &info = asset.info;
    if (info.width <= 0 || info.height <= 0)
        return QSize(1920, 1080);
    const int rotation = ((info.rotationDegrees % 360) + 360) % 360;
    if (rotation == 90 || rotation == 270)
        return QSize(info.height, info.width);
    return QSize(info.width, info.height);
}

} // namespace VideoEditor
