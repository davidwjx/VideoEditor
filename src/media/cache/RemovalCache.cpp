#include "RemovalCache.h"

#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QCryptographicHash>

namespace VideoEditor {

namespace {

const QString kSchema = QStringLiteral("m4.removal.cache.1");

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool isValidKey(const QString &key)
{
    static const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
    return hex64.match(key).hasMatch();
}

bool isValidStatus(const QString &status)
{
    return status == QStringLiteral("complete")
        || status == QStringLiteral("partial")
        || status == QStringLiteral("failed");
}

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
            .toHex());
}

qint64 directorySize(const QString &path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        total += QFileInfo(it.next()).size();
    return total;
}

// Sorted relative path + size + file SHA-256, each NUL separated. The chunk
// directory is renamed atomically after this is computed, so relative names
// are stable between writeChunk and verify.
QString treeHash(const QString &dirPath)
{
    const QDir dir(dirPath);
    if (!dir.exists())
        return QString();

    QStringList relativePaths;
    QDirIterator it(dirPath, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        relativePaths.append(dir.relativeFilePath(it.next()));
    std::sort(relativePaths.begin(), relativePaths.end());

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &relative : relativePaths) {
        const QFileInfo info(dir.filePath(relative));
        hash.addData(relative.toUtf8());
        hash.addData("\0", 1);
        hash.addData(QByteArray::number(info.size()));
        hash.addData("\0", 1);
        hash.addData(fileSha256(info.absoluteFilePath()).toLatin1());
        hash.addData("\0", 1);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject cropToJson(const QRect &crop)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), crop.x());
    object.insert(QStringLiteral("y"), crop.y());
    object.insert(QStringLiteral("width"), crop.width());
    object.insert(QStringLiteral("height"), crop.height());
    return object;
}

QRect cropFromJson(const QJsonObject &object)
{
    return QRect(object.value(QStringLiteral("x")).toInt(),
                 object.value(QStringLiteral("y")).toInt(),
                 object.value(QStringLiteral("width")).toInt(),
                 object.value(QStringLiteral("height")).toInt());
}

QJsonObject indexToJson(const RemovalChunkIndex &index)
{
    QJsonObject object;
    object.insert(QStringLiteral("index"), index.index);
    object.insert(QStringLiteral("firstFrame"), index.firstFrame);
    QJsonArray frames;
    for (const RemovalChunkFrame &frame : index.frames) {
        QJsonObject item;
        item.insert(QStringLiteral("frame"), frame.frame);
        item.insert(QStringLiteral("crop"), cropToJson(frame.crop));
        item.insert(QStringLiteral("file"), frame.file);
        item.insert(QStringLiteral("sha256"), frame.sha256);
        frames.append(item);
    }
    object.insert(QStringLiteral("frames"), frames);
    return object;
}

std::optional<RemovalChunkIndex> indexFromJson(const QJsonObject &object)
{
    if (!object.contains(QStringLiteral("index"))
        || !object.contains(QStringLiteral("firstFrame"))
        || !object.value(QStringLiteral("frames")).isArray())
        return std::nullopt;

    RemovalChunkIndex index;
    index.index = object.value(QStringLiteral("index")).toInt();
    index.firstFrame = object.value(QStringLiteral("firstFrame")).toInt();
    for (const QJsonValue &value : object.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject item = value.toObject();
        RemovalChunkFrame frame;
        frame.frame = item.value(QStringLiteral("frame")).toInt();
        frame.crop = cropFromJson(item.value(QStringLiteral("crop")).toObject());
        frame.file = item.value(QStringLiteral("file")).toString();
        frame.sha256 = item.value(QStringLiteral("sha256")).toString();
        index.frames.append(frame);
    }
    return index;
}

QJsonObject manifestToJson(const RemovalCacheManifest &manifest)
{
    QJsonObject object;
    object.insert(QStringLiteral("schema"),
                  manifest.schema.isEmpty() ? kSchema : manifest.schema);
    object.insert(QStringLiteral("cacheKey"), manifest.cacheKey);
    object.insert(QStringLiteral("clipId"), manifest.clipId);
    object.insert(QStringLiteral("sourceInUs"), double(manifest.sourceInUs));
    object.insert(QStringLiteral("sourceOutUs"), double(manifest.sourceOutUs));
    object.insert(QStringLiteral("width"), manifest.width);
    object.insert(QStringLiteral("height"), manifest.height);
    QJsonObject fps;
    fps.insert(QStringLiteral("num"), manifest.fps.num);
    fps.insert(QStringLiteral("den"), manifest.fps.den);
    object.insert(QStringLiteral("fps"), fps);
    object.insert(QStringLiteral("trackingModelId"), manifest.trackingModelId);
    object.insert(QStringLiteral("inpaintModelId"), manifest.inpaintModelId);
    object.insert(QStringLiteral("pipelineVersion"), manifest.pipelineVersion);
    object.insert(QStringLiteral("status"), manifest.status);
    object.insert(QStringLiteral("totalFrames"), manifest.totalFrames);
    object.insert(QStringLiteral("createdAt"), manifest.createdAt);
    QJsonArray chunks;
    for (const RemovalCacheChunk &chunk : manifest.chunks) {
        QJsonObject item;
        item.insert(QStringLiteral("index"), chunk.index);
        item.insert(QStringLiteral("firstFrame"), chunk.firstFrame);
        item.insert(QStringLiteral("frameCount"), chunk.frameCount);
        item.insert(QStringLiteral("treeSha256"), chunk.treeSha256);
        chunks.append(item);
    }
    object.insert(QStringLiteral("chunks"), chunks);
    return object;
}

std::optional<RemovalCacheManifest> manifestFromJson(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("cacheKey")).isString()
        || object.value(QStringLiteral("cacheKey")).toString().isEmpty()
        || !object.value(QStringLiteral("status")).isString()
        || !isValidStatus(object.value(QStringLiteral("status")).toString())
        || !object.value(QStringLiteral("chunks")).isArray())
        return std::nullopt;

    RemovalCacheManifest manifest;
    manifest.schema = object.value(QStringLiteral("schema")).toString();
    manifest.cacheKey = object.value(QStringLiteral("cacheKey")).toString();
    manifest.clipId = object.value(QStringLiteral("clipId")).toString();
    manifest.sourceInUs = int64_t(object.value(QStringLiteral("sourceInUs")).toDouble());
    manifest.sourceOutUs = int64_t(object.value(QStringLiteral("sourceOutUs")).toDouble());
    manifest.width = object.value(QStringLiteral("width")).toInt();
    manifest.height = object.value(QStringLiteral("height")).toInt();
    const QJsonObject fps = object.value(QStringLiteral("fps")).toObject();
    manifest.fps.num = fps.value(QStringLiteral("num")).toInt();
    manifest.fps.den = fps.value(QStringLiteral("den")).toInt();
    if (manifest.fps.den <= 0)
        manifest.fps.den = 1;
    manifest.trackingModelId = object.value(QStringLiteral("trackingModelId")).toString();
    manifest.inpaintModelId = object.value(QStringLiteral("inpaintModelId")).toString();
    manifest.pipelineVersion = object.value(QStringLiteral("pipelineVersion")).toString();
    manifest.status = object.value(QStringLiteral("status")).toString();
    manifest.totalFrames = object.value(QStringLiteral("totalFrames")).toInt();
    manifest.createdAt = object.value(QStringLiteral("createdAt")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("chunks")).toArray()) {
        const QJsonObject item = value.toObject();
        RemovalCacheChunk chunk;
        chunk.index = item.value(QStringLiteral("index")).toInt();
        chunk.firstFrame = item.value(QStringLiteral("firstFrame")).toInt();
        chunk.frameCount = item.value(QStringLiteral("frameCount")).toInt();
        chunk.treeSha256 = item.value(QStringLiteral("treeSha256")).toString();
        manifest.chunks.append(chunk);
    }
    return manifest;
}

QString chunkDir(const QString &cacheDir, int index)
{
    return QDir(cacheDir).filePath(QStringLiteral("chunks/%1").arg(index));
}

QString manifestPath(const QString &cacheDir)
{
    return QDir(cacheDir).filePath(QStringLiteral("manifest.json"));
}

} // namespace

RemovalCache::RemovalCache(QString root)
    : m_root(std::move(root))
{
}

QString RemovalCache::cacheDir(const QString &cacheKey) const
{
    if (!isValidKey(cacheKey))
        return QString();
    return QDir(m_root).filePath(cacheKey);
}

bool RemovalCache::ensureDir(const QString &cacheKey, QString *error) const
{
    const QString dir = cacheDir(cacheKey);
    if (dir.isEmpty()) {
        setError(error, QStringLiteral("修复缓存键无效：需要 64 位十六进制哈希。"));
        return false;
    }
    if (!QDir().mkpath(QDir(dir).filePath(QStringLiteral("chunks")))) {
        setError(error, QStringLiteral("无法创建修复缓存目录：%1").arg(dir));
        return false;
    }
    return true;
}

bool RemovalCache::writeChunk(const QString &cacheKey, int chunkIndex,
                              int firstFrame, const QList<QImage> &patches,
                              const QList<QRect> &crops,
                              RemovalChunkIndex *written, QString *error)
{
    if (firstFrame < 0) {
        setError(error, QStringLiteral("修复缓存分块序号无效。"));
        return false;
    }
    QList<int> frameIndices;
    frameIndices.reserve(patches.size());
    for (int i = 0; i < patches.size(); ++i)
        frameIndices.append(firstFrame + i);
    return writeChunkFrames(cacheKey, chunkIndex, frameIndices, patches, crops,
                            written, error);
}

bool RemovalCache::writeChunkFrames(const QString &cacheKey, int chunkIndex,
                                    const QList<int> &frameIndices,
                                    const QList<QImage> &patches,
                                    const QList<QRect> &crops,
                                    RemovalChunkIndex *written, QString *error)
{
    if (patches.size() != crops.size()
        || patches.size() != frameIndices.size()) {
        setError(error, QStringLiteral("修复缓存分块写入失败：帧与裁剪区域数量不一致。"));
        return false;
    }
    if (chunkIndex < 0) {
        setError(error, QStringLiteral("修复缓存分块序号无效。"));
        return false;
    }
    for (int frameIndex : frameIndices) {
        if (frameIndex < 0) {
            setError(error, QStringLiteral("修复缓存分块帧序号无效。"));
            return false;
        }
    }
    if (!ensureDir(cacheKey, error))
        return false;

    // A replacement chunk must never be served from the parsed-index cache.
    invalidateChunkIndexCache(cacheKey, chunkIndex);

    const QString dir = cacheDir(cacheKey);
    const QString chunksDir = QDir(dir).filePath(QStringLiteral("chunks"));
    const QString tmp = QDir(chunksDir).filePath(
        QStringLiteral(".tmp-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QDir().mkpath(tmp)) {
        setError(error, QStringLiteral("无法创建修复缓存临时目录：%1").arg(tmp));
        return false;
    }

    const auto fail = [&](const QString &message) {
        QDir(tmp).removeRecursively();
        setError(error, message);
        return false;
    };

    RemovalChunkIndex index;
    index.index = chunkIndex;
    index.firstFrame =
        frameIndices.isEmpty() ? chunkIndex * 30 : frameIndices.first();

    for (int i = 0; i < patches.size(); ++i) {
        // Patches carry feathered alpha; normalize before PNG so an
        // alpha-less caller format cannot silently become opaque.
        QImage image = patches.at(i);
        if (!image.isNull() && image.format() != QImage::Format_ARGB32)
            image = image.convertToFormat(QImage::Format_ARGB32);
        if (image.isNull())
            return fail(QStringLiteral("修复缓存分块写入失败：第 %1 帧为空图像。").arg(i));

        const QString fileName =
            QStringLiteral("frame_%1.png").arg(i, 6, 10, QLatin1Char('0'));
        const QString path = QDir(tmp).filePath(fileName);
        if (!image.save(path, "PNG"))
            return fail(QStringLiteral("修复缓存分块写入失败：无法保存第 %1 帧。").arg(i));

        RemovalChunkFrame frame;
        frame.frame = frameIndices.at(i);
        frame.crop = crops.at(i);
        frame.file = fileName;
        frame.sha256 = fileSha256(path);
        if (frame.sha256.isEmpty())
            return fail(QStringLiteral("修复缓存分块写入失败：无法读取第 %1 帧。").arg(i));
        index.frames.append(frame);
    }

    QSaveFile indexFile(QDir(tmp).filePath(QStringLiteral("index.json")));
    const QByteArray indexBytes =
        QJsonDocument(indexToJson(index)).toJson(QJsonDocument::Compact);
    if (!indexFile.open(QIODevice::WriteOnly)
        || indexFile.write(indexBytes) != indexBytes.size()
        || !indexFile.commit()) {
        return fail(QStringLiteral("修复缓存分块写入失败：无法写入分块索引。"));
    }

    index.treeSha256 = treeHash(tmp);
    if (index.treeSha256.isEmpty())
        return fail(QStringLiteral("修复缓存分块写入失败：无法计算分块哈希。"));

    const QString target = QDir(chunksDir).filePath(QString::number(chunkIndex));
    if (QDir(target).exists() && !QDir(target).removeRecursively())
        return fail(QStringLiteral("修复缓存分块写入失败：无法替换旧分块：%1").arg(target));
    if (!QDir().rename(tmp, target)) {
        invalidateChunkIndexCache(cacheKey, chunkIndex);
        setError(error, QStringLiteral("修复缓存分块写入失败：无法提交分块目录：%1").arg(target));
        return false; // Keep the temporary directory for diagnosis.
    }
    invalidateChunkIndexCache(cacheKey, chunkIndex);

    if (written)
        *written = index;
    return true;
}

bool RemovalCache::writeManifest(const RemovalCacheManifest &manifest,
                                 QString *error)
{
    const QString dir = cacheDir(manifest.cacheKey);
    if (dir.isEmpty()) {
        setError(error, QStringLiteral("修复缓存清单写入失败：缓存键无效。"));
        return false;
    }
    if (!isValidStatus(manifest.status)) {
        setError(error, QStringLiteral("修复缓存清单写入失败：状态无效：%1")
                            .arg(manifest.status));
        return false;
    }
    if (!QDir().mkpath(dir)) {
        setError(error, QStringLiteral("无法创建修复缓存目录：%1").arg(dir));
        return false;
    }

    const QByteArray bytes =
        QJsonDocument(manifestToJson(manifest)).toJson(QJsonDocument::Compact);
    QSaveFile file(manifestPath(dir));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size()
        || !file.commit()) {
        setError(error, QStringLiteral("修复缓存清单写入失败：%1").arg(file.fileName()));
        return false;
    }
    // Manifest is the commit point; force index revalidation on next read.
    invalidateChunkIndexCache(manifest.cacheKey);
    return true;
}

std::optional<RemovalCacheManifest> RemovalCache::readManifest(
    const QString &cacheKey, QString *error) const
{
    const QString dir = cacheDir(cacheKey);
    if (dir.isEmpty()) {
        setError(error, QStringLiteral("修复缓存键无效：需要 64 位十六进制哈希。"));
        return std::nullopt;
    }
    const QString path = manifestPath(dir);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("修复缓存清单不存在或无法读取：%1").arg(path));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("修复缓存清单损坏：%1").arg(path));
        return std::nullopt;
    }
    const auto manifest = manifestFromJson(document.object());
    if (!manifest)
        setError(error, QStringLiteral("修复缓存清单缺少必要字段：%1").arg(path));
    return manifest;
}

std::optional<RemovalChunkIndex> RemovalCache::cachedChunkIndex(
    const QString &cacheKey, int chunkIndex, QString *error) const
{
    const QString dir = cacheDir(cacheKey);
    if (dir.isEmpty()) {
        setError(error, QStringLiteral("修复缓存键无效：需要 64 位十六进制哈希。"));
        return std::nullopt;
    }

    {
        QMutexLocker locker(&m_indexMutex);
        const auto keyIt = m_indexCache.constFind(cacheKey);
        if (keyIt != m_indexCache.constEnd()) {
            const auto it = keyIt.value().constFind(chunkIndex);
            if (it != keyIt.value().constEnd())
                return it.value();
        }
    }

    const QString path =
        QDir(chunkDir(dir, chunkIndex)).filePath(QStringLiteral("index.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("修复缓存分块索引不存在：%1").arg(path));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("修复缓存分块索引损坏：%1").arg(path));
        return std::nullopt;
    }
    const auto index = indexFromJson(document.object());
    if (!index) {
        setError(error, QStringLiteral("修复缓存分块索引缺少必要字段：%1").arg(path));
        return std::nullopt;
    }

    {
        QMutexLocker locker(&m_indexMutex);
        m_indexCache[cacheKey].insert(chunkIndex, *index);
        ++m_chunkIndexReads;
    }
    return index;
}

std::optional<RemovalChunkIndex> RemovalCache::readChunkIndex(
    const QString &cacheKey, int chunkIndex, QString *error) const
{
    return cachedChunkIndex(cacheKey, chunkIndex, error);
}

int RemovalCache::chunkIndexReadCount() const
{
    QMutexLocker locker(&m_indexMutex);
    return m_chunkIndexReads;
}

void RemovalCache::invalidateChunkIndexCache(const QString &cacheKey,
                                             int chunkIndex) const
{
    QMutexLocker locker(&m_indexMutex);
    if (chunkIndex < 0) {
        m_indexCache.remove(cacheKey);
        return;
    }
    const auto it = m_indexCache.find(cacheKey);
    if (it != m_indexCache.end())
        it->remove(chunkIndex);
}

QImage RemovalCache::readFrameImage(const QString &cacheKey,
                                    const RemovalChunkFrame &frame,
                                    QString *error) const
{
    const QString dir = cacheDir(cacheKey);
    if (dir.isEmpty()) {
        setError(error, QStringLiteral("修复缓存键无效：需要 64 位十六进制哈希。"));
        return QImage();
    }
    if (frame.file.isEmpty() || frame.file.contains(QLatin1Char('/'))
        || frame.file.contains(QLatin1Char('\\'))
        || frame.file.contains(QStringLiteral(".."))) {
        setError(error, QStringLiteral("修复缓存帧文件名无效：%1").arg(frame.file));
        return QImage();
    }

    const QDir chunksDir(QDir(dir).filePath(QStringLiteral("chunks")));
    const QStringList candidates =
        chunksDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : candidates) {
        if (name.startsWith(QLatin1Char('.')))
            continue;
        bool ok = false;
        const int index = name.toInt(&ok);
        if (!ok)
            continue;
        QString indexError;
        const auto chunkIndex = readChunkIndex(cacheKey, index, &indexError);
        if (!chunkIndex)
            continue;
        for (const RemovalChunkFrame &candidate : chunkIndex->frames) {
            if (candidate.frame != frame.frame || candidate.file != frame.file)
                continue;

            const QString path = QDir(chunkDir(dir, index)).filePath(candidate.file);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                setError(error, QStringLiteral("无法读取修复缓存帧：%1").arg(path));
                return QImage();
            }
            const QByteArray bytes = file.readAll();
            file.close();
            if (!candidate.sha256.isEmpty()) {
                const QString actual = QString::fromLatin1(
                    QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
                if (actual.compare(candidate.sha256, Qt::CaseInsensitive) != 0) {
                    setError(error, QStringLiteral("修复缓存帧哈希不匹配：%1").arg(path));
                    return QImage();
                }
            }
            QImage image;
            if (!image.loadFromData(bytes, "PNG")) {
                setError(error, QStringLiteral("修复缓存帧无法解码：%1").arg(path));
                return QImage();
            }
            return image;
        }
    }

    setError(error, QStringLiteral("修复缓存中找不到帧 %1。").arg(frame.frame));
    return QImage();
}

RemovalCacheState RemovalCache::state(const QString &cacheKey,
                                      QString *reason) const
{
    const QString dir = cacheDir(cacheKey);
    if (dir.isEmpty()) {
        setError(reason, QStringLiteral("修复缓存键无效：需要 64 位十六进制哈希。"));
        return RemovalCacheState::Corrupt;
    }
    if (!QFileInfo::exists(manifestPath(dir))) {
        setError(reason, QStringLiteral("修复缓存尚未生成。"));
        return RemovalCacheState::Missing;
    }

    QString parseError;
    const auto manifest = readManifest(cacheKey, &parseError);
    if (!manifest) {
        setError(reason, parseError);
        return RemovalCacheState::Corrupt;
    }
    if (manifest->cacheKey != cacheKey) {
        setError(reason, QStringLiteral("修复缓存清单的缓存键不匹配。"));
        return RemovalCacheState::Missing;
    }
    if (manifest->status == QStringLiteral("partial")) {
        setError(reason, QStringLiteral("修复缓存不完整。"));
        return RemovalCacheState::Partial;
    }
    if (manifest->status == QStringLiteral("failed")) {
        setError(reason, QStringLiteral("修复缓存生成失败。"));
        return RemovalCacheState::Corrupt;
    }
    if (manifest->status != QStringLiteral("complete")) {
        setError(reason, QStringLiteral("修复缓存状态未知：%1").arg(manifest->status));
        return RemovalCacheState::Corrupt;
    }

    for (const RemovalCacheChunk &chunk : manifest->chunks) {
        const QString chunkPath = chunkDir(dir, chunk.index);
        if (!QDir(chunkPath).exists()
            || !QFileInfo::exists(QDir(chunkPath).filePath(QStringLiteral("index.json")))) {
            setError(reason, QStringLiteral("修复缓存缺少分块 %1。").arg(chunk.index));
            return RemovalCacheState::Corrupt;
        }
    }
    return RemovalCacheState::Complete;
}

bool RemovalCache::verify(const QString &cacheKey, QString *reason) const
{
    QString stateReason;
    if (state(cacheKey, &stateReason) != RemovalCacheState::Complete) {
        setError(reason, stateReason);
        return false;
    }

    const auto manifest = readManifest(cacheKey, reason);
    if (!manifest)
        return false;

    const QString dir = cacheDir(cacheKey);
    for (const RemovalCacheChunk &chunk : manifest->chunks) {
        const QString chunkPath = chunkDir(dir, chunk.index);
        const QString actual = treeHash(chunkPath);
        if (actual.isEmpty()
            || actual.compare(chunk.treeSha256, Qt::CaseInsensitive) != 0) {
            setError(reason, QStringLiteral("修复缓存分块 %1 校验失败：文件被修改或损坏。")
                                .arg(chunk.index));
            return false;
        }
    }
    return true;
}

bool RemovalCache::invalidateFrameRange(const QString &cacheKey, int firstFrame,
                                        int lastFrame, QString *error)
{
    const auto manifest = readManifest(cacheKey, error);
    if (!manifest)
        return false;
    if (lastFrame < firstFrame) {
        setError(error, QStringLiteral("修复缓存失效区间无效。"));
        return false;
    }

    const QString dir = cacheDir(cacheKey);
    QList<RemovalCacheChunk> kept;
    bool removed = false;
    for (const RemovalCacheChunk &chunk : manifest->chunks) {
        const int last = chunk.firstFrame + chunk.frameCount - 1;
        const bool intersects = chunk.frameCount > 0
            && chunk.firstFrame <= lastFrame && last >= firstFrame;
        if (intersects) {
            QDir(chunkDir(dir, chunk.index)).removeRecursively();
            removed = true;
        } else {
            kept.append(chunk);
        }
    }
    if (!removed)
        return true;

    invalidateChunkIndexCache(cacheKey);
    RemovalCacheManifest updated = *manifest;
    updated.chunks = kept;
    updated.status = QStringLiteral("partial");
    return writeManifest(updated, error);
}

bool RemovalCache::collectGarbage(const QSet<QString> &pinnedKeys,
                                  qint64 maxBytes, QString *error)
{
    const QDir rootDir(m_root);
    if (!rootDir.exists())
        return true;

    struct Entry {
        QString path;
        qint64 size = 0;
        QDateTime modified;
    };
    QList<Entry> entries;
    qint64 total = sizeOnDisk(error);
    const QStringList names =
        rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (!isValidKey(name) || pinnedKeys.contains(name))
            continue;
        const QString path = rootDir.filePath(name);
        entries.append(Entry{path, directorySize(path), QFileInfo(path).lastModified()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.modified < b.modified;
    });

    for (const Entry &entry : entries) {
        if (maxBytes > 0 && total <= maxBytes)
            break;
        if (!QDir(entry.path).removeRecursively()) {
            setError(error, QStringLiteral("无法删除修复缓存：%1").arg(entry.path));
            return false;
        }
        invalidateChunkIndexCache(QFileInfo(entry.path).fileName());
        total -= entry.size;
    }
    return true;
}

qint64 RemovalCache::sizeOnDisk(QString *error) const
{
    Q_UNUSED(error);
    if (!QDir(m_root).exists())
        return 0;
    return directorySize(m_root);
}

} // namespace VideoEditor
