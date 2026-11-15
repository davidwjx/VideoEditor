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
    // The old handles and frame size belong to a texture ring that is about to
    // be torn down. Keeping them would let the render thread reopen stale
    // shared resources before the first frame of the new media arrives.
    m_handles = {};
    m_pendingSize = QSize{};
    const bool hadFrameSize = !m_frameSize.isEmpty();
    m_frameSize = QSize{};
    m_pendingSlot = -1;
    m_pendingPts = 0;
    m_rotation = 0;
    m_abandoned = {};
    m_bridgeErrorReported = false;
    m_resetRequested.store(true);
    if (hadFrameSize)
        emit contentRectChanged();
    update();
}

void VideoPreviewItem::discardFrame(int slot)
{
    if (slot < 0 || slot >= 3)
        return;
    m_abandoned[size_t(slot)] = true;
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
    const bool frameSizeChanged = size != m_frameSize;
    m_frameSize = size;
    m_rotation = rotationDegrees;
    m_pendingPts = ptsUs;
    m_hasPending = true;
    if (frameSizeChanged)
        emit contentRectChanged();
    update();
}

QRectF VideoPreviewItem::contentRect() const
{
    if (m_frameSize.isEmpty())
        return boundingRect();
    return aspectFitRect(QSizeF(m_frameSize));
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
    if (newGeometry.size() != oldGeometry.size()) {
        if (!m_frameSize.isEmpty())
            emit contentRectChanged();
        update();
    }
}

QSGNode *VideoPreviewItem::updatePaintNode(QSGNode *oldNode,
                                           UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);

    if (m_resetRequested.exchange(false)) {
        // Drop the node (and the Qt texture wrappers it owns) before the bridge
        // releases the D3D11 textures they wrap. QSGSimpleTextureNode::
        // setTexture(nullptr) dereferences the texture in this Qt build, so
        // the old node is dropped instead of cleared.
        delete node;
        node = nullptr;
        m_bridge = FrameBridge();
    }

    const bool handlesValid = m_handles[0] && m_handles[1] && m_handles[2];
    if (!m_bridge.isValid() && window() && handlesValid
        && m_pendingSize.isValid()) {
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
            // Never consume the slot of a live pending frame: an abandoned flag
            // can outlive a bridge reset and the slot indices are reused.
            if (m_abandoned[size_t(i)]
                && !(m_hasPending && m_pendingSlot == i)) {
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
