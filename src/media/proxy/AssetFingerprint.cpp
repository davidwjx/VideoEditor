#include "AssetFingerprint.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace VideoEditor {

QString fingerprintFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QFileInfo info(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));

    // Hashing the whole media file is too expensive for large sources. Size,
    // mtime, the head and the tail together identify a file reliably enough
    // for cache invalidation.
    constexpr qint64 kChunk = 1024 * 1024;
    const QByteArray head = file.read(kChunk);
    hash.addData(head);
    if (info.size() > kChunk * 2) {
        file.seek(info.size() - kChunk);
        hash.addData(file.read(kChunk));
    }

    return QString::fromLatin1(hash.result().toHex());
}

} // namespace VideoEditor
