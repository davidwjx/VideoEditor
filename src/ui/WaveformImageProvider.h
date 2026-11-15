#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QQuickImageProvider>

namespace VideoEditor {

class WaveformService;

// Exposes waveform peaks to QML as images. The requested id has the form
// "<assetId>/<widthPx>"; the height comes from requestedSize.
class WaveformImageProvider : public QQuickImageProvider {
public:
    explicit WaveformImageProvider(WaveformService *service);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    WaveformService *m_service = nullptr;
    // Rendered waveforms are deterministic per asset/width/height; caching
    // them avoids repainting all peaks on every zoom step.
    mutable QCache<QString, QImage> m_cache{64};
    mutable QMutex m_cacheMutex;
};

} // namespace VideoEditor
