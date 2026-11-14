#include "WaveformService.h"

#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSaveFile>

#include "AssetFingerprint.h"

namespace VideoEditor {

namespace {

bool readCachedWaveform(const QString &file, WaveformPeaks *peaks)
{
    QFile cache(file);
    if (!cache.open(QIODevice::ReadOnly))
        return false;
    const auto decoded = decodeWaveform(cache.readAll());
    if (!decoded || decoded->isEmpty())
        return false;
    *peaks = *decoded;
    return true;
}

void writeCachedWaveform(const QString &file, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile cache(file);
    if (!cache.open(QIODevice::WriteOnly))
        return;
    if (cache.write(bytes) != bytes.size()) {
        cache.cancelWriting();
        return;
    }
    // A failed commit leaves the previous cache (if any) intact; the next
    // request simply regenerates the waveform.
    cache.commit();
}

} // namespace

WaveformService::WaveformService(QString cacheRoot, QObject *parent)
    : QObject(parent)
    , m_cacheRoot(std::move(cacheRoot))
    , m_worker(new QObject)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

WaveformService::~WaveformService()
{
    m_thread.quit();
    m_thread.wait();
}

void WaveformService::request(const QString &assetId, const QString &path)
{
    if (assetId.isEmpty() || path.isEmpty())
        return;

    QMetaObject::invokeMethod(
        m_worker,
        [this, assetId, path]() {
            const QString cacheFile = cacheFilePath(path);
            WaveformPeaks peaks;
            if (cacheFile.isEmpty() || !readCachedWaveform(cacheFile, &peaks)) {
                QString error;
                peaks = computeWaveform(path, &error);
                if (!peaks.isEmpty() && !cacheFile.isEmpty())
                    writeCachedWaveform(cacheFile, encodeWaveform(peaks));
            }
            QMetaObject::invokeMethod(
                this,
                [this, assetId, peaks]() {
                    {
                        QWriteLocker locker(&m_lock);
                        m_peaks.insert(assetId, peaks);
                    }
                    emit ready(assetId);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

bool WaveformService::hasPeaks(const QString &assetId) const
{
    QReadLocker locker(&m_lock);
    const auto it = m_peaks.constFind(assetId);
    return it != m_peaks.constEnd() && !it->isEmpty();
}

WaveformPeaks WaveformService::peaks(const QString &assetId) const
{
    QReadLocker locker(&m_lock);
    return m_peaks.value(assetId);
}

QString WaveformService::cacheFilePath(const QString &path) const
{
    const QString fingerprint = fingerprintFile(path);
    if (fingerprint.isEmpty())
        return {};
    return QDir(m_cacheRoot).filePath(
        QStringLiteral("waveform/%1.wf").arg(fingerprint));
}

} // namespace VideoEditor
