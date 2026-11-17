// src/ai/engine/TrackingPipeline.h
#pragma once

#include <functional>
#include <memory>

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "AiProtocol.h"
#include "MaskCodec.h"
#include "Removal.h"

namespace VideoEditor {

class Sam2Tracker;

struct TrackingRunCallbacks {
    // Receives progress/chunkDone objects (already shaped for the protocol).
    std::function<void(const QJsonObject &)> event;
    // Polled once per frame; returning true stops the run and preserves every
    // mask written so far (state becomes Partial).
    std::function<bool()> cancel;
};

struct TrackingRunResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString confidenceHash;
    int completedFrames = 0;
    RemovalTrackingState state = RemovalTrackingState::Failed;
};

// Offline tracking: anchor on the authored keyframes, propagate between them
// with DIS optical flow and refine every K frames with EfficientTAM.
class TrackingPipeline {
public:
    TrackingPipeline();
    ~TrackingPipeline();
    TrackingPipeline(const TrackingPipeline &) = delete;
    TrackingPipeline &operator=(const TrackingPipeline &) = delete;

    // Not owned. When null, the pipeline runs DIS-only (used by tests).
    void setTracker(Sam2Tracker *tracker);

    // Model refinement cadence. 1 = refine every frame (interactive GPU),
    // 5 = keyframe cadence used on CPU-only machines.
    void setRefineInterval(int interval);

    // Decodes the asset range with FrameReader and loads authored masks from
    // <sidecarRoot>/masks/<hash>.rle.
    TrackingRunResult run(const AiJobSpec &spec,
                          const TrackingRunCallbacks &callbacks = {});

    // Deterministic entry point: frame 0 corresponds to sourceInUs.
    TrackingRunResult runFrames(const AiJobSpec &spec,
                                const QList<QImage> &frames,
                                const QList<int> &keyframeFrameIndices,
                                const QList<BinaryMask> &keyframeMasks,
                                const TrackingRunCallbacks &callbacks = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
