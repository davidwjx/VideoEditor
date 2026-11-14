#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QString>

#include "AudioSink.h"

// Windows COM interfaces are only needed by the implementation; keeping them
// out of this header lets tests and other consumers include it freely.
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

namespace VideoEditor {

// Converts interleaved float samples to 16-bit PCM (clamped to [-1, 1] and
// rounded to nearest). Exposed for unit tests; used by the PCM16 fallback.
void floatToPcm16(const float *samples, std::int16_t *out, std::size_t count);

// WASAPI shared-mode render sink at 48 kHz stereo. Creation negotiates IEEE
// float first (AUTOCONVERTPCM + SRC), then retries 16-bit PCM with the same
// conversion flags and explicit float->int16 conversion in write(). Any failure
// returns nullptr plus a Chinese error message so the engine can continue with
// a silent fallback.
class WasapiOutput : public IAudioSink {
public:
    ~WasapiOutput() override;

    WasapiOutput(const WasapiOutput &) = delete;
    WasapiOutput &operator=(const WasapiOutput &) = delete;

    static std::unique_ptr<WasapiOutput> create(int sampleRate, int channels,
                                                QString *error);

    bool start(QString *error) override;
    void stop() override;
    void flush() override;
    int availableFrames() const override;
    int write(const float *samples, int frames) override;
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override;
    bool deviceLost() const override { return m_deviceLost.load(); }

private:
    WasapiOutput() = default;
    void release();
    void noteIoFailure() const;
    void noteIoSuccess() const;

    IMMDevice *m_device = nullptr;
    IAudioClient *m_client = nullptr;
    IAudioRenderClient *m_render = nullptr;
    std::uint32_t m_bufferFrames = 0;
    int m_sampleRate = 48000;
    int m_channels = 2;
    bool m_pcm16 = false;
    bool m_comInitialized = false;
    std::atomic<int64_t> m_writtenFrames{0};
    std::atomic_bool m_started{false};
    // Mutable because the failure tracker is updated from const observers.
    mutable std::atomic<int> m_ioFailures{0};
    mutable std::atomic_bool m_deviceLost{false};
    std::vector<std::int16_t> m_converted;
};

} // namespace VideoEditor
