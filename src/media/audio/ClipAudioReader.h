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
//
// Level convention: FfmpegAudioSource returns libswresample's standard
// interleaved output. For mono sources upmixed to stereo, libswresample uses a
// 1/sqrt(2) matrix per channel (power preserving). ClipAudioReader intentionally
// applies a dual-mono +3 dB per channel trim (factor 2 on that matrix) so a mono
// clip is not perceived as quieter than a stereo clip in the preview mix. This
// is the documented preview convention and is deliberately NOT baked into
// FfmpegAudioSource, so waveform/export consumers still see the standard level.
// A mono source at per-channel level L therefore renders at sqrt(2)*L per
// channel here, i.e. 3 dB above a stereo source at the same per-channel level.
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

    static constexpr float kMonoDualMonoGain = 2.0f; // see class comment

private:
    int64_t sampleIndexForUs(int64_t us) const;
    int64_t usForSample(int64_t sample) const;
    void applyMonoTrim(float *out, int frames) const;

    std::unique_ptr<FfmpegAudioSource> m_source;
    std::unique_ptr<OlaTimeStretcher> m_stretcher;
    std::vector<float> m_sourceScratch;
    std::vector<float> m_olaScratch;
    int m_sampleRate = 48000;
    int m_channels = 2;
    int m_sourceChannels = 0;
    bool m_monoToStereo = false;
    bool m_open = false;
    // Pitch-preserving cursor: next absolute source sample to push into the
    // OLA queue and how much end-of-source zero padding is left to flush.
    bool m_olaPrimed = false;
    int64_t m_olaNextSample = 0;
    int64_t m_olaPadRemaining = 0;
};

} // namespace VideoEditor
