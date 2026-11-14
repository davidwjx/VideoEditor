#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

#include <QString>
#include <QThread>

namespace VideoEditor {

// Abstraction over the audio output device so playback can be unit-tested
// without hardware. All methods except the const observers are called on the
// audio thread only.
//
// Format contract: sinks are created for 48 kHz and receive interleaved float
// stereo blocks no larger than availableFrames(). Implementations may accept
// fewer frames than requested (the engine then carries the remainder), but
// they must never require the engine to guess the device buffer size.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    // Prepares the device for playback. Returns false and fills `error` (in
    // Chinese, user facing) when the device cannot be opened or started.
    virtual bool start(QString *error) = 0;
    virtual void stop() = 0;

    // Discards frames queued but not yet played. Called after stop() and
    // before every restart so stale audio is never replayed after a pause or
    // seek. WASAPI implements this with IAudioClient::Reset(); discarded frames
    // are reported as played because the device padding drops to zero.
    virtual void flush() {}

    // Frames that can be written without blocking. The engine renders at most
    // this many frames per tick, so nothing is rendered only to be dropped.
    virtual int availableFrames() const
    {
        return std::numeric_limits<int>::max();
    }

    // Writes up to `frames` interleaved frames. Returns the number of frames
    // accepted (0 when the device buffer is full).
    virtual int write(const float *samples, int frames) = 0;

    virtual int sampleRate() const = 0;
    virtual int channels() const = 0;

    // Total frames the device has actually played since it was created. The
    // engine subtracts a baseline captured after flush/restart.
    virtual int64_t playedFrames() const = 0;

    // True after repeated device I/O failures. The engine then clears
    // hasOutputDevice() and falls back to the silent wall clock.
    virtual bool deviceLost() const { return false; }
};

// Silent sink used when no device is available and by tests. It discards the
// samples but still counts them, so the engine's clock math is exercised. With
// setPaceRealTime(true) write() sleeps for the block duration, which makes it
// usable as a wall-clock stand-in; tests keep the default (off) to stay fast.
class NullAudioSink : public IAudioSink {
public:
    explicit NullAudioSink(int sampleRate = 48000, int channels = 2)
        : m_sampleRate(sampleRate > 0 ? sampleRate : 48000)
        , m_channels(channels > 0 ? channels : 2)
    {
    }

    void setPaceRealTime(bool enabled) { m_paceRealTime.store(enabled); }

    bool start(QString *) override
    {
        m_running.store(true);
        return true;
    }

    void stop() override { m_running.store(false); }

    // The sink has no internal buffer, so every written frame is already
    // "played"; flush is intentionally a no-op.
    void flush() override {}

    int write(const float *, int frames) override
    {
        if (frames <= 0)
            return 0;
        m_playedFrames.fetch_add(frames);
        if (m_paceRealTime.load())
            QThread::msleep(static_cast<unsigned long>(int64_t(frames) * 1000
                                                       / m_sampleRate));
        return frames;
    }

    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override { return m_playedFrames.load(); }

    bool running() const { return m_running.load(); }

private:
    int m_sampleRate;
    int m_channels;
    std::atomic<int64_t> m_playedFrames{0};
    std::atomic_bool m_running{false};
    std::atomic_bool m_paceRealTime{false};
};

} // namespace VideoEditor
