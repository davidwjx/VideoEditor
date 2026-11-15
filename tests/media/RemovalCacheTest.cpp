#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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
