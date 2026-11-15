#pragma once

#include <QMutex>
#include <QQuickImageProvider>

namespace VideoEditor {

// Serves the live mask editor overlay as image://mask/<revision>. The revision
// in the URL only exists to make QML re-request the image after every edit;
// requestImage() always returns the newest overlay.
class MaskImageProvider : public QQuickImageProvider {
public:
    MaskImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
    void setMask(const QImage &image);
    int revision() const;

private:
    mutable QMutex m_mutex;
    QImage m_image;
    int m_revision = 0;
};

} // namespace VideoEditor
