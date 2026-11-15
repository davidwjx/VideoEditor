#pragma once

#include <cstdint>

namespace VideoEditor {

enum class FrameFetchAction {
    Reuse,  // the current frame still covers the requested time
    Decode, // advance the decoder forward, skipping at most maxFramesToDrop
    Seek,   // jump the decoder (backwards or a large forward jump)
};

struct FrameFetchPlan {
    FrameFetchAction action = FrameFetchAction::Seek;
    int maxFramesToDrop = 0;
};

// Decides how ClipVideoSource should reach targetUs from the frame whose
// presentation timestamp is currentPtsUs. A negative currentPtsUs means no
// frame has been decoded yet.
FrameFetchPlan planFrameFetch(int64_t currentPtsUs, int64_t targetUs,
                              int64_t frameDurationUs,
                              int64_t seekThresholdUs = 500'000);

} // namespace VideoEditor
