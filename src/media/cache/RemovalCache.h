#pragma once

#include <cstdint>
#include <optional>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QRect>
#include <QSet>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

enum class RemovalCacheState { Missing, Partial, Complete, Corrupt };

struct RemovalChunkFrame {
    int frame = 0;
    QRect crop;
    QString file;
    QString sha256;
};

struct RemovalChunkIndex {
    int index = 0;
    int firstFrame = 0;
    QList<RemovalChunkFrame> frames;
    QString treeSha256;
};

struct RemovalCacheChunk {
    int index = 0;
    int firstFrame = 0;
    int frameCount = 0;
    QString treeSha256;
};

struct RemovalCacheManifest {
    QString schema;
    QString cacheKey;
    QString clipId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    int width = 0;
    int height = 0;
    Rational fps;
    QString trackingModelId;
    QString inpaintModelId;
    QString pipelineVersion;
    QString status;
    QList<RemovalCacheChunk> chunks;
    int totalFrames = 0;
    QString createdAt;
};

// Content-addressed repair cache rooted outside the project. The manifest is
// written last and is the only commit point: chunks written before it are not
// visible to state()/verify().
class RemovalCache {
public:
    explicit RemovalCache(QString root);

    QString root() const { return m_root; }
    QString cacheDir(const QString &cacheKey) const;
    bool ensureDir(const QString &cacheKey, QString *error = nullptr) const;

    bool writeChunk(const QString &cacheKey, int chunkIndex, int firstFrame,
                    const QList<QImage> &patches, const QList<QRect> &crops,
                    RemovalChunkIndex *written, QString *error = nullptr);
    // Sparse variant: explicit global frame indices per patch. Used by the
    // engine because only frames with a non-empty mask produce a patch.
    bool writeChunkFrames(const QString &cacheKey, int chunkIndex,
                          const QList<int> &frameIndices,
                          const QList<QImage> &patches,
                          const QList<QRect> &crops,
                          RemovalChunkIndex *written,
                          QString *error = nullptr);
    bool writeManifest(const RemovalCacheManifest &manifest,
                       QString *error = nullptr);
    std::optional<RemovalCacheManifest> readManifest(
        const QString &cacheKey, QString *error = nullptr) const;
    std::optional<RemovalChunkIndex> readChunkIndex(
        const QString &cacheKey, int chunkIndex,
        QString *error = nullptr) const;
    QImage readFrameImage(const QString &cacheKey,
                          const RemovalChunkFrame &frame,
                          QString *error = nullptr) const;

    RemovalCacheState state(const QString &cacheKey,
                            QString *reason = nullptr) const;
    bool verify(const QString &cacheKey, QString *reason = nullptr) const;
    bool invalidateFrameRange(const QString &cacheKey, int firstFrame,
                              int lastFrame, QString *error = nullptr);
    bool collectGarbage(const QSet<QString> &pinnedKeys, qint64 maxBytes,
                        QString *error = nullptr);
    qint64 sizeOnDisk(QString *error = nullptr) const;

    // Diagnostics: number of chunk index files actually parsed. Repeated
    // readFrameImage/readChunkIndex calls for the same chunk must not grow it.
    int chunkIndexReadCount() const;

private:
    std::optional<RemovalChunkIndex> cachedChunkIndex(
        const QString &cacheKey, int chunkIndex, QString *error) const;
    void invalidateChunkIndexCache(const QString &cacheKey,
                                   int chunkIndex = -1) const;

    QString m_root;
    mutable QMutex m_indexMutex;
    mutable QHash<QString, QHash<int, RemovalChunkIndex>> m_indexCache;
    mutable int m_chunkIndexReads = 0;
};

} // namespace VideoEditor
