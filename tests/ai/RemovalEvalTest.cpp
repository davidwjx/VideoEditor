#include <gtest/gtest.h>

#include <QFileInfo>
#include <QTemporaryDir>

#include "MediaProbe.h"
#include "RemovalCacheKey.h"
#include "RemovalEvalMetrics.h"

using namespace VideoEditor;

// Pure metric helpers: no models, no codecs, no GUI.
TEST(RemovalEvalMetrics, ComputesIouPsnrAndTemporalDelta)
{
    BinaryMask a;
    a.width = a.height = 8;
    a.bits.assign(64, 0);
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 6; ++x)
            a.set(x, y, true);

    BinaryMask b = a;
    EXPECT_DOUBLE_EQ(maskIou(a, b), 1.0);
    b.set(0, 0, true);
    EXPECT_LT(maskIou(a, b), 1.0);
    EXPECT_GT(maskIou(a, b), 0.9);

    QImage black(8, 8, QImage::Format_ARGB32);
    black.fill(Qt::black);
    QImage white = black;
    white.fill(Qt::white);
    EXPECT_DOUBLE_EQ(psnrInMask(black, black, a), 100.0);
    EXPECT_LT(psnrInMask(black, white, a), 1.0);

    QList<QImage> frames{black, white, black};
    EXPECT_GT(temporalDeltaInMask(frames, a), 0.0);
    EXPECT_EQ(temporalDeltaInMask({black}, a), 0.0);
}

TEST(RemovalEvalMetrics, RejectsMismatchedGeometry)
{
    BinaryMask a;
    a.width = a.height = 4;
    a.bits.assign(16, 255);
    BinaryMask b = a;
    b.width = 5;
    EXPECT_DOUBLE_EQ(maskIou(a, b), 0.0);

    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(Qt::black);
    QImage other(5, 5, QImage::Format_ARGB32);
    other.fill(Qt::black);
    EXPECT_DOUBLE_EQ(psnrInMask(image, other, a), 0.0);
}

// The committed corpus must keep the geometry the evaluation relies on.
TEST(RemovalEval, FixturesDecodeAtExpectedGeometry)
{
    const QString fixtures =
        QStringLiteral(VIDEOEDITOR_FIXTURE_DIR) + QStringLiteral("/removal");
    const QString video = fixtures + QStringLiteral("/static_logo.mp4");
    if (!QFileInfo::exists(video))
        GTEST_SKIP() << "removal fixtures missing; run generate_fixtures.ps1";

    QString error;
    const auto info = MediaProbe::probe(video, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->width, 640);
    EXPECT_EQ(info->height, 360);
    EXPECT_TRUE((info->frameRate == Rational{30, 1}));
    EXPECT_NEAR(info->durationUs, 5'000'000, 40'000);

    const QString mask = fixtures + QStringLiteral("/static_logo_mask.mp4");
    ASSERT_TRUE(QFileInfo::exists(mask));
    QString maskError;
    const auto maskInfo = MediaProbe::probe(mask, &maskError);
    ASSERT_TRUE(maskInfo.has_value()) << maskError.toStdString();
    EXPECT_EQ(maskInfo->width, 640);
    EXPECT_EQ(maskInfo->height, 360);

    const QString clean = fixtures + QStringLiteral("/clean_background.mp4");
    ASSERT_TRUE(QFileInfo::exists(clean));
}

TEST(RemovalEval, CacheBudgetIsAtMostTwiceSource)
{
    const qint64 sourceBytes = 2'000'000; // ~5 s 640x360 mpeg4 upper bound
    EXPECT_EQ(removalCacheBudgetBytes(sourceBytes), sourceBytes * 2);
    EXPECT_LE(removalCacheBudgetBytes(sourceBytes), sourceBytes * 2);
    EXPECT_EQ(removalCacheBudgetBytes(0), 0);
    EXPECT_EQ(removalCacheBudgetBytes(-1), 0);
}
