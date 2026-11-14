#pragma once

#include <array>
#include <memory>
#include <QObject>
#include <QSize>

#include "IVideoDecoder.h"
#include "MediaInfo.h"
#include "SharedTextureRing.h"

namespace VideoEditor {

class D3D11Context;
class FramePublisher;

class DecoderWorker : public QObject {
    Q_OBJECT

public:
    explicit DecoderWorker(QObject *parent = nullptr);
    ~DecoderWorker() override;

public slots:
    // Every open is tagged with the controller's generation token so frames
    // (and the opened notification) from a superseded media can be rejected.
    void open(const QString &path, const QString &proxyPath,
              quint64 generation);
    void close();
    void requestFrame();
    void seek(qint64 ptsUs);

signals:
    void opened(VideoEditor::MediaInfo info, bool hardware,
                QString hardwareFallbackReason, quint64 generation);
    void frameReady(int slot, QSize size, int rotationDegrees, qint64 ptsUs,
                    quintptr handle0, quintptr handle1, quintptr handle2,
                    quint64 generation);
    void error(QString message);
    void finished();

private:
    void clearDecoder();

    std::unique_ptr<D3D11Context> m_context;
    std::unique_ptr<IVideoDecoder> m_decoder;
    std::unique_ptr<SharedTextureRing> m_ring;
    std::unique_ptr<FramePublisher> m_publisher;
    MediaInfo m_info;
    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_eof = false;
};

} // namespace VideoEditor
