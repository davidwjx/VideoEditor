#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <QString>

#include "AudioSink.h"

// Windows COM interfaces are only needed by the implementation; keeping them
// out of this header lets tests and other consumers include it freely.
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

namespace VideoEditor {

// WASAPI shared-mode render sink at 48 kHz stereo. Creation negotiates IEEE
// float first (AUTOCONVERTPCM + SRC), then falls back to 16-bit PCM with
// explicit conversion in write(). Any failure returns nullptr plus a Chinese
// error message so the engine can continue with a silent fallback.
class WasapiOutput : public IAudioSink {
public:
    ~WasapiOutput() override;

    WasapiOutput(const WasapiOutput &) = delete;
    WasapiOutput &operator=(const WasapiOutput &) = delete;

    static std::unique_ptr<WasapiOutput> create(int sampleRate, int channels,
                                                QString *error);

    bool start(QString *error) override;
    void stop() override;
    int write(const float *samples, int frames) override;
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override;

private:
    WasapiOutput() = default;
    void release();

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
};

} // namespace VideoEditor
