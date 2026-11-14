#pragma once

#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QString>
#include <QThread>

#include "Waveform.h"

namespace VideoEditor {

// Asynchronously builds (or reloads from disk) waveform peaks for assets.
// Generation runs on a dedicated worker thread, so the GUI thread never
// blocks. Peaks are published under a read/write lock.
class WaveformService : public QObject {
    Q_OBJECT

public:
    explicit WaveformService(QString cacheRoot, QObject *parent = nullptr);
    ~WaveformService() override;

    // Schedules generation for an asset. The request itself never blocks;
    // ready() is emitted on this object's thread once peaks are available.
    void request(const QString &assetId, const QString &path);
    bool hasPeaks(const QString &assetId) const;
    WaveformPeaks peaks(const QString &assetId) const;
    QString cacheFilePath(const QString &path) const;

signals:
    void ready(QString assetId);

private:
    QString m_cacheRoot;
    QThread m_thread;
    QObject *m_worker = nullptr;
    mutable QReadWriteLock m_lock;
    QHash<QString, WaveformPeaks> m_peaks;
};

} // namespace VideoEditor
