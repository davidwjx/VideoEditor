// src/core/Removal.h
#pragma once

#include <cstdint>
#include <QList>
#include <QSize>
#include <QString>

namespace VideoEditor {

struct Asset;

enum class RemovalTrackingState {
    NotTracked, Tracking, Partial, Complete, LowConfidence, Failed
};
QString removalTrackingStateToString(RemovalTrackingState state);
RemovalTrackingState removalTrackingStateFromString(const QString &text,
                                                    bool *ok = nullptr);

enum class RepairMode { Fast, Quality };
QString repairModeToString(RepairMode mode);
RepairMode repairModeFromString(const QString &text, bool *ok = nullptr);

struct MaskKeyframe {
    QString id;
    int64_t sourceTimeUs = 0;   // source time, not timeline time
    QString maskHash;           // 64 lowercase hex, RLE sidecar hash
    int featherPx = 4;          // 0..64, not baked into maskHash
    int revision = 1;
    bool operator==(const MaskKeyframe &) const = default;
};

struct RemovalParameters {
    RepairMode mode = RepairMode::Fast;
    bool temporalStabilization = true;
    int stabilizationStrength = 50; // 0..100
    int processingWidth = 1920;
    int processingHeight = 1080;
    bool operator==(const RemovalParameters &) const = default;
};

struct RemovalLayer {
    QString id;
    QString clipId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    QList<MaskKeyframe> maskKeyframes;
    RemovalTrackingState trackingState = RemovalTrackingState::NotTracked;
    QString trackingConfidenceHash; // 64 hex, tracking sidecar hash
    RemovalParameters parameters;
    QString cacheKey;   // 64 hex or empty
    QString cachePath;  // "removal/<cacheKey>" or empty
    int revision = 1;
    bool enabled = true;
    bool operator==(const RemovalLayer &) const = default;
};

// Display geometry after applying 90/270 rotation. Masks, cache manifests and
// RepairFrameSource all use this size so patches on rotated assets line up with
// preview and export.
QSize displaySizeForAsset(const Asset &asset);

} // namespace VideoEditor
