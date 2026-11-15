#pragma once

#include "Timeline.h"

namespace VideoEditor {

// Destination rectangle in normalized output coordinates: (0,0) is the top
// left of the output frame, w/h are fractions of the output size. Values
// outside [0,1] are legal (a transform can pan or zoom a layer off frame).
struct LayerRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
};

// Maps a clip transform onto the output frame. A default transform fits the
// source inside the output and centers it; scale factors multiply that fit.
LayerRect layerRectFor(const Transform &transform, int sourceW, int sourceH,
                       int outW, int outH);

} // namespace VideoEditor
