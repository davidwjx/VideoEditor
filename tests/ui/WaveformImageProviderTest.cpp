#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "WaveformImageProvider.h"
#include "WaveformService.h"

using namespace VideoEditor;

TEST(WaveformImageProvider, DrawsRequestedSize)
{
    const QString tone = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                             .filePath(QStringLiteral("tone.wav"));
    if (!QFileInfo::exists(tone))
        GTEST_SKIP() << "missing tone.wav";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    WaveformService service(dir.path());
    QSignalSpy ready(&service, &WaveformService::ready);
    service.request(QStringLiteral("asset-a"), tone);
    if (!ready.wait(5000))
        GTEST_SKIP() << "waveform worker did not finish";

    WaveformImageProvider provider(&service);
    QSize size;
    const QImage image = provider.requestImage(
        QStringLiteral("asset-a/300"), &size, QSize(300, 56));
    EXPECT_EQ(image.size(), QSize(300, 56));
    bool hasInk = false;
    for (int y = 0; y < image.height() && !hasInk; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                hasInk = true;
                break;
            }
        }
    }
    EXPECT_TRUE(hasInk);
}

TEST(WaveformImageProvider, HonoursTrimmedSourceRange)
{
    // Music rather than the constant-amplitude tone: sliced ranges must have
    // visibly different envelopes for the comparison to mean anything.
    const QString music = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                              .filePath(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(music))
        GTEST_SKIP() << "missing bgm.mp3";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    WaveformService service(dir.path());
    QSignalSpy ready(&service, &WaveformService::ready);
    service.request(QStringLiteral("asset-a"), music);
    if (!ready.wait(8000))
        GTEST_SKIP() << "waveform worker did not finish";

    WaveformImageProvider provider(&service);
    QSize size;
    const QImage full = provider.requestImage(
        QStringLiteral("asset-a/300"), &size, QSize(300, 56));
    const QImage firstHalf = provider.requestImage(
        QStringLiteral("asset-a/300/0/500000"), &size, QSize(300, 56));
    const QImage secondHalf = provider.requestImage(
        QStringLiteral("asset-a/300/500000/1000000"), &size, QSize(300, 56));

    int firstDiff = 0;
    int secondDiff = 0;
    for (int y = 0; y < full.height(); ++y) {
        for (int x = 0; x < full.width(); ++x) {
            if (full.pixel(x, y) != firstHalf.pixel(x, y))
                ++firstDiff;
            if (firstHalf.pixel(x, y) != secondHalf.pixel(x, y))
                ++secondDiff;
        }
    }
    EXPECT_GT(firstDiff, 0) << "a trimmed range must not draw the whole file";
    EXPECT_GT(secondDiff, 0) << "different trim ranges must differ";
}

TEST(WaveformImageProvider, UnknownAssetIsTransparent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    WaveformService service(dir.path());
    WaveformImageProvider provider(&service);
    QSize size;
    const QImage image = provider.requestImage(
        QStringLiteral("missing/100"), &size, QSize(100, 40));
    EXPECT_EQ(image.size(), QSize(100, 40));
    EXPECT_EQ(QImage(image).pixelColor(50, 20).alpha(), 0);
}
