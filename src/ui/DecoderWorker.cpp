#include "DecoderWorker.h"

#include <QFileInfo>
#include <QMetaType>

#include "D3D11Context.h"
#include "FramePublisher.h"
#include "MediaProbe.h"
#include "VideoDecoderFactory.h"

Q_DECLARE_METATYPE(VideoEditor::MediaInfo)

namespace VideoEditor {

namespace {

// 90/270 degree display rotation transposes the visible frame. The shared
// texture ring is allocated at display dimensions so the rotated picture keeps
// its natural aspect ratio.
QSize displaySizeFor(int width, int height, int rotationDegrees)
{
    const int normalized = ((rotationDegrees % 360) + 360) % 360;
    return (normalized == 90 || normalized == 270) ? QSize(height, width)
                                                   : QSize(width, height);
}

} // namespace

DecoderWorker::DecoderWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<VideoEditor::MediaInfo>("VideoEditor::MediaInfo");
}

DecoderWorker::~DecoderWorker() = default;

void DecoderWorker::clearDecoder()
{
    m_publisher.reset();
    m_ring.reset();
    m_decoder.reset();
    m_context.reset();
    m_eof = false;
    m_busy = false;
}

void DecoderWorker::open(const QString &path, const QString &proxyPath)
{
    clearDecoder();

    const bool usingProxy = QFileInfo::exists(proxyPath);
    const QString actualPath = usingProxy ? proxyPath : path;
    QString error;
    auto info = MediaProbe::probe(path, &error);
    if (!info) {
        emit this->error(error);
        return;
    }
    m_info = *info;
    m_info.path = path;
    // ffmpeg 生成代理时会应用显示矩阵，代理画面已经摆正，不能重复旋转。
    if (usingProxy)
        m_info.rotationDegrees = 0;

    m_context = D3D11Context::create(true, &error);
    if (!m_context) {
        // 视频设备不可用时，仍然创建普通 D3D11 设备用于软件帧上传。
        QString softwareError;
        m_context = D3D11Context::create(false, &softwareError);
        if (!m_context) {
            emit this->error(QStringLiteral("%1\n%2").arg(error, softwareError));
            return;
        }
    }

    auto result = createVideoDecoder(actualPath, *m_context, m_info);
    if (!result.decoder) {
        emit this->error(result.error);
        return;
    }
    m_decoder = std::move(result.decoder);

    DecodedFrame firstFrame;
    if (!m_decoder->decodeNext(firstFrame, &error)) {
        emit this->error(error.isEmpty()
                             ? QStringLiteral("视频没有可解码的帧。")
                             : error);
        return;
    }

    const QSize displaySize = displaySizeFor(firstFrame.width,
                                             firstFrame.height,
                                             m_info.rotationDegrees);
    m_ring = std::make_unique<SharedTextureRing>(*m_context);
    if (!m_ring->create(displaySize.width(), displaySize.height(),
                        SharedTextureRing::kSlotCount, &error)) {
        emit this->error(error);
        return;
    }

    m_publisher = std::make_unique<FramePublisher>(*m_context, *m_ring);
    if (!m_publisher->publish(firstFrame, m_info.rotationDegrees, &error)) {
        emit this->error(error);
        return;
    }
    m_eof = false;

    emit opened(m_info, m_decoder->isHardware(), result.hardwareFallbackReason);
    emit frameReady(m_publisher->lastWrittenSlot(), displaySize,
                    m_info.rotationDegrees, firstFrame.ptsUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)));
}

void DecoderWorker::requestFrame()
{
    if (!m_decoder || m_busy || m_eof)
        return;

    m_busy = true;
    DecodedFrame frame;
    QString error;
    if (!m_decoder->decodeNext(frame, &error)) {
        m_busy = false;
        if (!error.isEmpty())
            emit this->error(error);
        else
            m_eof = true;
        return;
    }

    if (!m_publisher->publish(frame, m_info.rotationDegrees, &error)) {
        m_busy = false;
        emit this->error(error);
        return;
    }

    m_busy = false;
    emit frameReady(m_publisher->lastWrittenSlot(),
                    displaySizeFor(frame.width, frame.height,
                                   m_info.rotationDegrees),
                    m_info.rotationDegrees, frame.ptsUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)));
}

void DecoderWorker::seek(qint64 ptsUs)
{
    if (!m_decoder)
        return;
    QString error;
    if (!m_decoder->seek(ptsUs, &error)) {
        emit this->error(error);
        return;
    }
    m_eof = false;
}

} // namespace VideoEditor
