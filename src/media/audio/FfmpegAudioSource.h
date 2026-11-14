#pragma once

#include <cstdint>
#include <memory>

#include <QString>

namespace VideoEditor {

// Decodes an audio stream and exposes it as interleaved 32-bit float samples
// at a fixed output sample rate / channel count (48 kHz stereo by default).
// All source positions are integer microseconds; no floating time accumulates.
class FfmpegAudioSource {
public:
    static std::unique_ptr<FfmpegAudioSource> open(const QString &path,
                                                   int sampleRate, int channels,
                                                   QString *error);
    ~FfmpegAudioSource();

    FfmpegAudioSource(const FfmpegAudioSource &) = delete;
    FfmpegAudioSource &operator=(const FfmpegAudioSource &) = delete;

    bool seek(int64_t sourceUs, QString *error);
    // Reads exactly `frames` frames starting at `startUs`. Regions outside the
    // media content are filled with silence, so reaching the end of an asset
    // never interrupts timeline mixing.
    bool readRange(int64_t startUs, int frames, float *out, QString *error);
    int64_t durationUs() const;

private:
    struct Impl;
    explicit FfmpegAudioSource(std::unique_ptr<Impl> impl);

    // Returns false on EOF or error; `error` is left empty only for EOF.
    bool decodeMore(QString *error);

    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
