#include "VideoPreviewItem.h"

#ifdef _WIN32

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
    m_resetRequested.store(true);
    update();
}

void VideoPreviewItem::resetBridge()
{
    m_hasPending = false;
    m_resetRequested.store(true);
    update();
}

void VideoPreviewItem::submitFrame(int slot, const QSize &size,
                                   int rotationDegrees, qint64 ptsUs)
{
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
        m_bridge.initialize(window(), m_handles, m_pendingSize.width(),
                            m_pendingSize.height());
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
        }
    }

    // No texture yet: return no node so the renderer never sees an empty
    // QSGSimpleTextureNode (its material logs "No QSGTexture provided").
    return node;
}

} // namespace VideoEditor

#endif
