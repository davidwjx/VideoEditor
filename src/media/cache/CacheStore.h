#pragma once

#include <QString>

namespace VideoEditor {

class CacheStore {
public:
    explicit CacheStore(QString root);

    QString root() const { return m_root; }
    QString proxyPath(const QString &key) const;
    bool ensureProxyDir(QString *error = nullptr) const;
    qint64 sizeOnDisk(QString *error = nullptr) const;
    bool clear(QString *error = nullptr) const;

private:
    QString m_root;
};

} // namespace VideoEditor
