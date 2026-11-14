#include "CacheStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

CacheStore::CacheStore(QString root)
    : m_root(std::move(root))
{
}

QString CacheStore::proxyPath(const QString &key) const
{
    return QDir(m_root).filePath(QStringLiteral("proxies/%1/proxy.mp4").arg(key));
}

bool CacheStore::ensureProxyDir(QString *error) const
{
    if (!QDir().mkpath(QDir(m_root).filePath(QStringLiteral("proxies")))) {
        setError(error, QStringLiteral("无法创建代理缓存目录。"));
        return false;
    }
    return true;
}

qint64 CacheStore::sizeOnDisk(QString *error) const
{
    Q_UNUSED(error);
    QDirIterator it(m_root, QDir::Files, QDirIterator::Subdirectories);
    qint64 total = 0;
    while (it.hasNext())
        total += QFileInfo(it.next()).size();
    return total;
}

bool CacheStore::clear(QString *error) const
{
    QDir dir(m_root);
    if (!dir.exists())
        return true;
    if (!dir.removeRecursively()) {
        setError(error, QStringLiteral("无法清理缓存目录。"));
        return false;
    }
    return true;
}

} // namespace VideoEditor
