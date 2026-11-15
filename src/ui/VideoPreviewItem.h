#pragma once

#include <array>
#include <atomic>
#include <QQuickItem>
#include <QRectF>
#include <QSize>

#include "FrameBridge.h"

namespace VideoEditor {

class VideoPreviewItem : public QQuickItem {
    Q_OBJECT
    // Aspect-fit destination of the presented video inside this item. The mask
    // editor maps pointer coordinates through this rectangle, never the whole
    // item (letterbox bars are not part of the picture).
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)

public:
    explicit VideoPreviewItem(QQuickItem *parent = nullptr);

    void setSharedHandles(const std::array<quintptr, 3> &handles);
    void submitFrame(int slot, const QSize &size, int rotationDegrees,
                     qint64 ptsUs);
    // Release a frame the controller rejected (stale media generation) without
    // showing it. The actual keyed-mutex release happens in updatePaintNode(),
    // where the shared resources are valid.
    void discardFrame(int slot);
    void resetBridge();

    QRectF contentRect() const;

signals:
    void framePresented(qint64 ptsUs);
    void errorOccurred(QString message);
    void contentRectChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;

private:
    QRectF aspectFitRect(const QSizeF &frameSize) const;

    FrameBridge m_bridge;
    std::array<quintptr, 3> m_handles{};
    std::array<bool, 3> m_abandoned{};
    QSize m_pendingSize;
    QSize m_frameSize;
    qint64 m_pendingPts = 0;
    int m_pendingSlot = -1;
    int m_rotation = 0;
    bool m_hasPending = false;
    bool m_bridgeErrorReported = false;
    std::atomic_bool m_resetRequested{false};
};

} // namespace VideoEditor
