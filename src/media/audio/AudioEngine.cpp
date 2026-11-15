#include "AudioEngine.h"

#include <algorithm>
#include <cstring>
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

AudioEngine::AudioEngine(QObject *parent, SinkFactory sinkFactory)
    : QObject(nullptr)
    , m_mixer(kSampleRate, kChannels)
    , m_sinkFactory(sinkFactory ? std::move(sinkFactory) : createDefaultSink)
    , m_block(std::size_t(kBlockFrames) * std::size_t(kChannels), 0.0f)
    , m_carry(std::size_t(kBlockFrames) * std::size_t(kChannels), 0.0f)
{
    // `parent` is intentionally ignored: a parented object cannot be moved to
    // the audio thread.
    Q_UNUSED(parent);

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
    // Never join from inside the audio thread itself.
    if (QThread::currentThread() == &m_thread)
        return;

    // Idempotent: returns immediately if shutdown() already ran, so the
    // destructor never re-posts a blocking call to an exiting thread.
    shutdown();
    m_thread.quit();
    if (!m_thread.wait(3000)) {
        m_thread.terminate();
        m_thread.wait(1000);
    }
}

QString AudioEngine::lastDeviceError() const
{
    QMutexLocker locker(&m_errorMutex);
    return m_deviceError;
}

Project AudioEngine::projectSnapshot() const
{
    Project result;
    auto *self = const_cast<AudioEngine *>(this);
    self->dispatchToAudioThread([this, &result] {
        result = m_mixer.project();
    }, true);
    return result;
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
    // After shutdown there is no event loop left to serve the call.
    if (m_shutdownRequested.load() || !m_thread.isRunning())
        return;
    QMetaObject::invokeMethod(this, std::move(work),
                              blocking ? Qt::BlockingQueuedConnection
                                       : Qt::QueuedConnection);
}

void AudioEngine::replaceSink(SinkFactory factory, QString *error)
{
    m_playing = false;
    if (m_renderTimer)
        m_renderTimer->stop();
    m_pendingSeekUs.store(-1);
    m_carryFrames = 0;
    m_carryOffset = 0;
    if (m_sink) {
        m_sink->stop();
        m_sink->flush();
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

void AudioEngine::play(qint64 positionUs)
{
    auto work = [this, start = std::max<int64_t>(0, int64_t(positionUs))] {
        if (m_playing)
            return;

        m_mixer.setPosition(start);
        m_clock.reset(start);
        m_playStartUs = start;
        m_sinkBaseFrames = 0;
        m_pendingSeekUs.store(-1);
        m_carryFrames = 0;
        m_carryOffset = 0;
        m_notifyCounter = 0;

        if (!m_sink) {
            // Silent fallback: no device, no playback loop. The preview
            // controller advances by wall clock (M2 design decision 5).
            emit positionChanged(qint64(start));
            return;
        }

        // Discard anything queued by an earlier run before restarting.
        m_sink->flush();
        QString error;
        if (!m_sink->start(&error)) {
            failDevice(error.isEmpty() ? QStringLiteral("无法启动音频输出。")
                                       : error);
            emit positionChanged(qint64(start));
            return;
        }
        // Baseline only after flush/restart so stale frames never count.
        m_sinkBaseFrames = m_sink->playedFrames();
        m_playing = true;
        m_renderTimer->start();
        emit positionChanged(qint64(start));
    };
    dispatchToAudioThread(std::move(work), false);
}

void AudioEngine::pause()
{
    auto work = [this] {
        // Honour a seek that was queued while playing instead of discarding it.
        applyPendingSeek(false);
        if (!m_playing) {
            if (m_renderTimer)
                m_renderTimer->stop();
            emit positionChanged(qint64(m_clock.positionUs()));
            return;
        }

        m_playing = false;
        if (m_renderTimer)
            m_renderTimer->stop();
        if (m_sink) {
            m_sink->stop();
            updateClockFromSink(); // capture the final played position
            m_sink->flush();
        }
        m_pendingSeekUs.store(-1);
        m_carryFrames = 0;
        m_carryOffset = 0;
        emit positionChanged(qint64(m_clock.positionUs()));
    };
    // Blocking: callers (and tests) observe the final position immediately.
    dispatchToAudioThread(std::move(work), true);
}

void AudioEngine::seek(qint64 positionUs)
{
    const int64_t target = std::max<int64_t>(0, int64_t(positionUs));
    auto work = [this, target] {
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
        m_carryFrames = 0;
        m_carryOffset = 0;
        m_pendingSeekUs.store(-1);
        emit positionChanged(qint64(target));
    };
    dispatchToAudioThread(std::move(work), true);
}

void AudioEngine::shutdown()
{
    // Idempotent: the destructor may call shutdown() again after an explicit
    // shutdown already quit the thread. Repeated calls must never block.
    if (m_shutdownRequested.exchange(true))
        return;

    auto work = [this] {
        m_playing = false;
        if (m_renderTimer)
            m_renderTimer->stop();
        m_pendingSeekUs.store(-1);
        m_carryFrames = 0;
        m_carryOffset = 0;
        if (m_sink) {
            m_sink->stop();
            m_sink->flush();
            // Destroy on the audio thread: WASAPI/COM is thread scoped.
            m_sink.reset();
        }
        m_hasOutputDevice.store(false);
        m_thread.quit();
    };

    if (QThread::currentThread() == thread()) {
        work();
        return;
    }
    if (m_thread.isRunning())
        QMetaObject::invokeMethod(this, work, Qt::BlockingQueuedConnection);
}

bool AudioEngine::applyPendingSeek(bool restart)
{
    const int64_t pending = m_pendingSeekUs.exchange(-1);
    if (pending < 0)
        return true;

    if (m_sink)
        m_sink->stop();
    m_mixer.setPosition(pending);
    m_clock.reset(pending);
    m_playStartUs = pending;
    m_carryFrames = 0;
    m_carryOffset = 0;

    if (!m_sink) {
        m_sinkBaseFrames = 0;
        return true;
    }

    if (!restart) {
        // Pause will flush immediately afterwards; anchor the clock at the
        // seek target so the old sink's consumed frames are not re-added.
        m_sinkBaseFrames = m_sink->playedFrames();
        return true;
    }

    // Discard stale queued audio, then restart and rebase the clock.
    m_sink->flush();
    QString error;
    if (!m_sink->start(&error)) {
        m_sinkBaseFrames = 0;
        failDevice(error.isEmpty() ? QStringLiteral("无法启动音频输出。") : error);
        return false;
    }
    m_sinkBaseFrames = m_sink->playedFrames();
    return true;
}

void AudioEngine::updateClockFromSink()
{
    if (!m_sink)
        return;
    const int64_t consumedFrames =
        std::max<int64_t>(0, m_sink->playedFrames() - m_sinkBaseFrames);
    const int64_t consumedUs = consumedFrames * 1'000'000 / kSampleRate;
    m_clock.setPosition(std::max<int64_t>(0, m_playStartUs + consumedUs));
}

void AudioEngine::failDevice(const QString &error)
{
    m_playing = false;
    if (m_renderTimer)
        m_renderTimer->stop();
    m_carryFrames = 0;
    m_carryOffset = 0;
    if (m_sink)
        m_sink->stop();
    // Task 10 switches to the silent wall clock when this is false.
    m_hasOutputDevice.store(false);
    setDeviceError(error);
    emit deviceFailed(error);
}

void AudioEngine::onRenderTimer()
{
    if (!m_playing || !m_sink)
        return;

    if (!applyPendingSeek(true))
        return;

    if (m_sink->deviceLost()) {
        updateClockFromSink();
        failDevice(QStringLiteral("音频输出设备已断开。"));
        return;
    }

    // Push carried samples before rendering anything new: a device that
    // accepted only part of the previous block gets the tail next tick.
    if (m_carryFrames > m_carryOffset) {
        const int remaining = m_carryFrames - m_carryOffset;
        const int n = m_sink->write(
            m_carry.data()
                + std::size_t(m_carryOffset) * std::size_t(kChannels),
            remaining);
        if (n > 0) {
            m_carryOffset += n;
            if (m_carryOffset >= m_carryFrames) {
                m_carryFrames = 0;
                m_carryOffset = 0;
            }
        }
    }

    // Render only what the device can accept right now, so no block is ever
    // rendered just to be dropped.
    if (m_carryFrames == 0 && !m_sink->deviceLost()) {
        const int available = std::clamp(m_sink->availableFrames(), 0, kBlockFrames);
        if (available > 0) {
            m_mixer.render(m_block.data(), available, nullptr);
            const int n = std::max(0, m_sink->write(m_block.data(), available));
            if (n < available) {
                std::memcpy(
                    m_carry.data(),
                    m_block.data() + std::size_t(n) * std::size_t(kChannels),
                    std::size_t(available - n) * std::size_t(kChannels)
                        * sizeof(float));
                m_carryFrames = available - n;
                m_carryOffset = 0;
            }
        }
    }

    updateClockFromSink();

    if (m_sink->deviceLost()) {
        failDevice(QStringLiteral("音频输出设备已断开。"));
        return;
    }

    if (++m_notifyCounter >= kNotifyEveryTicks) {
        m_notifyCounter = 0;
        emit positionChanged(qint64(m_clock.positionUs()));
    }
}

} // namespace VideoEditor
