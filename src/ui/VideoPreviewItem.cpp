#include "VideoPreviewItem.h"

#ifdef _WIN32

#include <QMetaObject>
#include <QSGSimpleTextureNode>

namespace VideoEditor {

VideoPreviewItem::VideoPreviewItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void VideoPreviewItem::setSharedHandles(const std::array<quintptr, 3> &handles)
{
    if (m_handles == handles)
        return;
    m_handles = handles;
    m_abandoned = {};
    m_bridgeErrorReported = false;
    m_resetRequested.store(true);
    update();
}

void VideoPreviewItem::resetBridge()
{
    m_hasPending = false;
    m_abandoned = {};
    m_bridgeErrorReported = false;
    m_resetRequested.store(true);
    update();
}

void VideoPreviewItem::submitFrame(int slot, const QSize &size,
                                   int rotationDegrees, qint64 ptsUs)
{
    // A frame that is replaced before it is presented must be released back to
    // the producer, or that ring slot stays locked forever.
    if (m_hasPending && m_pendingSlot >= 0 && m_pendingSlot < 3
        && m_pendingSlot != slot) {
        m_abandoned[size_t(m_pendingSlot)] = true;
    }
    m_pendingSlot = slot;
    m_pendingSize = size;
    m_rotation = rotationDegrees;
    m_pendingPts = ptsUs;
    m_hasPending = true;
    update();
}

QRectF VideoPreviewItem::aspectFitRect(const QSizeF &frameSize) const
{
    if (frameSize.isEmpty())
        return boundingRect();
    QSizeF scaled = frameSize;
    scaled.scale(boundingRect().size(), Qt::KeepAspectRatio);
    QRectF result(QPointF(0, 0), scaled);
    result.moveCenter(boundingRect().center());
    return result;
}

void VideoPreviewItem::geometryChange(const QRectF &newGeometry,
                                      const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

QSGNode *VideoPreviewItem::updatePaintNode(QSGNode *oldNode,
                                           UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);

    if (m_resetRequested.exchange(false)) {
        m_bridge = FrameBridge();
        // QSGSimpleTextureNode::setTexture(nullptr) dereferences the texture in
        // this Qt build, so the old node is dropped instead of cleared. A frame
        // submitted together with new shared handles survives the reset;
        // resetBridge() is what explicitly drops a stale pending frame.
        delete node;
        node = nullptr;
    }

    if (!m_bridge.isValid() && window() && m_pendingSize.isValid()) {
        QString error;
        if (!m_bridge.initialize(window(), m_handles, m_pendingSize.width(),
                                 m_pendingSize.height(), &error)) {
            if (!m_bridgeErrorReported && !error.isEmpty()) {
                m_bridgeErrorReported = true;
                emit errorOccurred(error);
            }
        } else {
            m_bridgeErrorReported = false;
        }
    }

    if (m_bridge.isValid()) {
        for (int i = 0; i < 3; ++i) {
            if (m_abandoned[size_t(i)]) {
                m_bridge.discard(i);
                m_abandoned[size_t(i)] = false;
            }
        }
    }

    if (m_hasPending && m_bridge.isValid()) {
        if (QSGTexture *texture = m_bridge.present(m_pendingSlot, window())) {
            if (!node) {
                node = new QSGSimpleTextureNode();
                node->setOwnsTexture(true);
            }
            node->setTexture(texture);
            node->setRect(aspectFitRect(m_pendingSize));
            node->markDirty(QSGNode::DirtyMaterial);
            emit framePresented(m_pendingPts);
            m_hasPending = false;
        } else {
            // The producer may not have released the keyed mutex yet. Never
            // block the render thread; schedule the retry on the GUI thread
            // (update() called during the sync pass can be swallowed by the
            // current render cycle).
            QMetaObject::invokeMethod(this, [this] { update(); },
                                      Qt::QueuedConnection);
        }
    }

    // Keep the aspect fit in sync with the item's current size even while
    // paused (a resize must recentre/rescale the last presented frame).
    if (node && m_pendingSize.isValid())
        node->setRect(aspectFitRect(m_pendingSize));

    // No texture yet: return no node so the renderer never sees an empty
    // QSGSimpleTextureNode (its material logs "No QSGTexture provided").
    return node;
}

} // namespace VideoEditor

#endif
