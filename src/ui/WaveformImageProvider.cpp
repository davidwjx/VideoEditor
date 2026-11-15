#include "WaveformImageProvider.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QStringList>

#include "WaveformService.h"

namespace VideoEditor {

WaveformImageProvider::WaveformImageProvider(WaveformService *service)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_service(service)
{
}

QImage WaveformImageProvider::requestImage(const QString &id, QSize *size,
                                           const QSize &requestedSize)
{
    const QStringList parts = id.split(QLatin1Char('/'));
    const int width = qMax(1, requestedSize.width() > 0
                                  ? requestedSize.width()
                                  : (parts.size() > 1 ? parts.at(1).toInt() : 400));
    const int height = qMax(1, requestedSize.height() > 0
                                  ? requestedSize.height()
                                  : 56);
    if (size)
        *size = QSize(width, height);

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return image;
    image.fill(Qt::transparent);
    if (!m_service)
        return image;
    const WaveformPeaks peaks = m_service->peaks(parts.first());
    if (peaks.isEmpty())
        return image;

    const QString cacheKey = parts.first() + QLatin1Char('/')
                             + QString::number(width) + QLatin1Char('x')
                             + QString::number(height) + QLatin1Char('/')
                             + QString::number(peaks.bucketCount());
    if (const QImage *cached = m_cache.object(cacheKey))
        return *cached;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(0x7f, 0xc8, 0xff, 220));
    const double center = height / 2.0;
    for (int x = 0; x < width; ++x) {
        const int from = int(int64_t(x) * peaks.bucketCount() / width);
        const int to = qMax(from + 1,
                            int(int64_t(x + 1) * peaks.bucketCount() / width));
        float minimum = 0.0f;
        float maximum = 0.0f;
        for (int b = from; b < qMin(to, peaks.bucketCount()); ++b) {
            minimum = qMin(minimum, peaks.minValues.at(b));
            maximum = qMax(maximum, peaks.maxValues.at(b));
        }
        const double top = center - maximum * center;
        const double bottom = center - minimum * center;
        painter.drawLine(QPointF(x, top), QPointF(x, bottom));
    }
    painter.end();
    m_cache.insert(cacheKey, new QImage(image));
    return image;
}

} // namespace VideoEditor
