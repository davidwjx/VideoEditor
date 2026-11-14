#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include "MediaInfo.h"
#include "VideoPreviewItem.h"

namespace VideoEditor {

class DecoderWorker;

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

public:
    explicit PreviewController(QObject *parent = nullptr);
    ~PreviewController() override;

    VideoPreviewItem *item() const { return m_item; }
    void setItem(VideoPreviewItem *item);

    bool playing() const { return m_playing; }
    qint64 positionMs() const { return m_positionUs / 1000; }
    qint64 durationMs() const { return m_durationUs / 1000; }
    QString timecode() const;
    QString decodeMode() const { return m_decodeMode; }
    QString mediaName() const { return m_mediaName; }

    Q_INVOKABLE void openMedia(const QString &path,
                               const QString &proxyPath = {});
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekMs(qint64 ms);
    Q_INVOKABLE void stepFrames(int delta);

signals:
    void itemChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void decodeModeChanged();
    void mediaChanged();
    void framePresented(qint64 ptsUs);
    void error(QString message);

private:
    void onOpened(const MediaInfo &info, bool hardware,
                  const QString &fallbackReason);
    void onFrameReady(int slot, const QSize &size, int rotation, qint64 ptsUs,
                      quintptr h0, quintptr h1, quintptr h2);
    void onFramePresented(qint64 ptsUs);
    void onFinished();
    void onError(const QString &message);
    void requestNextFrame();

    QThread m_thread;
    DecoderWorker *m_worker = nullptr;
    QPointer<VideoPreviewItem> m_item;
    QTimer m_frameTimer;
    MediaInfo m_info;
    QString m_mediaName;
    QString m_decodeMode;
    qint64 m_positionUs = 0;
    qint64 m_durationUs = 0;
    bool m_playing = false;
    bool m_framePending = false;
    bool m_atEnd = false;
};

} // namespace VideoEditor
