#include "AudioEngine.h"

#include <algorithm>
#include <utility>

#include "WasapiOutput.h"

namespace VideoEditor {

namespace {

// M2 global constraints: fixed 48 kHz float stereo, 10 ms blocks.
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockFrames = kSampleRate / 100;
constexpr int kTimerIntervalMs = 10;
constexpr int kNotifyEveryTicks = 3; // ~30 Hz position notifications

std::unique_ptr<IAudioSink> createDefaultSink(int sampleRate, int channels,
                                              QString *error)
{
    return WasapiOutput::create(sampleRate, channels, error);
}

} // namespace

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
    , m_mixer(kSampleRate, kChannels)
    , m_sinkFactory(createDefaultSink)
    , m_block(size_t(kBlockFrames) * size_t(kChannels), 0.0f)
{
    m_renderTimer = new QTimer(this);
    m_renderTimer->setTimerType(Qt::PreciseTimer);
    m_renderTimer->setInterval(kTimerIntervalMs);
    connect(m_renderTimer, &QTimer::timeout, this, &AudioEngine::onRenderTimer);

    m_thread.setObjectName(QStringLiteral("AudioEngine"));
    moveToThread(&m_thread);
    m_thread.start();

    // Create the device on the audio thread so COM init/uninit stay balanced.
    dispatchToAudioThread([this] { replaceSink(m_sinkFactory, nullptr); }, true);
}

AudioEngine::~AudioEngine()
{
    if (!m_thread.isFinished()) {
        if (m_thread.isRunning())
            shutdown();
        m_thread.quit();
        if (!m_thread.wait(3000))
            m_thread.terminate();
        m_thread.wait(1000);
    }
}

QString AudioEngine::lastDeviceError() const
{
    QMutexLocker locker(&m_errorMutex);
    return m_deviceError;
}

void AudioEngine::setDeviceError(const QString &error)
{
    QMutexLocker locker(&m_errorMutex);
    m_deviceError = error;
}

void AudioEngine::clearDeviceError()
{
    QMutexLocker locker(&m_errorMutex);
    m_deviceError.clear();
}

void AudioEngine::dispatchToAudioThread(std::function<void()> work, bool blocking)
{
    if (QThread::currentThread() == thread()) {
        work();
        return;
    }
    if (!m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(this, std::move(work),
                              blocking ? Qt::BlockingQueuedConnection
                                       : Qt::QueuedConnection);
}

void AudioEngine::replaceSink(SinkFactory factory, QString *error)
{
    if (m_sink) {
        m_sink->stop();
        m_sink.reset();
    }

    QString localError;
    std::unique_ptr<IAudioSink> sink;
    if (factory)
        sink = factory(kSampleRate, kChannels, &localError);

    if (!sink) {
        if (localError.isEmpty())
            localError = QStringLiteral("没有可用的音频输出设备。");
        m_hasOutputDevice.store(false);
        setDeviceError(localError);
        if (error)
            *error = localError;
        emit deviceFailed(localError);
        return;
    }

    m_sink = std::move(sink);
    m_hasOutputDevice.store(true);
    clearDeviceError();
}

void AudioEngine::setSinkFactoryForTest(SinkFactory factory)
{
    auto work = [this, factory = std::move(factory)]() mutable {
        m_sinkFactory = factory;
        replaceSink(std::move(factory), nullptr);
    };
    dispatchToAudioThread(std::move(work), true);
}

void AudioEngine::setProject(const Project &project)
{
    dispatchToAudioThread([this, project] { m_mixer.setProject(project); }, false);
}

void AudioEngine::play(int64_t positionUs)
{
    auto work = [this, positionUs] {
        if (m_playing)
            return;

        const int64_t start = std::max<int64_t>(0, positionUs);
        m_mixer.setPosition(start);
        m_clock.reset(start);
        m_playStartUs = start;
        m_pendingSeekUs.store(-1);
        m_notifyCounter = 0;

        if (!m_sink) {
            // Silent fallback: no device, no playback loop. The preview
            // controller advances by wall clock (M2 design decision 5).
            emit positionChanged(qint64(start));
            return;
        }

        QString error;
        if (!m_sink->start(&error)) {
            if (error.isEmpty())
                error = QStringLiteral("无法启动音频输出。");
            setDeviceError(error);
            emit deviceFailed(error);
            emit positionChanged(qint64(start));
            return;
        }
        m_sinkBaseFrames = m_sink->playedFrames();
        m_playing = true;
        m_renderTimer->start();
        emit positionChanged(qint64(m_clock.positionUs()));
    };
    dispatchToAudioThread(std::move(work), false);
}

void AudioEngine::pause()
{
    dispatchToAudioThread([this] {
        if (!m_playing)
            return;
        m_playing = false;
        m_renderTimer->stop();
        m_pendingSeekUs.store(-1);
        if (m_sink)
            m_sink->stop();
    }, false);
}

void AudioEngine::seek(int64_t positionUs)
{
    const int64_t target = std::max<int64_t>(0, positionUs);
    dispatchToAudioThread([this, target] {
        if (m_playing) {
            // Applied at the next block boundary so the device is never reset
            // while a write is in flight.
            m_pendingSeekUs.store(target);
            return;
        }
        m_mixer.setPosition(target);
        m_clock.reset(target);
        m_playStartUs = target;
        m_sinkBaseFrames = 0;
        m_pendingSeekUs.store(-1);
        emit positionChanged(qint64(target));
    }, false);
}

void AudioEngine::shutdown()
{
    auto work = [this] {
        m_playing = false;
        if (m_renderTimer)
            m_renderTimer->stop();
        m_pendingSeekUs.store(-1);
        if (m_sink) {
            m_sink->stop();
            // Destroy on the audio thread: WASAPI/COM is thread scoped.
            m_sink.reset();
        }
        m_thread.quit();
    };
    dispatchToAudioThread(std::move(work), true);
}

void AudioEngine::onRenderTimer()
{
    if (!m_playing || !m_sink)
        return;

    const int64_t pendingSeek = m_pendingSeekUs.exchange(-1);
    if (pendingSeek >= 0) {
        m_sink->stop();
        m_mixer.setPosition(pendingSeek);
        m_clock.reset(pendingSeek);
        m_playStartUs = pendingSeek;
        m_sinkBaseFrames = 0;
        QString error;
        if (!m_sink->start(&error)) {
            if (error.isEmpty())
                error = QStringLiteral("无法启动音频输出。");
            m_playing = false;
            m_renderTimer->stop();
            setDeviceError(error);
            emit deviceFailed(error);
            return;
        }
        m_sinkBaseFrames = m_sink->playedFrames();
    }

    m_mixer.render(m_block.data(), kBlockFrames, nullptr);

    int written = 0;
    while (written < kBlockFrames) {
        const int n = m_sink->write(
            m_block.data() + size_t(written) * size_t(kChannels),
            kBlockFrames - written);
        if (n <= 0)
            break; // device buffer full; retry on the next tick
        written += n;
    }

    // Absolute, integer-only clock math: no floating accumulation and no drift.
    const int64_t consumedFrames =
        std::max<int64_t>(0, m_sink->playedFrames() - m_sinkBaseFrames);
    const int64_t consumedUs = consumedFrames * 1'000'000 / kSampleRate;
    m_clock.setPosition(std::max<int64_t>(0, m_playStartUs + consumedUs));

    if (++m_notifyCounter >= kNotifyEveryTicks) {
        m_notifyCounter = 0;
        emit positionChanged(qint64(m_clock.positionUs()));
    }
}

} // namespace VideoEditor
