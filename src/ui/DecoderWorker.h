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
    void open(const QString &path, const QString &proxyPath);
    void requestFrame();
    void seek(qint64 ptsUs);

signals:
    void opened(VideoEditor::MediaInfo info, bool hardware,
                QString hardwareFallbackReason);
    void frameReady(int slot, QSize size, int rotationDegrees, qint64 ptsUs,
                    quintptr handle0, quintptr handle1, quintptr handle2);
    void error(QString message);

private:
    void clearDecoder();

    std::unique_ptr<D3D11Context> m_context;
    std::unique_ptr<IVideoDecoder> m_decoder;
    std::unique_ptr<SharedTextureRing> m_ring;
    std::unique_ptr<FramePublisher> m_publisher;
    MediaInfo m_info;
    bool m_busy = false;
    bool m_eof = false;
};

} // namespace VideoEditor
