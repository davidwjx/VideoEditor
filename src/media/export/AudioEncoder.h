#pragma once

#include <cstdint>
#include <vector>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "ExportPackets.h"

namespace VideoEditor {

// Encodes interleaved 48 kHz float samples as AAC-LC. The export pipeline
// feeds exactly one AAC frame (1024 samples) per call and is responsible for
// zero-padding the final partial frame. Contexts start at sample 0; packet pts
// is the absolute sample index.
//
// Packet contract: the encoder emits one AAC pre-roll (priming) packet before
// the timeline content. It is exposed as the first element of `packets` and
// has a negative pts (-encoderDelaySamples()). It carries the MDCT overlap for
// the first timeline frame, so the muxer must write it and then discard the
// priming on playback exactly once via skip_samples = encoderDelaySamples().
// Every other packet has pts >= 0 and is timeline content. One instance is
// used per export; not thread-safe.
class AudioEncoder {
public:
    AudioEncoder() = default;
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder &) = delete;
    AudioEncoder &operator=(const AudioEncoder &) = delete;

    // Opens the native FFmpeg AAC encoder. On failure fills `error` with a
    // Chinese explanation.
    bool open(int sampleRate, int channels, int bitrateKbps,
              QString *error = nullptr);

    // Encodes `interleaved` (frames * channels floats) as the AAC frame whose
    // first sample is `sampleIndex` in the timeline. `frames` must equal
    // frameSize() and the first call must use sampleIndex 0; the pipeline pads
    // and splits its own buffers. New packets (including the initial pre-roll
    // packet) are appended to `packets`.
    bool encode(const float *interleaved, int frames, int64_t sampleIndex,
                std::vector<EncodedPacket> *packets, QString *error = nullptr);

    // Signals end of stream and drains delayed packets. Safe to call once
    // after the last frame.
    bool flush(std::vector<EncodedPacket> *packets, QString *error = nullptr);

    void close();

    // Samples per encoded frame (1024 for AAC-LC).
    int frameSize() const { return m_frameSize; }
    // Priming samples at the start of the encoded stream. The muxer writes
    // skip_samples with this value on the first (negative-pts) packet.
    int encoderDelaySamples() const { return m_delay; }

    AVCodecContext *context() const { return m_ctx; }

private:
    bool receivePackets(std::vector<EncodedPacket> *packets, QString *error);

    AVCodecContext *m_ctx = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    int m_channels = 0;
    int m_frameSize = 0;
    int m_delay = 0;
    // The first encode call must start at sample 0; the pre-roll identity
    // depends on it rather than on filtering whatever packet is negative.
    bool m_firstEncode = true;
    // Fallback pts for encoders that omit it; also used by flush packets.
    int64_t m_lastSampleIndex = 0;
};

} // namespace VideoEditor
