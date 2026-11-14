#pragma once

#include <array>
#include <atomic>
#include <QQuickItem>
#include <QSize>

#include "FrameBridge.h"

namespace VideoEditor {

class VideoPreviewItem : public QQuickItem {
    Q_OBJECT

public:
    explicit VideoPreviewItem(QQuickItem *parent = nullptr);

    void setSharedHandles(const std::array<quintptr, 3> &handles);
    void submitFrame(int slot, const QSize &size, int rotationDegrees,
                     qint64 ptsUs);
    void resetBridge();

signals:
    void framePresented(qint64 ptsUs);
    void errorOccurred(QString message);

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
    qint64 m_pendingPts = 0;
    int m_pendingSlot = -1;
    int m_rotation = 0;
    bool m_hasPending = false;
    bool m_bridgeErrorReported = false;
    std::atomic_bool m_resetRequested{false};
};

} // namespace VideoEditor
