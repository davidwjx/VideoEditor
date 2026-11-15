// src/ai/engine/TemporalStabilizer.h
#pragma once

#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>

namespace VideoEditor {

// Cheap temporal stabilization for repaired patches: aligns +/-2 neighbours by
// their mask centroids, takes a per-pixel median inside the target hole and
// blends it in only when the window agrees (MAD gate 12/255). No optical flow;
// documented as limited for fast motion. Alpha is never modified.
class TemporalStabilizer {
public:
    static void stabilize(QList<QImage> &patches, const QList<QRect> &crops,
                          const QList<QPoint> &centroids, int strength);
};

} // namespace VideoEditor
