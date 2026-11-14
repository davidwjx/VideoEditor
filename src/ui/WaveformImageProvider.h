#pragma once

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
};

} // namespace VideoEditor
