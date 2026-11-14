#pragma once

#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>
#include <QString>
#include <QThread>

#include "Waveform.h"

namespace VideoEditor {

// Asynchronously builds (or reloads from disk) waveform peaks for assets.
// Generation runs on a dedicated worker thread, so the GUI thread never
// blocks. Peaks and failure messages are published under a read/write lock.
class WaveformService : public QObject {
    Q_OBJECT

public:
    explicit WaveformService(QString cacheRoot, QObject *parent = nullptr);
    ~WaveformService() override;

    // Schedules generation for an asset. The request itself never blocks.
    // ready() is emitted once peaks are available; failed() is emitted with a
    // user-facing Chinese message when generation cannot produce peaks.
    // Repeated requests for a resolved asset are no-ops (use retry() after a
    // failure if a new attempt is wanted).
    void request(const QString &assetId, const QString &path);
    // Clears a remembered failure and schedules the asset again.
    void retry(const QString &assetId, const QString &path);
    bool hasPeaks(const QString &assetId) const;
    WaveformPeaks peaks(const QString &assetId) const;
    // Last failure message for an asset, or an empty string.
    QString lastError(const QString &assetId) const;
    // Performs file I/O (asset fingerprint): intended for worker/tests, not
    // for the GUI hot path. request() calls it on the worker thread.
    QString cacheFilePath(const QString &path) const;

signals:
    void ready(QString assetId);
    void failed(QString assetId, QString message);

private:
    QString m_cacheRoot;
    QThread m_thread;
    QObject *m_worker = nullptr;
    mutable QReadWriteLock m_lock;
    QHash<QString, WaveformPeaks> m_peaks;
    QHash<QString, QString> m_failures;
    QSet<QString> m_pending;
};

} // namespace VideoEditor
