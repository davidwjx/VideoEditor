// src/ai/engine/Sam2Tracker.h
#pragma once

#include <memory>
#include <optional>

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QString>

#include "MaskCodec.h"

namespace VideoEditor {

// Normalized source coordinates (0..1). label 1 is foreground, 0 background.
struct TrackingPrompt {
    QRectF box;
    QList<QPointF> points;
    QList<int> labels;
};

struct TrackingFrameResult {
    BinaryMask mask;
    float predictedIou = 0.0f;
    float occlusion = 0.0f;
    int areaPx = 0;
};

struct TrackingConfig {
    QMap<QString, QString> partPaths;
    QJsonObject io;
    int inputSize = 512;
    int memoryBankSize = 5;
    int deviceIndex = 0;
    bool allowDml = false;
    bool allowCpu = true;
};

// Name kept for the SAM2-compatible interface, but the offline implementation
// runs EfficientTAM keyframe segmentation: image_encoder -> prompt_encoder ->
// mask_decoder. Per-frame memory propagation is intentionally not enabled; the
// tracking pipeline uses DIS optical flow between keyframes instead.
class Sam2Tracker {
public:
    Sam2Tracker();
    ~Sam2Tracker();
    Sam2Tracker(const Sam2Tracker &) = delete;
    Sam2Tracker &operator=(const Sam2Tracker &) = delete;

    bool initialize(const TrackingConfig &config, QString *error = nullptr);
    std::optional<TrackingFrameResult> segment(const QImage &image,
                                               const TrackingPrompt &prompt,
                                               QString *error = nullptr);
    std::optional<TrackingFrameResult> propagate(const QImage &image,
                                                 QString *error = nullptr);
    void resetMemory();

    QString provider() const;
    bool usingCpuFallback() const;

    // SAM-style box prompt: top-left label 2, bottom-right label 3. The prompt
    // encoder turns these two points into exactly three sparse tokens
    // (two real + padding), which the EfficientTAM mask_decoder consumes as-is.
    static TrackingPrompt promptFromMask(const BinaryMask &mask);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
