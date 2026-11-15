#include "PreviewController.h"

#include <QFileInfo>
#include <QMetaObject>

#include "AudioEngine.h"
#include "DecoderWorker.h"
#include "TimeBase.h"
#include "TimelineEvaluator.h"
#include "TimelineRenderWorker.h"
#include "VideoPreviewItem.h"

namespace VideoEditor {

namespace {

int frameIntervalMs(Rational fps)
{
    if (!fps.isValid())
        return 33;
    return qMax(1, int(1000.0 * fps.den / fps.num));
}

} // namespace

PreviewController::PreviewController(QObject *parent)
    : QObject(parent)
    , m_worker(new DecoderWorker)
{
    qRegisterMetaType<VideoEditor::Project>("VideoEditor::Project");

    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &DecoderWorker::opened, this, &PreviewController::onOpened);
    connect(m_worker, &DecoderWorker::frameReady, this,
            &PreviewController::onFrameReady);
    connect(m_worker, &DecoderWorker::error, this,
            &PreviewController::onAssetError);
    connect(m_worker, &DecoderWorker::finished, this,
            &PreviewController::onFinished);
    m_thread.start();

    // The timeline compositor has its own D3D11 device and thread; keeping it
    // off the asset decoder thread means neither can stall the other.
    m_timelineWorker = new TimelineRenderWorker;
    m_timelineWorker->moveToThread(&m_timelineThread);
    connect(&m_timelineThread, &QThread::finished, m_timelineWorker,
            &QObject::deleteLater);
    connect(m_timelineWorker, &TimelineRenderWorker::frameReady, this,
            &PreviewController::onTimelineFrameReady);
    connect(m_timelineWorker, &TimelineRenderWorker::error, this,
            &PreviewController::onTimelineError);
    connect(m_timelineWorker, &TimelineRenderWorker::frameSkipped, this,
            &PreviewController::onTimelineFrameSkipped);
    m_timelineThread.setObjectName(QStringLiteral("TimelineRender"));
    m_timelineThread.start();

    m_frameTimer.setSingleShot(false);
    connect(&m_frameTimer, &QTimer::timeout, this,
            &PreviewController::requestNextFrame);

    m_presentationTimer.setSingleShot(false);
    m_presentationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_presentationTimer, &QTimer::timeout, this,
            &PreviewController::onTimelineTick);
}

PreviewController::~PreviewController()
{
    m_frameTimer.stop();
    m_presentationTimer.stop();

    if (m_timelineThread.isRunning()) {
        // Stop the worker before its D3D11 resources outlive the thread.
        QMetaObject::invokeMethod(m_timelineWorker, "close",
                                  Qt::BlockingQueuedConnection);
        m_timelineThread.quit();
        m_timelineThread.wait();
    }

    m_thread.quit();
    m_thread.wait();
}

void PreviewController::setItem(VideoPreviewItem *item)
{
    if (m_item == item)
        return;
    if (m_item)
        disconnect(m_item, nullptr, this, nullptr);
    m_item = item;
    if (m_item) {
        connect(m_item, &VideoPreviewItem::framePresented,
                this, &PreviewController::framePresented,
                Qt::QueuedConnection);
        connect(m_item, &VideoPreviewItem::framePresented,
                this, &PreviewController::onFramePresented,
                Qt::QueuedConnection);
        connect(m_item, &VideoPreviewItem::errorOccurred,
                this, &PreviewController::onError, Qt::QueuedConnection);
    }
    emit itemChanged();
}

void PreviewController::openMedia(const QString &path, const QString &proxyPath)
{
    pause();
    if (m_mode == Mode::Timeline) {
        m_presentationTimer.stop();
        if (m_timelineWorker)
            QMetaObject::invokeMethod(m_timelineWorker, "close",
                                      Qt::QueuedConnection);
        m_mode = Mode::Asset;
        m_timelineProject = Project{};
        m_lastTimelineFrame = -1;
        m_lastTimelineSubmittedUs = -1;
        m_requestedTimelineUs = -1;
        m_timelineRequests.reset();
    }

    const quint64 generation = ++m_generation;
    if (m_item)
        m_item->resetBridge();
    m_info = MediaInfo{};
    m_positionUs = 0;
    m_durationUs = 0;
    m_mediaName = QFileInfo(path).fileName();
    m_decodeMode.clear();
    m_framePending = false;
    m_atEnd = false;
    emit positionChanged();
    emit durationChanged();
    emit mediaChanged();
    emit decodeModeChanged();

    QMetaObject::invokeMethod(m_worker, "open", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(QString, proxyPath),
                              Q_ARG(quint64, generation));
}

void PreviewController::closeMedia()
{
    pause();
    m_frameTimer.stop();
    if (m_mode == Mode::Timeline) {
        m_presentationTimer.stop();
        if (m_timelineWorker)
            QMetaObject::invokeMethod(m_timelineWorker, "close",
                                      Qt::QueuedConnection);
        m_mode = Mode::Asset;
        m_timelineProject = Project{};
        m_lastTimelineFrame = -1;
        m_lastTimelineSubmittedUs = -1;
        m_requestedTimelineUs = -1;
        m_timelineRequests.reset();
    }

    // Bump before clearing anything so any frame the worker already emitted
    // for the previous media is treated as stale.
    ++m_generation;
    m_info = MediaInfo{};
    m_positionUs = 0;
    m_durationUs = 0;
    m_mediaName.clear();
    m_decodeMode.clear();
    m_framePending = false;
    m_atEnd = false;
    if (m_item)
        m_item->resetBridge();
    emit positionChanged();
    emit durationChanged();
    emit mediaChanged();
    emit decodeModeChanged();

    QMetaObject::invokeMethod(m_worker, "close", Qt::QueuedConnection);
}

void PreviewController::play()
{
    if (m_playing)
        return;

    if (m_mode == Mode::Timeline) {
        if (m_durationUs > 0 && m_positionUs >= m_durationUs) {
            m_positionUs = 0;
            m_lastTimelineFrame = -1;
        }
        m_atEnd = false;

        if (m_audioEngine && m_audioEngine->hasOutputDevice()) {
            // AudioEngine lives on its own thread; never call its slots
            // directly. The audio clock becomes the master clock.
            QMetaObject::invokeMethod(m_audioEngine, "setProject",
                                      Qt::QueuedConnection,
                                      Q_ARG(VideoEditor::Project,
                                            m_timelineProject));
            QMetaObject::invokeMethod(m_audioEngine, "play",
                                      Qt::QueuedConnection,
                                      Q_ARG(qint64, m_positionUs));
        } else {
            // No output device: silent preview advanced by the wall clock.
            m_playStartUs = m_positionUs;
            m_wallClock.restart();
        }

        m_playing = true;
        m_lastTimelineFrame = -1;
        m_presentationTimer.start(frameIntervalMs(m_timelineFps));
        emit playingChanged();
        onTimelineTick();
        return;
    }

    if (!m_info.supported)
        return;
    // Pressing play on the last frame restarts from the beginning; the seek
    // also clears the decoder's EOF state and queues the first frame.
    if (m_atEnd)
        seekMs(0);
    m_playing = true;
    m_frameTimer.setInterval(frameIntervalMs(
        m_info.frameRate.isValid() ? m_info.frameRate : Rational{30, 1}));
    m_frameTimer.start();
    emit playingChanged();
    requestNextFrame();
}

void PreviewController::pause()
{
    if (!m_playing)
        return;

    if (m_mode == Mode::Timeline) {
        if (m_audioEngine)
            m_audioEngine->pause();
        m_presentationTimer.stop();
        const int64_t current = currentTimelineUs();
        const qint64 clamped = qBound<qint64>(
            0, current, m_durationUs > 0 ? m_durationUs : current);
        if (clamped != m_positionUs) {
            m_positionUs = clamped;
            emit positionChanged();
        }
    } else {
        m_frameTimer.stop();
    }

    m_playing = false;
    emit playingChanged();
}

void PreviewController::togglePlay()
{
    m_playing ? pause() : play();
}

void PreviewController::seekMs(qint64 ms)
{
    if (m_mode == Mode::Timeline) {
        seekFrame(microsToFrames(qMax<qint64>(0, ms) * 1000, m_timelineFps));
        return;
    }

    m_atEnd = false;
    m_positionUs = qMax<qint64>(0, ms) * 1000;
    emit positionChanged();
    QMetaObject::invokeMethod(m_worker, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, m_positionUs));
    m_framePending = false;
    requestNextFrame();
}

void PreviewController::stepFrames(int delta)
{
    if (m_mode == Mode::Timeline) {
        pause();
        seekFrame(microsToFrames(m_positionUs, m_timelineFps) + delta);
        return;
    }

    pause();
    const Rational fps = m_info.frameRate.isValid() ? m_info.frameRate
                                                     : Rational{30, 1};
    const int64_t frameUs = framesToMicros(1, fps);
    const qint64 target = qMax<qint64>(0, m_positionUs + delta * frameUs);
    seekMs(target / 1000);
}

void PreviewController::seekFrame(qint64 frame)
{
    if (m_mode != Mode::Timeline) {
        const Rational fps = m_info.frameRate.isValid() ? m_info.frameRate
                                                         : Rational{30, 1};
        seekMs(framesToMicros(qMax<qint64>(0, frame), fps) / 1000);
        return;
    }

    qint64 target = framesToMicros(qMax<qint64>(0, frame), m_timelineFps);
    if (m_durationUs > 0)
        target = qMin(target, m_durationUs);

    const bool wasPlaying = m_playing;
    if (wasPlaying && m_audioEngine)
        m_audioEngine->pause();
    if (m_audioEngine)
        m_audioEngine->seek(target);

    const bool positionChangedNow = target != m_positionUs;
    m_positionUs = target;
    m_playStartUs = target;
    m_wallClock.restart();
    m_atEnd = false;
    m_lastTimelineFrame = microsToFrames(target, m_timelineFps);
    m_lastTimelineSubmittedUs = -1;
    // Do not reset the gate: an in-flight frame completes first and then
    // dispatches this latest target, so requests stay single-in-flight.
    if (positionChangedNow)
        emit positionChanged();

    if (wasPlaying) {
        if (m_audioEngine && m_audioEngine->hasOutputDevice()) {
            QMetaObject::invokeMethod(m_audioEngine, "play",
                                      Qt::QueuedConnection,
                                      Q_ARG(qint64, target));
        } else {
            m_playStartUs = target;
            m_wallClock.restart();
        }
    }

    requestTimelineFrame(target);
}

void PreviewController::openTimeline(const Project &project, qint64 durationUs)
{
    pause();
    if (m_item)
        m_item->resetBridge();

    const quint64 generation = ++m_generation;
    m_mode = Mode::Timeline;
    m_timelineProject = project;
    m_timelineFps = project.timeline.fps.isValid() ? project.timeline.fps
                                                     : Rational{30, 1};
    m_durationUs = durationUs > 0 ? durationUs
                                  : TimelineEvaluator(project).durationUs();
    m_positionUs = 0;
    m_playStartUs = 0;
    m_lastTimelineFrame = -1;
    m_lastTimelineSubmittedUs = -1;
    m_requestedTimelineUs = -1;
    m_timelineRequests.reset();
    m_atEnd = false;
    m_framePending = false;
    m_mediaName = project.name;
    m_decodeMode = QStringLiteral("时间线 D3D11 合成");
    m_info = MediaInfo{};
    m_wallClock.invalidate();
    m_presentationTimer.stop();
    m_frameTimer.stop();
    // Release the asset decoder and its device while the timeline owns the
    // preview; entering timeline mode is a full preview switch.
    QMetaObject::invokeMethod(m_worker, "close", Qt::QueuedConnection);

    emit positionChanged();
    emit durationChanged();
    emit mediaChanged();
    emit decodeModeChanged();

    if (m_timelineWorker) {
        QMetaObject::invokeMethod(m_timelineWorker, "setProject",
                                  Qt::QueuedConnection,
                                  Q_ARG(VideoEditor::Project, m_timelineProject),
                                  Q_ARG(QString, QString()),
                                  Q_ARG(quint64, generation));
    }
    requestTimelineFrame(0);
}

void PreviewController::updateTimelineProject(const Project &project)
{
    if (m_mode != Mode::Timeline)
        return;

    m_timelineProject = project;
    m_timelineFps = project.timeline.fps.isValid() ? project.timeline.fps
                                                     : Rational{30, 1};

    const qint64 duration = TimelineEvaluator(project).durationUs();
    const bool durationChangedNow = duration != m_durationUs;
    m_durationUs = duration;
    if (m_durationUs >= 0 && m_positionUs > m_durationUs) {
        m_positionUs = m_durationUs;
        emit positionChanged();
    }
    if (durationChangedNow)
        emit durationChanged();

    // Reuse the current generation: the in-flight coalescer gate is only
    // released for the generation it was opened with.
    if (m_timelineWorker) {
        QMetaObject::invokeMethod(m_timelineWorker, "setProject",
                                  Qt::QueuedConnection,
                                  Q_ARG(VideoEditor::Project, project),
                                  Q_ARG(QString, QString()),
                                  Q_ARG(quint64, m_generation));
    }
    if (m_audioEngine) {
        QMetaObject::invokeMethod(m_audioEngine, "setProject",
                                  Qt::QueuedConnection,
                                  Q_ARG(VideoEditor::Project, project));
    }

    // Recompose the current instant so a paused edit is visible immediately;
    // while playing the presentation timer picks up the new last frame too.
    m_lastTimelineFrame = -1;
    m_lastTimelineSubmittedUs = -1;
    requestTimelineFrame(m_positionUs);
}

void PreviewController::setAudioEngine(AudioEngine *engine)
{
    m_audioEngine = engine;
}

void PreviewController::onOpened(const MediaInfo &info, bool hardware,
                                 const QString &fallbackReason,
                                 quint64 generation)
{
    if (generation != m_generation)
        return;
    m_info = info;
    m_durationUs = info.durationUs;

    QString mode = hardware ? QStringLiteral("硬件 D3D11VA")
                            : QStringLiteral("软件解码");
    if (!hardware && !fallbackReason.isEmpty())
        mode += QStringLiteral("（%1）").arg(fallbackReason);
    m_decodeMode = mode;

    m_frameTimer.setInterval(frameIntervalMs(
        info.frameRate.isValid() ? info.frameRate : Rational{30, 1}));

    emit durationChanged();
    emit decodeModeChanged();
    // The decoder already delivered the first frame in open(); do not request
    // another one here or the initial display can skip frame 0.
}

void PreviewController::onFrameReady(int slot, const QSize &size, int rotation,
                                     qint64 ptsUs, quintptr h0, quintptr h1,
                                     quintptr h2, quint64 generation)
{
    if (generation != m_generation || m_mode != Mode::Asset) {
        // The frame belongs to a media that is no longer open. Release its
        // ring slot so the old producer/ring teardown is never left waiting.
        if (m_item)
            m_item->discardFrame(slot);
        return;
    }

    m_positionUs = ptsUs;
    m_atEnd = false;
    emit positionChanged();

    if (!m_item) {
        m_framePending = false;
        return;
    }

    // The frame is not done until the scene graph presents it: a slot that is
    // produced but never presented holds its keyed mutex and can stall the
    // producer once the ring wraps.
    m_framePending = true;
    m_item->setSharedHandles({h0, h1, h2});
    m_item->submitFrame(slot, size, rotation, ptsUs);
}

void PreviewController::onTimelineFrameReady(int slot, const QSize &size,
                                             qint64 timelineUs, quintptr h0,
                                             quintptr h1, quintptr h2,
                                             quint64 generation)
{
    if (generation != m_generation || m_mode != Mode::Timeline) {
        if (m_item)
            m_item->discardFrame(slot);
        return;
    }

    // One frame in flight: completion releases the gate and, at most, requests
    // the latest target that arrived while this frame was rendering.
    const std::optional<int64_t> next =
        m_timelineRequests.complete(timelineUs);

    // A frame requested before the latest seek/edit must not move the paused
    // playhead back to its older timestamp; the coalescer will dispatch the
    // newest request next.
    if (!m_playing && timelineUs == m_requestedTimelineUs) {
        const qint64 clamped = qBound<qint64>(
            0, timelineUs, m_durationUs > 0 ? m_durationUs : timelineUs);
        if (clamped != m_positionUs) {
            m_positionUs = clamped;
            emit positionChanged();
        }
    }

    if (m_item) {
        m_lastTimelineSubmittedUs = timelineUs;
        m_item->setSharedHandles({h0, h1, h2});
        m_item->submitFrame(slot, size, 0, timelineUs);
    }

    if (next)
        dispatchTimelineFrame(*next);
}

void PreviewController::onTimelineFrameSkipped(qint64 timelineUs,
                                                quint64 generation)
{
    if (generation != m_generation || m_mode != Mode::Timeline)
        return;
    // The worker dropped this request (ring slot still held by the scene
    // graph). Release the gate; do not retry the same instant immediately.
    if (const std::optional<int64_t> next =
            m_timelineRequests.complete(timelineUs)) {
        dispatchTimelineFrame(*next);
    }
}

void PreviewController::onFramePresented(qint64 pts)
{
    m_framePending = false;
    if (m_mode == Mode::Timeline && pts == m_lastTimelineSubmittedUs)
        emit timelineFramePresented(pts);
}

void PreviewController::onFinished()
{
    m_framePending = false;
    m_atEnd = true;
    pause();
}

void PreviewController::onError(const QString &message)
{
    m_framePending = false;
    pause();
    emit error(message);
}

void PreviewController::onAssetError(const QString &message,
                                     quint64 generation)
{
    // An error from the asset decoder after the controller already switched
    // media or mode belongs to the previous preview and must be ignored.
    if (generation != m_generation || m_mode != Mode::Asset)
        return;
    onError(message);
}

void PreviewController::onTimelineError(const QString &message)
{
    // A missing clip or a failed composition must not stop preview: the
    // remaining layers keep rendering and the error is surfaced once.
    emit error(message);
}

void PreviewController::requestNextFrame()
{
    if (m_mode != Mode::Asset || !m_info.supported || m_framePending)
        return;
    m_framePending = true;
    QMetaObject::invokeMethod(m_worker, "requestFrame", Qt::QueuedConnection);
}

int64_t PreviewController::currentTimelineUs() const
{
    if (m_mode != Mode::Timeline)
        return m_positionUs;
    if (m_audioEngine && m_audioEngine->hasOutputDevice())
        return m_audioEngine->positionUs();
    if (m_playing && m_wallClock.isValid())
        return m_playStartUs + m_wallClock.elapsed() * 1000;
    return m_positionUs;
}

void PreviewController::onTimelineTick()
{
    if (m_mode != Mode::Timeline) {
        m_presentationTimer.stop();
        return;
    }

    // If the output device disappears mid-playback, fall back to the wall
    // clock anchored at the last known position instead of freezing.
    if (m_playing && !(m_audioEngine && m_audioEngine->hasOutputDevice())
        && !m_wallClock.isValid()) {
        m_playStartUs = m_positionUs;
        m_wallClock.restart();
    }

    const int64_t current = currentTimelineUs();
    const int64_t limit = m_durationUs > 0 ? m_durationUs : current;
    const qint64 clamped = qBound<qint64>(0, current, limit);
    if (clamped != m_positionUs) {
        m_positionUs = clamped;
        emit positionChanged();
    }

    if (m_playing && m_durationUs > 0 && current >= m_durationUs) {
        m_atEnd = true;
        pause();
        // Compose the last real frame so the preview does not freeze on a
        // stale picture.
        const qint64 lastFrame =
            qMax<qint64>(0, microsToFrames(m_durationUs, m_timelineFps) - 1);
        m_lastTimelineFrame = lastFrame;
        requestTimelineFrame(framesToMicros(lastFrame, m_timelineFps));
        return;
    }

    const qint64 frame = microsToFrames(m_positionUs, m_timelineFps);
    if (frame != m_lastTimelineFrame) {
        m_lastTimelineFrame = frame;
        requestTimelineFrame(framesToMicros(frame, m_timelineFps));
    }
}

void PreviewController::requestTimelineFrame(qint64 timelineUs)
{
    if (!m_timelineWorker)
        return;
    m_requestedTimelineUs = timelineUs;
    // While a frame is in flight, request() only records the newest target;
    // the completion path dispatches it. Stale intermediate frames never hit
    // the worker queue.
    if (const std::optional<int64_t> dispatch =
            m_timelineRequests.request(timelineUs)) {
        dispatchTimelineFrame(*dispatch);
    }
}

void PreviewController::dispatchTimelineFrame(int64_t timelineUs)
{
    QMetaObject::invokeMethod(m_timelineWorker, "renderFrame",
                              Qt::QueuedConnection,
                              Q_ARG(qint64, qint64(timelineUs)),
                              Q_ARG(quint64, m_generation));
}

QString PreviewController::timecode() const
{
    if (m_mode == Mode::Timeline) {
        return formatTimecode(m_positionUs, m_timelineFps);
    }
    return formatTimecode(m_positionUs,
                          m_info.frameRate.isValid() ? m_info.frameRate
                                                     : Rational{30, 1});
}

} // namespace VideoEditor
