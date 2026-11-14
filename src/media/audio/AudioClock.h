#pragma once

#include <cstdint>

#include <QtGlobal>

#include "TimeBase.h"

namespace VideoEditor {

// Shared by the mixer's integer position arithmetic (Task 7) and the
// audio playback clock (Task 8). Negative clock values are clamped to frame 0.
inline int64_t frameIndexForClock(int64_t clockUs, Rational fps)
{
    return microsToFrames(qMax<int64_t>(0, clockUs), fps);
}

} // namespace VideoEditor
