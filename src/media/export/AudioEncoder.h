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
// zero-padding the final partial frame. Packet pts is the absolute sample
// index, so packets stay in timeline order for the muxer. One instance is used
// per export; not thread-safe.
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
    // frameSize(); the pipeline pads and splits its own buffers. New packets
    // are appended to `packets`.
    bool encode(const float *interleaved, int frames, int64_t sampleIndex,
                std::vector<EncodedPacket> *packets, QString *error = nullptr);

    // Signals end of stream and drains delayed packets. Safe to call once
    // after the last frame.
    bool flush(std::vector<EncodedPacket> *packets, QString *error = nullptr);

    void close();

    // Samples per encoded frame (1024 for AAC-LC).
    int frameSize() const { return m_frameSize; }
    // Encoder priming samples that a container must discard at the start.
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
};

} // namespace VideoEditor
