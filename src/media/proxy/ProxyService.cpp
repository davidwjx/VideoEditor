#include "ProxyService.h"

#include <QFileInfo>

#include <utility>

#include "AssetFingerprint.h"
#include "ProxyGenerator.h"

namespace VideoEditor {

ProxyService::ProxyService(QString cacheRoot, QString ffmpegPath, QObject *parent)
    : QObject(parent)
    , m_cache(std::move(cacheRoot))
    , m_ffmpegPath(std::move(ffmpegPath))
{
}

void ProxyService::request(const QString &assetId, const QString &sourcePath,
                           int height)
{
    const QString fingerprint = fingerprintFile(sourcePath);
    if (fingerprint.isEmpty()) {
        emit failed(assetId, QStringLiteral("无法读取素材指纹。"));
        return;
    }

    const QString key = QStringLiteral("%1-h%2").arg(fingerprint).arg(height);
    const QString output = m_cache.proxyPath(key);
    if (QFileInfo::exists(output)) {
        emit ready(assetId, output);
        return;
    }

    m_queue.enqueue(Job{assetId, sourcePath, output, height});
    startNext();
}

void ProxyService::startNext()
{
    if (m_generator || m_queue.isEmpty())
        return;

    const Job job = m_queue.dequeue();
    m_generator = new ProxyGenerator(m_ffmpegPath, this);

    connect(m_generator, &ProxyGenerator::finished, this,
            [this, job](const QString &path) {
                m_generator->deleteLater();
                m_generator = nullptr;
                emit ready(job.assetId, path);
                startNext();
            });

    connect(m_generator, &ProxyGenerator::failed, this,
            [this, job](const QString &message) {
                m_generator->deleteLater();
                m_generator = nullptr;
                emit failed(job.assetId, message);
                startNext();
            });

    m_generator->start(job.sourcePath, job.outputPath, job.height);
}

} // namespace VideoEditor
