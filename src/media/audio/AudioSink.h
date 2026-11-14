#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <QString>
#include <QThread>

namespace VideoEditor {

// Abstraction over the audio output device so playback can be unit-tested
// without hardware. All methods except playedFrames()/sampleRate()/channels()
// are called on the audio thread only.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    // Prepares the device for playback. Returns false and fills `error` (in
    // Chinese, user facing) when the device cannot be opened or started.
    virtual bool start(QString *error) = 0;
    virtual void stop() = 0;

    // Writes up to `frames` interleaved frames. Returns the number of frames
    // accepted (0 when the device buffer is full; call again later).
    virtual int write(const float *samples, int frames) = 0;

    virtual int sampleRate() const = 0;
    virtual int channels() const = 0;

    // Total frames the device has actually played since it was created. The
    // engine subtracts a baseline captured at play/seek start.
    virtual int64_t playedFrames() const = 0;
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
