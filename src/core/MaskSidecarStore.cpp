#include "MaskSidecarStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QCryptographicHash>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool isValidHash(const QString &hash)
{
    static const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
    return hex64.match(hash).hasMatch();
}

QString hashBytes(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool writeAtomicFile(const QString &path, const QByteArray &bytes,
                     QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("无法写入遮罩旁车文件：%1").arg(path));
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, QStringLiteral("无法保存遮罩旁车文件：%1").arg(path));
        return false;
    }
    return true;
}

bool copyTree(const QString &from, const QString &to, QString *error)
{
    const QDir source(from);
    if (!source.exists())
        return true;
    if (!QDir().mkpath(to)) {
        setError(error, QStringLiteral("无法创建遮罩旁车目录：%1").arg(to));
        return false;
    }

    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString sourcePath = it.next();
        const QString relative = source.relativeFilePath(sourcePath);
        const QString targetPath = QDir(to).filePath(relative);
        if (!QDir().mkpath(QFileInfo(targetPath).absolutePath())) {
            setError(error, QStringLiteral("无法创建遮罩旁车目录：%1")
                                .arg(QFileInfo(targetPath).absolutePath()));
            return false;
        }
        if (QFile::exists(targetPath) && !QFile::remove(targetPath)) {
            setError(error, QStringLiteral("无法覆盖遮罩旁车文件：%1").arg(targetPath));
            return false;
        }
        if (!QFile::copy(sourcePath, targetPath)) {
            setError(error, QStringLiteral("无法迁移遮罩旁车文件：%1").arg(sourcePath));
            return false;
        }
    }
    return true;
}

} // namespace

MaskSidecarStore::MaskSidecarStore(QString projectFilePath)
    : m_projectFilePath(std::move(projectFilePath))
{
}

QString MaskSidecarStore::rootForProject(const QString &projectFilePath,
                                         const QString &fallbackRoot)
{
    if (!projectFilePath.isEmpty())
        return projectFilePath + QStringLiteral(".assets");
    return QDir(fallbackRoot).filePath(QStringLiteral("removal-sidecars/unsaved"));
}

void MaskSidecarStore::setProjectPath(const QString &projectFilePath,
                                      const QString &fallbackRoot)
{
    m_projectFilePath = projectFilePath;
    m_fallbackRoot = fallbackRoot;
}

QString MaskSidecarStore::sidecarRoot() const
{
    return rootForProject(m_projectFilePath, m_fallbackRoot);
}

QString MaskSidecarStore::maskPath(const QString &maskHash) const
{
    if (!isValidHash(maskHash))
        return QString();
    return QDir(sidecarRoot())
        .filePath(QStringLiteral("masks/%1.rle").arg(maskHash));
}

QString MaskSidecarStore::trackingPath(const QString &confidenceHash) const
{
    if (!isValidHash(confidenceHash))
        return QString();
    return QDir(sidecarRoot())
        .filePath(QStringLiteral("tracking/%1.json").arg(confidenceHash));
}

bool MaskSidecarStore::ensureLayout(QString *error)
{
    if (m_projectFilePath.isEmpty() && m_fallbackRoot.isEmpty()) {
        setError(error, QStringLiteral("无法确定工程旁车目录：工程未保存且没有回退目录。"));
        return false;
    }
    const QDir root(sidecarRoot());
    if (!QDir().mkpath(root.filePath(QStringLiteral("masks")))
        || !QDir().mkpath(root.filePath(QStringLiteral("tracking")))) {
        setError(error, QStringLiteral("无法创建工程旁车目录：%1").arg(root.path()));
        return false;
    }
    return true;
}

bool MaskSidecarStore::writeMask(const BinaryMask &mask, QString *hashOut,
                                 QString *error)
{
    const QByteArray encoded = MaskCodec::encode(mask);
    if (encoded.isEmpty()) {
        setError(error, QStringLiteral("遮罩数据无效，无法写入旁车文件。"));
        return false;
    }
    const QString hash = hashBytes(encoded);
    if (!ensureLayout(error))
        return false;
    if (!writeAtomicFile(maskPath(hash), encoded, error))
        return false;
    if (hashOut)
        *hashOut = hash;
    return true;
}

std::optional<BinaryMask> MaskSidecarStore::readMask(const QString &maskHash,
                                                     QString *error) const
{
    const QString path = maskPath(maskHash);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("遮罩引用无效：需要 64 位十六进制哈希。"));
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("无法读取遮罩旁车文件：%1").arg(path));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    return MaskCodec::decode(bytes, error);
}

bool MaskSidecarStore::writeTracking(const QJsonObject &json, QString *hashOut,
                                     QString *error)
{
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const QString hash = hashBytes(bytes);
    if (!ensureLayout(error))
        return false;
    if (!writeAtomicFile(trackingPath(hash), bytes, error))
        return false;
    if (hashOut)
        *hashOut = hash;
    return true;
}

std::optional<QJsonObject> MaskSidecarStore::readTracking(
    const QString &confidenceHash, QString *error) const
{
    const QString path = trackingPath(confidenceHash);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("跟踪置信度引用无效：需要 64 位十六进制哈希。"));
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("无法读取跟踪置信度旁车文件：%1").arg(path));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("跟踪置信度旁车文件损坏：%1").arg(path));
        return std::nullopt;
    }
    return document.object();
}

bool MaskSidecarStore::relocateTo(const QString &newProjectFilePath,
                                  QString *error)
{
    if (newProjectFilePath.isEmpty()) {
        setError(error, QStringLiteral("无法迁移工程旁车：目标工程路径为空。"));
        return false;
    }

    const QString oldRoot = sidecarRoot();
    const QString newRoot = rootForProject(newProjectFilePath, m_fallbackRoot);
    if (oldRoot == newRoot) {
        m_projectFilePath = newProjectFilePath;
        return true;
    }

    if (!copyTree(QDir(oldRoot).filePath(QStringLiteral("masks")),
                  QDir(newRoot).filePath(QStringLiteral("masks")), error)
        || !copyTree(QDir(oldRoot).filePath(QStringLiteral("tracking")),
                     QDir(newRoot).filePath(QStringLiteral("tracking")), error)) {
        return false;
    }

    m_projectFilePath = newProjectFilePath;
    return true;
}

bool MaskSidecarStore::removeUnreferenced(const QSet<QString> &maskHashes,
                                          const QSet<QString> &trackingHashes,
                                          QString *error)
{
    const QDir root(sidecarRoot());
    const auto removeStale = [&](const QString &subdir, const QString &suffix,
                                 const QSet<QString> &keep) {
        const QDir dir(root.filePath(subdir));
        if (!dir.exists())
            return true;
        const QStringList files =
            dir.entryList({QStringLiteral("*") + suffix}, QDir::Files);
        for (const QString &name : files) {
            const QString base = name.left(name.size() - suffix.size());
            if (keep.contains(base))
                continue;
            if (!QFile::remove(dir.filePath(name))) {
                setError(error, QStringLiteral("无法删除未引用的旁车文件：%1")
                                    .arg(dir.filePath(name)));
                return false;
            }
        }
        return true;
    };

    return removeStale(QStringLiteral("masks"), QStringLiteral(".rle"), maskHashes)
        && removeStale(QStringLiteral("tracking"), QStringLiteral(".json"),
                       trackingHashes);
}

} // namespace VideoEditor
