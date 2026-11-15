#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include <QString>

#include "Project.h"

namespace VideoEditor {

class ClipAudioReader;

struct MixStats {
    float peak = 0.0f;
    int activeLayers = 0;
};

// Gain for a clip at `localTimeUs` (relative to the clip start), combining the
// clip volume, interpolated keyframes and the fade-in/fade-out ramps.
float clipGainAt(const Clip &clip, int64_t localTimeUs);

// Constant-power stereo pan. -1000 is hard left, 0 is centre, 1000 hard right.
void panGains(int panMilli, float *left, float *right);

// Mixes all audible audio layers of a project into interleaved 48 kHz float
// blocks. Positions are derived from an absolute integer frame count, so long
// playback never drifts.
class TimelineMixer {
public:
    explicit TimelineMixer(int sampleRate = 48000, int channels = 2);
    ~TimelineMixer();

    TimelineMixer(const TimelineMixer &) = delete;
    TimelineMixer &operator=(const TimelineMixer &) = delete;

    void setProject(const Project &project);
    const Project &project() const { return m_project; }
    void setPosition(int64_t positionUs);
    int64_t positionUs() const;

    void render(float *interleaved, int frames, MixStats *stats = nullptr);

private:
    ClipAudioReader *readerFor(const Clip &clip);
    int64_t positionForFrameCount(int64_t frames) const;

    Project m_project;
    int m_sampleRate = 48000;
    int m_channels = 2;
    int64_t m_startPositionUs = 0;
    int64_t m_framesRendered = 0;
    std::map<QString, std::unique_ptr<ClipAudioReader>> m_readers;
};

} // namespace VideoEditor
