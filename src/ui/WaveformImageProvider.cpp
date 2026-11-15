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

    // Optional source range from the trim points. Mapping the requested
    // source interval onto the clip rectangle means a trimmed clip no longer
    // shows the whole asset's waveform squeezed or stretched.
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    if (parts.size() > 3) {
        sourceInUs = qMax<int64_t>(0, parts.at(2).toLongLong());
        sourceOutUs = parts.at(3).toLongLong();
    }
    // Caches written before duration metadata existed can report durationUs
    // <= 0; derive an effective duration from the 10 ms bucket count instead.
    const int64_t effectiveDurationUs =
        peaks.durationUs > 0
            ? peaks.durationUs
            : int64_t(peaks.bucketCount()) * 1'000'000
                  / WaveformPeaks::kBucketsPerSecond;
    if (sourceOutUs <= sourceInUs || peaks.durationUs <= 0) {
        sourceInUs = 0;
        sourceOutUs = effectiveDurationUs;
    }
    sourceInUs = qBound<int64_t>(0, sourceInUs, effectiveDurationUs);
    sourceOutUs = qBound<int64_t>(sourceInUs, sourceOutUs, effectiveDurationUs);
    const int firstBucket = int(sourceInUs * peaks.bucketCount()
                                / qMax<int64_t>(1, effectiveDurationUs));
    const int lastBucket = int(sourceOutUs * peaks.bucketCount()
                               / qMax<int64_t>(1, effectiveDurationUs));

    const QString cacheKey = parts.first() + QLatin1Char('/')
                             + QString::number(width) + QLatin1Char('x')
                             + QString::number(height) + QLatin1Char('/')
                             + QString::number(firstBucket) + QLatin1Char('-')
                             + QString::number(lastBucket) + QLatin1Char('/')
                             + QString::number(peaks.bucketCount());
    {
        QMutexLocker locker(&m_cacheMutex);
        if (const QImage *cached = m_cache.object(cacheKey))
            return QImage(*cached);
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(0x7f, 0xc8, 0xff, 220));
    const double center = height / 2.0;
    const int rangeBuckets = qMax(1, lastBucket - firstBucket);
    for (int x = 0; x < width; ++x) {
        const int from = firstBucket
                         + int(int64_t(x) * rangeBuckets / width);
        const int to = qMax(from + 1,
                            firstBucket
                                + int(int64_t(x + 1) * rangeBuckets / width));
        float minimum = 0.0f;
        float maximum = 0.0f;
        for (int b = from; b < qMin(to, qMin(lastBucket + 1,
                                             peaks.bucketCount())); ++b) {
            minimum = qMin(minimum, peaks.minValues.at(b));
            maximum = qMax(maximum, peaks.maxValues.at(b));
        }
        const double top = center - maximum * center;
        const double bottom = center - minimum * center;
        painter.drawLine(QPointF(x, top), QPointF(x, bottom));
    }
    painter.end();
    {
        QMutexLocker locker(&m_cacheMutex);
        m_cache.insert(cacheKey, new QImage(image));
    }
    return image;
}

} // namespace VideoEditor
