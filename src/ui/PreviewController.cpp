#include "PreviewController.h"

#include <QFileInfo>
#include <QMetaObject>

#include "DecoderWorker.h"
#include "TimeBase.h"
#include "VideoPreviewItem.h"

namespace VideoEditor {

PreviewController::PreviewController(QObject *parent)
    : QObject(parent)
    , m_worker(new DecoderWorker)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &DecoderWorker::opened, this, &PreviewController::onOpened);
    connect(m_worker, &DecoderWorker::frameReady, this,
            &PreviewController::onFrameReady);
    connect(m_worker, &DecoderWorker::error, this, &PreviewController::error);
    m_thread.start();

    m_frameTimer.setSingleShot(false);
    connect(&m_frameTimer, &QTimer::timeout, this,
            &PreviewController::requestNextFrame);
}

PreviewController::~PreviewController()
{
    m_frameTimer.stop();
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
    }
    emit itemChanged();
}

void PreviewController::openMedia(const QString &path, const QString &proxyPath)
{
    pause();
    if (m_item)
        m_item->resetBridge();
    m_info = MediaInfo{};
    m_positionUs = 0;
    m_durationUs = 0;
    m_mediaName = QFileInfo(path).fileName();
    m_decodeMode.clear();
    m_framePending = false;
    emit positionChanged();
    emit durationChanged();
    emit mediaChanged();
    emit decodeModeChanged();

    QMetaObject::invokeMethod(m_worker, "open", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(QString, proxyPath));
}

void PreviewController::play()
{
    if (!m_info.supported || m_playing)
        return;
    m_playing = true;
    if (m_frameTimer.interval() <= 0)
        m_frameTimer.setInterval(33);
    m_frameTimer.start();
    emit playingChanged();
    requestNextFrame();
}

void PreviewController::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_frameTimer.stop();
    emit playingChanged();
}

void PreviewController::togglePlay()
{
    m_playing ? pause() : play();
}

void PreviewController::seekMs(qint64 ms)
{
    m_positionUs = qMax<qint64>(0, ms) * 1000;
    emit positionChanged();
    QMetaObject::invokeMethod(m_worker, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, m_positionUs));
    m_framePending = false;
    requestNextFrame();
}

void PreviewController::stepFrames(int delta)
{
    pause();
    const Rational fps = m_info.frameRate.isValid() ? m_info.frameRate
                                                     : Rational{30, 1};
    const int64_t frameUs = framesToMicros(1, fps);
    const qint64 target = qMax<qint64>(0, m_positionUs + delta * frameUs);
    seekMs(target / 1000);
}

void PreviewController::onOpened(const MediaInfo &info, bool hardware,
                                 const QString &fallbackReason)
{
    m_info = info;
    m_durationUs = info.durationUs;

    QString mode = hardware ? QStringLiteral("硬件 D3D11VA")
                            : QStringLiteral("软件解码");
    if (!hardware && !fallbackReason.isEmpty())
        mode += QStringLiteral("（%1）").arg(fallbackReason);
    m_decodeMode = mode;

    const Rational fps = info.frameRate.isValid() ? info.frameRate
                                                   : Rational{30, 1};
    m_frameTimer.setInterval(qMax(1, int(1000.0 * fps.den / fps.num)));

    emit durationChanged();
    emit decodeModeChanged();
    requestNextFrame();
}

void PreviewController::onFrameReady(int slot, const QSize &size, int rotation,
                                     qint64 ptsUs, quintptr h0, quintptr h1,
                                     quintptr h2)
{
    m_framePending = false;
    m_positionUs = ptsUs;
    emit positionChanged();

    if (m_item) {
        m_item->setSharedHandles({h0, h1, h2});
        m_item->submitFrame(slot, size, rotation, ptsUs);
    }
}

void PreviewController::requestNextFrame()
{
    if (!m_info.supported || m_framePending)
        return;
    m_framePending = true;
    QMetaObject::invokeMethod(m_worker, "requestFrame", Qt::QueuedConnection);
}

QString PreviewController::timecode() const
{
    return formatTimecode(m_positionUs,
                          m_info.frameRate.isValid() ? m_info.frameRate
                                                     : Rational{30, 1});
}

} // namespace VideoEditor
