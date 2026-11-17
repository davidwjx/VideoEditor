#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include "FrameRequestCoalescer.h"
#include "LiveRepairProvider.h"
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
    Q_PROPERTY(bool useProxy READ useProxy NOTIFY useProxyChanged)
    // Observable removal compare state, also driven by RemovalController.
    Q_PROPERTY(QString removalCompareMode READ removalCompareMode
                   NOTIFY removalCompareModeChanged)
    Q_PROPERTY(int removalSplitPermille READ removalSplitPermille
                   NOTIFY removalCompareModeChanged)

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
    bool useProxy() const { return m_useProxy; }
    QString removalCompareMode() const { return m_removalCompareMode; }
    int removalSplitPermille() const { return m_removalSplitPermille; }

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
    // Compare the cached repair over the original frame. mode is
    // "repair" | "original" | "split"; an unknown value means "repair".
    Q_INVOKABLE void setRemovalCompareMode(const QString &mode,
                                           int splitPermille = 500);
    // The render worker opens repair manifests below this directory.
    void setRemovalCacheRoot(const QString &root);
    // Preview-only GPU repair source forwarded to the render worker. nullptr
    // removes it; export never receives this provider.
    void setLiveRepairProvider(LiveRepairProviderPtr provider);
    // Re-composes the paused instant (for example when a live patch arrived).
    void refreshTimelineFrame();

    // Timeline mode entry point. Takes ownership of its own rendering thread;
    // the caller keeps using play()/pause()/seekFrame() as usual. useProxy is
    // forwarded to the render worker for every decoder it opens.
    void openTimeline(const Project &project, qint64 durationUs,
                      bool useProxy = true);
    // Pushes an edited project snapshot to the compositor and audio mixer
    // without resetting the playhead or interrupting playback.
    void updateTimelineProject(const Project &project);
    // Re-opens timeline sources with/without proxies. Only affects timeline
    // mode; asset mode is handled by AppController::openAsset().
    void setUseProxy(bool enabled);
    const Project &timelineProject() const { return m_timelineProject; }
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
    void useProxyChanged();
    void removalCompareModeChanged();
    void framePresented(qint64 ptsUs);
    // Emitted after a timeline-composited frame has been presented by the
    // scene graph, for self-tests and performance gates.
    void timelineFramePresented(qint64 timelineUs);
    // Presented-frame content evidence: how many video layers were actually
    // decoded and drawn into the frame that was just presented. Test-only;
    // no QML binding depends on it.
    void timelineFrameLayersPresented(int layerCount);
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
    void onAssetError(const QString &message, quint64 generation);
    void requestNextFrame();

    void onTimelineFrameReady(int slot, const QSize &size, qint64 timelineUs,
                              quintptr h0, quintptr h1, quintptr h2,
                              int layerCount, quint64 generation);
    void onTimelineError(const QString &message);
    void onTimelineFrameSkipped(qint64 timelineUs, quint64 generation);
    void onTimelineTick();
    void requestTimelineFrame(qint64 timelineUs);
    void dispatchTimelineFrame(int64_t timelineUs);
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
    FrameRequestCoalescer m_timelineRequests;
    Rational m_timelineFps{30, 1};
    int64_t m_playStartUs = 0;
    bool m_useProxy = true;
    QString m_removalCacheRoot;
    LiveRepairProviderPtr m_liveRepairProvider;
    QString m_removalCompareMode = QStringLiteral("repair");
    int m_removalSplitPermille = 500;
    int64_t m_lastTimelineFrame = -1;
    qint64 m_lastTimelineSubmittedUs = -1;
    int m_lastTimelineLayerCount = 0;
    // Newest position requested from the compositor. Older completions must
    // not pull the paused playhead backwards after a seek/edit.
    qint64 m_requestedTimelineUs = -1;
};

} // namespace VideoEditor
