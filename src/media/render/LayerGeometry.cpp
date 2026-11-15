#include "LayerGeometry.h"

#include <algorithm>

namespace VideoEditor {

LayerRect layerRectFor(const Transform &transform, int sourceW, int sourceH,
                       int outW, int outH)
{
    if (sourceW <= 0 || sourceH <= 0 || outW <= 0 || outH <= 0)
        return {0.0f, 0.0f, 1.0f, 1.0f};

    const float fit = std::min(float(outW) / float(sourceW),
                               float(outH) / float(sourceH));
    const float scale = fit * (transform.scaleMilli / 1000.0f);
    const float w = float(sourceW) * scale / float(outW);
    const float h = float(sourceH) * scale / float(outH);
    const float x = 0.5f * (1.0f - w) + transform.offsetXPermille / 1000.0f;
    const float y = 0.5f * (1.0f - h) + transform.offsetYPermille / 1000.0f;
    return {x, y, w, h};
}

} // namespace VideoEditor
