#pragma once

#include <atomic>
#include <cstdint>

#include <QtGlobal>

#include "TimeBase.h"

namespace VideoEditor {

// Master playback clock. The audio thread writes it as consumed frames grow;
// the GUI/video timer reads it lock-free. Positions are integer microseconds so
// long playback never drifts.
class AudioClock {
public:
    void reset(int64_t positionUs) { m_positionUs.store(positionUs); }
    void setPosition(int64_t positionUs) { m_positionUs.store(positionUs); }
    int64_t positionUs() const { return m_positionUs.load(); }

private:
    std::atomic<int64_t> m_positionUs{0};
};

// Maps a clock position onto the nearest project frame. Negative clock values
// are clamped to frame 0 (M2 design decision 5: video follows the audio clock).
inline int64_t frameIndexForClock(int64_t clockUs, Rational fps)
{
    return microsToFrames(qMax<int64_t>(0, clockUs), fps);
}

} // namespace VideoEditor
