#include "FrameSelector.h"

#include <algorithm>

namespace VideoEditor {

FrameFetchPlan planFrameFetch(int64_t currentPtsUs, int64_t targetUs,
                              int64_t frameDurationUs, int64_t seekThresholdUs)
{
    frameDurationUs = std::max<int64_t>(1, frameDurationUs);
    if (currentPtsUs < 0)
        return {FrameFetchAction::Seek, 0};
    if (targetUs < currentPtsUs)
        return {FrameFetchAction::Seek, 0};
    if (targetUs < currentPtsUs + frameDurationUs)
        return {FrameFetchAction::Reuse, 0};

    const int64_t delta = targetUs - currentPtsUs;
    if (delta > seekThresholdUs)
        return {FrameFetchAction::Seek, 0};

    // delta spans at least one frame duration. Decoding delta/frameDuration
    // frames reaches the frame that covers the target; the frames strictly in
    // between are the ones that may be dropped.
    const int64_t framesAhead = delta / frameDurationUs;
    const int drops = int(std::min<int64_t>(framesAhead - 1, 120));
    return {FrameFetchAction::Decode, std::max(0, drops)};
}

} // namespace VideoEditor
