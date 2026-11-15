#pragma once

#include <QString>

namespace VideoEditor {

class CacheStore {
public:
    explicit CacheStore(QString root);

    QString root() const { return m_root; }
    QString proxyPath(const QString &key) const;
    bool ensureProxyDir(QString *error = nullptr) const;

    // Generated repair cache lives under <root>/removal. It is disposable and
    // never contains project-owned mask/tracking sidecars.
    QString removalRoot() const;
    QString removalPath(const QString &cacheKey) const;
    bool ensureRemovalDir(QString *error = nullptr) const;
    bool clearRemoval(QString *error = nullptr) const;

    qint64 sizeOnDisk(QString *error = nullptr) const;
    bool clear(QString *error = nullptr) const;

private:
    QString m_root;
};

} // namespace VideoEditor
