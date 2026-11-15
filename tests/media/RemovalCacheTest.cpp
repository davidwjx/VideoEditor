#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include "CacheStore.h"
#include "RemovalCache.h"

using namespace VideoEditor;

namespace {
RemovalCacheManifest manifestFor(const QString &key)
{
    RemovalCacheManifest m;
    m.cacheKey = key;
    m.clipId = QStringLiteral("clip-1");
    m.sourceInUs = 0;
    m.sourceOutUs = 1'000'000;
    m.width = 64;
    m.height = 64;
    m.fps = Rational{30, 1};
    m.pipelineVersion = QStringLiteral("m4.0");
    m.status = QStringLiteral("complete");
    m.totalFrames = 3;
    return m;
}

QList<QImage> patches()
{
    QList<QImage> images;
    for (int i = 0; i < 3; ++i) {
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(QColor(10 * i, 20, 30, 255));
        images.append(image);
    }
    return images;
}
} // namespace

TEST(RemovalCache, WritesChunkAndManifestAndReadsBack)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('a'));
    QString error;

    RemovalChunkIndex index;
    const QList<QRect> crops(3, QRect(4, 4, 16, 16));
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(), crops, &index, &error))
        << error.toStdString();
    EXPECT_EQ(index.frames.size(), 3);

    RemovalCacheManifest manifest = manifestFor(key);
    RemovalCacheChunk chunk;
    chunk.index = 0;
    chunk.firstFrame = 0;
    chunk.frameCount = 3;
    chunk.treeSha256 = index.treeSha256;
    manifest.chunks.append(chunk);
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();

    EXPECT_EQ(cache.state(key), RemovalCacheState::Complete);
    QRect crop;
    const QImage image = cache.readFrameImage(key, index.frames[1], &error);
    ASSERT_FALSE(image.isNull()) << error.toStdString();
    EXPECT_EQ(image.size(), QSize(16, 16));
}

TEST(RemovalCache, DetectsPartialAndCorruptTree)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('b'));
    QString error;

    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    RemovalCacheManifest manifest = manifestFor(key);
    manifest.status = QStringLiteral("partial");
    RemovalCacheChunk chunk{0, 0, 3, index.treeSha256};
    manifest.chunks.append(chunk);
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    EXPECT_EQ(cache.state(key), RemovalCacheState::Partial);

    manifest.status = QStringLiteral("complete");
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    ASSERT_EQ(cache.state(key), RemovalCacheState::Complete);

    QFile file(cache.cacheDir(key) + QStringLiteral("/chunks/0/")
               + index.frames[0].file);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("broken", 6);
    file.close();
    EXPECT_EQ(cache.verify(key, &error), false);
    EXPECT_FALSE(error.isEmpty());
}

TEST(RemovalCache, GarbageCollectionNeverDeletesPinnedKeys)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString keep(64, QLatin1Char('c'));
    const QString drop(64, QLatin1Char('d'));
    QString error;
    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(keep, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    ASSERT_TRUE(cache.writeChunk(drop, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    cache.collectGarbage({keep}, 0, &error);
    EXPECT_TRUE(QFileInfo::exists(cache.cacheDir(keep)));
    EXPECT_FALSE(QFileInfo::exists(cache.cacheDir(drop)));
}

TEST(RemovalCache, PreservesAlphaFromNonArgb32Input)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('e'));
    QString error;

    QImage source(8, 8, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    source.setPixelColor(3, 3, QColor(200, 40, 10, 128));

    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, {source}, {QRect(0, 0, 8, 8)},
                                 &index, &error)) << error.toStdString();
    ASSERT_EQ(index.frames.size(), 1);

    const QImage read = cache.readFrameImage(key, index.frames[0], &error);
    ASSERT_FALSE(read.isNull()) << error.toStdString();
    EXPECT_EQ(read.format(), QImage::Format_ARGB32);
    EXPECT_EQ(read.pixelColor(3, 3).alpha(), 128);
    EXPECT_EQ(read.pixelColor(0, 0).alpha(), 0);
}

TEST(RemovalCache, ChunkIndexCacheAvoidsRepeatedReads)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('f'));
    QString error;

    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    const RemovalChunkFrame firstFrame = index.frames[0];
    RemovalCacheManifest manifest = manifestFor(key);
    manifest.chunks.append(RemovalCacheChunk{0, 0, 3, index.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    EXPECT_EQ(cache.chunkIndexReadCount(), 0);

    for (int i = 0; i < 5; ++i) {
        const QImage image = cache.readFrameImage(key, firstFrame, &error);
        ASSERT_FALSE(image.isNull()) << error.toStdString();
    }
    EXPECT_EQ(cache.chunkIndexReadCount(), 1);

    // A new chunk or manifest invalidation must force a reload.
    ASSERT_TRUE(cache.writeChunk(key, 1, 3, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    manifest.chunks.append(RemovalCacheChunk{1, 3, 3, index.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    const QImage image = cache.readFrameImage(key, firstFrame, &error);
    ASSERT_FALSE(image.isNull()) << error.toStdString();
    EXPECT_EQ(cache.chunkIndexReadCount(), 2);
}

TEST(RemovalCache, RejectsTraversalFrameFilename)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    RemovalChunkFrame frame;
    frame.frame = 0;
    frame.file = QStringLiteral("../evil.png");
    QString error;
    EXPECT_TRUE(cache.readFrameImage(QString(64, QLatin1Char('a')), frame, &error).isNull());
    EXPECT_FALSE(error.isEmpty());
}

TEST(RemovalCache, VerifyPassesForCompleteCache)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('b'));
    QString error;
    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    RemovalCacheManifest manifest = manifestFor(key);
    manifest.chunks.append(RemovalCacheChunk{0, 0, 3, index.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    EXPECT_TRUE(cache.verify(key, &error)) << error.toStdString();
}

TEST(RemovalCache, InvalidateFrameRangeMarksPartialAndDropsChunk)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('a'));
    QString error;
    RemovalChunkIndex first;
    RemovalChunkIndex second;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &first, &error));
    ASSERT_TRUE(cache.writeChunk(key, 1, 3, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &second, &error));
    RemovalCacheManifest manifest = manifestFor(key);
    manifest.totalFrames = 6;
    manifest.chunks.append(RemovalCacheChunk{0, 0, 3, first.treeSha256});
    manifest.chunks.append(RemovalCacheChunk{1, 3, 3, second.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error));

    ASSERT_TRUE(cache.invalidateFrameRange(key, 0, 2, &error)) << error.toStdString();
    EXPECT_EQ(cache.state(key), RemovalCacheState::Partial);
    EXPECT_FALSE(QFileInfo::exists(cache.cacheDir(key) + QStringLiteral("/chunks/0")));
    EXPECT_TRUE(QFileInfo::exists(cache.cacheDir(key) + QStringLiteral("/chunks/1")));
    EXPECT_FALSE(cache.readChunkIndex(key, 0, &error).has_value());
    EXPECT_TRUE(cache.readChunkIndex(key, 1, &error).has_value());
}

TEST(RemovalCache, RejectsUnknownManifestStatus)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    RemovalCacheManifest manifest = manifestFor(QString(64, QLatin1Char('a')));
    manifest.status = QStringLiteral("bogus");
    QString error;
    EXPECT_FALSE(cache.writeManifest(manifest, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(CacheStoreRemoval, PathsAreValidatedAndClearable)
{
    QTemporaryDir dir;
    CacheStore store(dir.path());
    EXPECT_EQ(store.removalRoot(), QDir(dir.path()).filePath(QStringLiteral("removal")));
    EXPECT_EQ(store.removalPath(QString(64, QLatin1Char('a'))),
              QDir(store.removalRoot()).filePath(QString(64, QLatin1Char('a'))));
    EXPECT_TRUE(store.removalPath(QStringLiteral("../escape")).isEmpty());
    EXPECT_TRUE(store.removalPath(QStringLiteral("not-a-hash")).isEmpty());

    QString error;
    ASSERT_TRUE(store.ensureRemovalDir(&error)) << error.toStdString();
    ASSERT_TRUE(QDir().mkpath(QDir(store.removalPath(QString(64, QLatin1Char('a')))).path()));
    ASSERT_TRUE(store.clearRemoval(&error)) << error.toStdString();
    EXPECT_FALSE(QDir(store.removalRoot()).exists());
}

TEST(RemovalCache, GarbageCollectionHonorsPositiveMaxBytes)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('d'));
    QString error;
    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));

    ASSERT_TRUE(cache.collectGarbage({}, cache.sizeOnDisk() + 1, &error));
    EXPECT_TRUE(QFileInfo::exists(cache.cacheDir(key)));

    ASSERT_TRUE(cache.collectGarbage({}, 1, &error));
    EXPECT_FALSE(QFileInfo::exists(cache.cacheDir(key)));
}
