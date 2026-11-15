#include "MaskImageProvider.h"

#include <QMutexLocker>
#include <QSize>

namespace VideoEditor {

MaskImageProvider::MaskImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage MaskImageProvider::requestImage(const QString &, QSize *size,
                                       const QSize &requestedSize)
{
    QImage image;
    {
        QMutexLocker lock(&m_mutex);
        image = m_image;
    }
    if (!requestedSize.isEmpty() && !image.isNull()
        && image.size() != requestedSize) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    if (size)
        *size = image.size();
    return image;
}

void MaskImageProvider::setMask(const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    m_image = image;
    ++m_revision;
}

int MaskImageProvider::revision() const
{
    QMutexLocker lock(&m_mutex);
    return m_revision;
}

} // namespace VideoEditor
