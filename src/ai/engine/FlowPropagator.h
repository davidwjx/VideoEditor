// src/ai/engine/FlowPropagator.h
#pragma once

#include <memory>

#include <QImage>
#include <QString>

#include "MaskCodec.h"

namespace VideoEditor {

struct FlowPropagationResult {
    bool ok = false;
    BinaryMask mask;
    float meanFlowPx = 0.0f;
    float maxFlowPx = 0.0f;
    float areaChangePercent = 0.0f;
    int areaPx = 0;
    QString error;
};

// Propagates a binary mask from one frame to the next with OpenCV DIS optical
// flow (PRESET_MEDIUM) and cv::remap. It is deterministic and needs no model,
// so the offline pipeline can use it between EfficientTAM keyframes.
class FlowPropagator {
public:
    FlowPropagator();
    ~FlowPropagator();
    FlowPropagator(const FlowPropagator &) = delete;
    FlowPropagator &operator=(const FlowPropagator &) = delete;

    // Warps previousMask (frame previous) forward onto `current`.
    FlowPropagationResult propagate(const QImage &previous,
                                    const QImage &current,
                                    const BinaryMask &previousMask);

    // Warps currentMask (frame current) backward onto `previous`, using the
    // previous->current flow and sampling the current mask at x+flow.
    FlowPropagationResult propagateBackward(const QImage &previous,
                                            const QImage &current,
                                            const BinaryMask &currentMask);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
