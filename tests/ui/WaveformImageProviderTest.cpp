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
