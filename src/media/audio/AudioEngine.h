#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include "AudioClock.h"
#include "AudioSink.h"
#include "Project.h"
#include "TimelineMixer.h"

namespace VideoEditor {

// Owns the audio thread, the 48 kHz mixer, the output sink and the master
// clock. The object lives on its audio thread: GUI-side calls are forwarded
// with queued invocations, while positionUs()/hasOutputDevice()/
// lastDeviceError() are safe to read from any thread. The audio clock is the
// master clock; video presentation follows it.
class AudioEngine : public QObject {
    Q_OBJECT

public:
    using SinkFactory =
        std::function<std::unique_ptr<IAudioSink>(int sampleRate, int channels,
                                                  QString *error)>;

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    // Lock-free master clock reads, valid from the GUI/video thread.
    int64_t positionUs() const { return m_clock.positionUs(); }
    bool hasOutputDevice() const { return m_hasOutputDevice.load(); }
    QString lastDeviceError() const;

    // Replaces the sink immediately (blocking) so tests can run without audio
    // hardware. Safe to call right after construction.
    void setSinkFactoryForTest(SinkFactory factory);

public slots:
    void setProject(const Project &project);
    void play(int64_t positionUs);
    void pause();
    void seek(int64_t positionUs);
    void shutdown();

signals:
    void positionChanged(qint64 positionUs);
    void deviceFailed(const QString &error);

private:
    void dispatchToAudioThread(std::function<void()> work, bool blocking);
    void replaceSink(SinkFactory factory, QString *error);
    void setDeviceError(const QString &error);
    void clearDeviceError();
    void onRenderTimer();

    QThread m_thread;
    QTimer *m_renderTimer = nullptr;
    TimelineMixer m_mixer;
    std::unique_ptr<IAudioSink> m_sink;
    SinkFactory m_sinkFactory;
    AudioClock m_clock;

    mutable QMutex m_errorMutex;
    QString m_deviceError;
    std::atomic_bool m_hasOutputDevice{false};
    std::atomic<int64_t> m_pendingSeekUs{-1};

    bool m_playing = false;
    int64_t m_playStartUs = 0;
    int64_t m_sinkBaseFrames = 0;
    int m_notifyCounter = 0;
    std::vector<float> m_block;
};

} // namespace VideoEditor
