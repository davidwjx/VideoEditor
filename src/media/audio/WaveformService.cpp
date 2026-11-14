#include "WaveformService.h"

#include <cstring>
#include <utility>

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSaveFile>

#include "AssetFingerprint.h"

namespace VideoEditor {

namespace {

constexpr qint64 kWaveformHeaderBytes = 4 + 2 + 8 + 4;

bool readCachedWaveform(const QString &file, WaveformPeaks *peaks)
{
    QFile cache(file);
    if (!cache.open(QIODevice::ReadOnly))
        return false;
    if (cache.size() < kWaveformHeaderBytes)
        return false;

    const QByteArray header = cache.read(kWaveformHeaderBytes);
    QDataStream stream(header);
    stream.setByteOrder(QDataStream::LittleEndian);
    char magic[4] = {};
    quint16 version = 0;
    qint64 durationUs = 0;
    qint32 count = 0;
    if (stream.readRawData(magic, 4) != 4
        || std::memcmp(magic, "VEWF", 4) != 0)
        return false;
    stream >> version >> durationUs >> count;
    if (stream.status() != QDataStream::Ok || version != 1 || durationUs < 0
        || count <= 0)
        return false;
    const qint64 expectedSize = kWaveformHeaderBytes + qint64(count) * 4;
    if (cache.size() < expectedSize)
        return false;

    // Read only the declared payload: a corrupt or padded file cannot make us
    // allocate more than its header promises.
    cache.seek(0);
    const auto decoded = decodeWaveform(cache.read(expectedSize));
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

    {
        QWriteLocker locker(&m_lock);
        // Already resolved, already failed, or already in flight: do not
        // generate again. A transient failure must not turn into an endless
        // retry loop, and an empty result must never replace good peaks.
        if (!m_peaks.value(assetId).isEmpty() || m_failures.contains(assetId)
            || m_pending.contains(assetId))
            return;
        m_pending.insert(assetId);
    }

    QMetaObject::invokeMethod(
        m_worker,
        [this, assetId, path]() {
            const QString cacheFile = cacheFilePath(path);
            WaveformPeaks peaks;
            QString error;
            if (cacheFile.isEmpty() || !readCachedWaveform(cacheFile, &peaks)) {
                peaks = computeWaveform(path, &error);
                if (!peaks.isEmpty()) {
                    if (!cacheFile.isEmpty())
                        writeCachedWaveform(cacheFile, encodeWaveform(peaks));
                } else if (error.isEmpty()) {
                    error = QStringLiteral("无法生成波形数据。");
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, assetId, peaks, error]() {
                    if (!peaks.isEmpty()) {
                        {
                            QWriteLocker locker(&m_lock);
                            m_pending.remove(assetId);
                            m_peaks.insert(assetId, peaks);
                        }
                        emit ready(assetId);
                    } else {
                        {
                            QWriteLocker locker(&m_lock);
                            m_pending.remove(assetId);
                            m_failures.insert(assetId, error);
                        }
                        emit failed(assetId, error);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void WaveformService::retry(const QString &assetId, const QString &path)
{
    {
        QWriteLocker locker(&m_lock);
        m_failures.remove(assetId);
    }
    request(assetId, path);
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

QString WaveformService::lastError(const QString &assetId) const
{
    QReadLocker locker(&m_lock);
    return m_failures.value(assetId);
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
