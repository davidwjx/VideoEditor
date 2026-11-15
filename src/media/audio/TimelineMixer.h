#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include <QSet>
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

    // Starts at an exact integer sample. Export uses this so encoder input
    // boundaries line up sample-for-sample with the timeline.
    void setPositionSamples(int64_t sampleIndex);
    int64_t positionSamples() const;

    void render(float *interleaved, int frames, MixStats *stats = nullptr);

    // Number of decoder readers kept open. Test/inspection hook used to prove
    // parameter-only edits do not reopen everything and long timelines stay
    // bounded.
    int cachedReaderCountForTest() const { return int(m_readers.size()); }
    // Number of successful decoder opens since construction. A parameter-only
    // edit must not increase this counter.
    int readerOpenCountForTest() const { return m_readerOpenCount; }
    static constexpr int kMaxCachedReaders = 8;

private:
    struct ReaderEntry {
        std::unique_ptr<ClipAudioReader> reader;
        QString sourceKey;
        // Source-affecting fields of the clip the reader cursor belongs to.
        int64_t sourceInUs = 0;
        int64_t sourceOutUs = 0;
        int64_t timelineStartUs = 0;
        int speedMilli = 1000;
        bool preservePitch = false;
        quint64 lastUsedRender = 0;
    };

    ClipAudioReader *readerFor(const Clip &clip);
    int64_t positionForFrameCount(int64_t frames) const;
    static QString sourceKeyFor(const Asset &asset);
    void resetReaders();
    void dropStaleReaders();
    void evictUnusedReaders(const QSet<QString> &used);

    Project m_project;
    int m_sampleRate = 48000;
    int m_channels = 2;
    // -1 = legacy microsecond origin (preview); >= 0 = exact sample origin
    // (export). Keeping both modes preserves M2 preview timing exactly.
    int64_t m_startSamples = -1;
    int64_t m_startPositionUs = 0;
    int64_t m_framesRendered = 0;
    quint64 m_renderSerial = 0;
    int m_readerOpenCount = 0;
    std::map<QString, ReaderEntry> m_readers;
};

} // namespace VideoEditor
