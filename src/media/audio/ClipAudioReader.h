#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QString>

#include "Project.h"

namespace VideoEditor {

class FfmpegAudioSource;
class OlaTimeStretcher;

// Renders the source audio of one timeline clip. Each reader owns its decoder
// and (for pitch-preserving speed changes) its own stretcher state, so two
// clips that reference the same asset never share a read cursor.
class ClipAudioReader {
public:
    ClipAudioReader();
    ~ClipAudioReader();

    ClipAudioReader(const ClipAudioReader &) = delete;
    ClipAudioReader &operator=(const ClipAudioReader &) = delete;

    bool open(const Asset &asset, int sampleRate, int channels, QString *error);
    // Renders `frames` interleaved frames for the block starting
    // `blockLocalUs` microseconds into the clip. Missing/EOF regions are silent.
    void render(const Clip &clip, int64_t blockLocalUs, int frames, float *out);
    void reset();

private:
    std::unique_ptr<FfmpegAudioSource> m_source;
    std::unique_ptr<OlaTimeStretcher> m_stretcher;
    std::vector<float> m_sourceScratch;
    int m_sampleRate = 48000;
    int m_channels = 2;
    bool m_open = false;
};

} // namespace VideoEditor
