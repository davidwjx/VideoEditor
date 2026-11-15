#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include "MediaInfo.h"
#include "Project.h"
#include "TimeBase.h"
#include "VideoPreviewItem.h"

namespace VideoEditor {

class AudioEngine;
class DecoderWorker;
class TimelineRenderWorker;

class PreviewController : public QObject {
    Q_OBJECT
    Q_PROPERTY(VideoEditor::VideoPreviewItem *item READ item WRITE setItem
                   NOTIFY itemChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)
    Q_PROPERTY(QString timecode READ timecode NOTIFY positionChanged)
    Q_PROPERTY(QString decodeMode READ decodeMode NOTIFY decodeModeChanged)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY mediaChanged)
    Q_PROPERTY(bool timelineMode READ timelineMode NOTIFY mediaChanged)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY durationChanged)

public:
    enum class Mode { Asset, Timeline };
    Q_ENUM(Mode)

    explicit PreviewController(QObject *parent = nullptr);
    ~PreviewController() override;

    VideoPreviewItem *item() const { return m_item; }
    void setItem(VideoPreviewItem *item);

    bool playing() const { return m_playing; }
    qint64 positionMs() const { return m_positionUs / 1000; }
    qint64 durationMs() const { return m_durationUs / 1000; }
    qint64 durationUs() const { return m_durationUs; }
    QString timecode() const;
    QString decodeMode() const { return m_decodeMode; }
    QString mediaName() const { return m_mediaName; }

    Mode mode() const { return m_mode; }
    bool timelineMode() const { return m_mode == Mode::Timeline; }

    Q_INVOKABLE void openMedia(const QString &path,
                               const QString &proxyPath = {});
    Q_INVOKABLE void closeMedia();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekMs(qint64 ms);
    Q_INVOKABLE void stepFrames(int delta);
    // Timeline mode only: seek to an absolute project frame number.
    Q_INVOKABLE void seekFrame(qint64 frame);

    // Timeline mode entry point. Takes ownership of its own rendering thread;
    // the caller keeps using play()/pause()/seekFrame() as usual.
    void openTimeline(const Project &project, qint64 durationUs);
    // AudioEngine lives on its own thread; PreviewController only forwards
    // queued calls and reads the lock-free master clock.
    void setAudioEngine(AudioEngine *engine);

signals:
    void itemChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void decodeModeChanged();
    void mediaChanged();
    void framePresented(qint64 ptsUs);
    // Emitted after a timeline-composited frame has been presented by the
    // scene graph, for self-tests and performance gates.
    void timelineFramePresented(qint64 timelineUs);
    void error(QString message);

private:
    void onOpened(const MediaInfo &info, bool hardware,
                  const QString &fallbackReason, quint64 generation);
    void onFrameReady(int slot, const QSize &size, int rotation, qint64 ptsUs,
                      quintptr h0, quintptr h1, quintptr h2,
                      quint64 generation);
    void onFramePresented(qint64 ptsUs);
    void onFinished();
    void onError(const QString &message);
    void requestNextFrame();

    void onTimelineFrameReady(int slot, const QSize &size, qint64 timelineUs,
                              quintptr h0, quintptr h1, quintptr h2,
                              quint64 generation);
    void onTimelineError(const QString &message);
    void onTimelineTick();
    void requestTimelineFrame(qint64 timelineUs);
    int64_t currentTimelineUs() const;

    QThread m_thread;
    DecoderWorker *m_worker = nullptr;
    QPointer<VideoPreviewItem> m_item;
    QTimer m_frameTimer;
    MediaInfo m_info;
    QString m_mediaName;
    QString m_decodeMode;
    qint64 m_positionUs = 0;
    qint64 m_durationUs = 0;
    // Incremented whenever the media changes; signals tagged with an older
    // token belong to the previous media and must not reach the item.
    quint64 m_generation = 0;
    bool m_playing = false;
    bool m_framePending = false;
    bool m_atEnd = false;

    Mode m_mode = Mode::Asset;
    QThread m_timelineThread;
    TimelineRenderWorker *m_timelineWorker = nullptr;
    AudioEngine *m_audioEngine = nullptr;
    Project m_timelineProject;
    QTimer m_presentationTimer;
    QElapsedTimer m_wallClock;
    QElapsedTimer m_timelinePendingSince;
    Rational m_timelineFps{30, 1};
    int64_t m_playStartUs = 0;
    int64_t m_lastTimelineFrame = -1;
    qint64 m_lastTimelineSubmittedUs = -1;
    bool m_timelineFramePending = false;
};

} // namespace VideoEditor
