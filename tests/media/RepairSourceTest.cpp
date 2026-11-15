#include <gtest/gtest.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "RepairFrameSource.h"
#include "RemovalCache.h"

using namespace VideoEditor;

namespace {

QImage patchImage(int r, int g, int b)
{
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(QColor(r, g, b, 255));
    return image;
}

RemovalCacheManifest manifestFor(const QString &key, int64_t sourceInUs,
                                 int64_t sourceOutUs, int totalFrames)
{
    RemovalCacheManifest manifest;
    manifest.schema = QStringLiteral("m4.removal.cache.1");
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("clip-1");
    manifest.sourceInUs = sourceInUs;
    manifest.sourceOutUs = sourceOutUs;
    manifest.width = 64;
    manifest.height = 64;
    manifest.fps = Rational{30, 1};
    manifest.pipelineVersion = QStringLiteral("m4.0");
    manifest.status = QStringLiteral("complete");
    manifest.totalFrames = totalFrames;
    return manifest;
}

// Writes one chunk with explicit (possibly sparse) global frame indices and
// completes the manifest with that chunk.
bool writeCache(RemovalCache &cache, const QString &key,
                const QList<int> &frames, int64_t sourceInUs,
                int64_t sourceOutUs, int totalFrames,
                const QString &status = QStringLiteral("complete"),
                QString *error = nullptr)
{
    QList<QImage> patches;
    QList<QRect> crops;
    for (int i = 0; i < frames.size(); ++i) {
        patches.append(patchImage(200, 0, 0));
        crops.append(QRect(4, 4, 16, 16));
    }
    RemovalChunkIndex index;
    if (!cache.writeChunkFrames(key, 0, frames, patches, crops, &index, error))
        return false;
    RemovalCacheManifest manifest =
        manifestFor(key, sourceInUs, sourceOutUs, totalFrames);
    manifest.status = status;
    manifest.chunks.append(
        RemovalCacheChunk{0, 0, totalFrames, index.treeSha256});
    return cache.writeManifest(manifest, error);
}

} // namespace

TEST(RepairSource, MapsSourceTimeToPatchAndCrop)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('a'));
    QString error;
    ASSERT_TRUE(writeCache(cache, key, {0}, 0, 1'000'000, 30, "complete",
                           &error))
        << error.toStdString();

    auto source = RepairFrameSource::open(cache, key, &error);
    ASSERT_TRUE(source) << error.toStdString();
    EXPECT_TRUE(source->isComplete());
    EXPECT_EQ(source->frameCount(), 30);

    const auto *frame = source->frameAt(0, &error);
    ASSERT_TRUE(frame) << error.toStdString();
    EXPECT_EQ(frame->crop, QRect(4, 4, 16, 16));
    EXPECT_EQ(frame->sourceWidth, 64);
    EXPECT_EQ(frame->sourceHeight, 64);
    ASSERT_FALSE(frame->patch.isNull());
    EXPECT_EQ(frame->patch.size(), QSize(16, 16));
    EXPECT_EQ(frame->patch.pixelColor(0, 0), QColor(200, 0, 0, 255));
}

TEST(RepairSource, ReturnsNullForPartialOrMissingCache)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    QString error;

    const QString missing(64, QLatin1Char('b'));
    EXPECT_FALSE(RepairFrameSource::open(cache, missing, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("缺失")))
        << error.toStdString();

    const QString partial(64, QLatin1Char('c'));
    ASSERT_TRUE(writeCache(cache, partial, {0}, 0, 1'000'000, 30, "partial"))
        << "partial fixture";
    error.clear();
    EXPECT_FALSE(RepairFrameSource::open(cache, partial, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("缺失或损坏")))
        << error.toStdString();
}

TEST(RepairSource, LoadsSparseFramesAndSkipsFramesWithoutPatch)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('d'));
    QString error;
    ASSERT_TRUE(writeCache(cache, key, {0, 2}, 0, 1'000'000, 3, "complete",
                           &error))
        << error.toStdString();

    auto source = RepairFrameSource::open(cache, key, &error);
    ASSERT_TRUE(source) << error.toStdString();

    EXPECT_NE(source->frameAt(0, &error), nullptr) << error.toStdString();
    // Frame 1 has no hole and therefore no patch; this is not an error.
    // 50 ms is unambiguously inside frame 1 (frame 1 starts at 33.333 ms).
    error.clear();
    EXPECT_EQ(source->frameAt(50'000, &error), nullptr);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_NE(source->frameAt(66'667, &error), nullptr) << error.toStdString();

    // A repeated lookup is served from the QCache (same pointer).
    const auto *first = source->frameAt(0, &error);
    const auto *second = source->frameAt(0, &error);
    EXPECT_EQ(first, second);
}

TEST(RepairSource, ReturnsNullOutsideSourceRangeAndClampsInside)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('e'));
    QString error;
    // Patch frame 0 and the last frame (2) so the clamp below is observable.
    ASSERT_TRUE(writeCache(cache, key, {0, 2}, 1'000'000, 2'000'000, 3,
                           "complete", &error))
        << error.toStdString();

    auto source = RepairFrameSource::open(cache, key, &error);
    ASSERT_TRUE(source) << error.toStdString();

    EXPECT_EQ(source->frameAt(0, &error), nullptr);
    EXPECT_EQ(source->frameAt(999'999, &error), nullptr);
    EXPECT_NE(source->frameAt(1'000'000, &error), nullptr);
    EXPECT_EQ(source->frameAt(2'000'000, &error), nullptr);
    // Inside the range the frame index is clamped to totalFrames-1 instead of
    // indexing past the manifest.
    EXPECT_NE(source->frameAt(1'999'999, &error), nullptr);
}

TEST(RepairSource, RejectsManifestWhoseChunkIndexIsUnreadable)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('f'));
    QString error;
    ASSERT_TRUE(writeCache(cache, key, {0}, 0, 1'000'000, 30, "complete",
                           &error))
        << error.toStdString();
    ASSERT_TRUE(QFile::remove(
        QDir(cache.cacheDir(key))
            .filePath(QStringLiteral("chunks/0/index.json"))));

    EXPECT_FALSE(RepairFrameSource::open(cache, key, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("缺失或损坏")))
        << error.toStdString();
}

TEST(RepairSource, RejectsTruncatedCompleteManifest)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('a'));
    QString error;

    // Two real chunks, but the "complete" manifest lists only chunk 0. Every
    // missing frame must not be mistaken for a sparse (hole-free) frame.
    RemovalChunkIndex first;
    RemovalChunkIndex second;
    ASSERT_TRUE(cache.writeChunkFrames(key, 0, {0}, {patchImage(10, 20, 30)},
                                       {QRect(4, 4, 16, 16)}, &first, &error))
        << error.toStdString();
    ASSERT_TRUE(cache.writeChunkFrames(key, 1, {30}, {patchImage(10, 20, 30)},
                                       {QRect(4, 4, 16, 16)}, &second, &error))
        << error.toStdString();

    RemovalCacheManifest manifest =
        manifestFor(key, 0, 2'000'000, 60);
    manifest.chunks.append(RemovalCacheChunk{0, 0, 30, first.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();

    EXPECT_EQ(cache.state(key, &error), RemovalCacheState::Corrupt);
    EXPECT_FALSE(cache.verify(key, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("缺少分块")))
        << error.toStdString();
    error.clear();
    EXPECT_FALSE(RepairFrameSource::open(cache, key, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("缺少分块")))
        << error.toStdString();
}

TEST(RepairSource, RejectsOutOfBoundsPatchCrop)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('b'));
    QString error;

    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunkFrames(key, 0, {0}, {patchImage(1, 2, 3)},
                                       {QRect(60, 60, 32, 32)}, &index, &error))
        << error.toStdString();
    RemovalCacheManifest manifest = manifestFor(key, 0, 1'000'000, 30);
    manifest.chunks.append(
        RemovalCacheChunk{0, 0, 30, index.treeSha256});
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();
    // The manifest coverage itself is valid; only the crop is corrupt.
    EXPECT_EQ(cache.state(key, &error), RemovalCacheState::Complete);

    EXPECT_FALSE(RepairFrameSource::open(cache, key, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("裁剪区域无效")))
        << error.toStdString();
}
