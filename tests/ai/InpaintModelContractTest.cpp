#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_ORT) && VIDEOEDITOR_HAVE_ORT

#include <iostream>

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>

#include "LamaInpainter.h"
#include "MiGanInpainter.h"
#include "ModelRegistry.h"

using namespace VideoEditor;

namespace {

QString modelsManifestPath()
{
    const QString dir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (dir.isEmpty())
        return {};
    const QString path = dir + QStringLiteral("/models.manifest.json");
    return QFileInfo::exists(path) ? path : QString();
}

const AiModelRecord *findRecord(const ModelRegistry &registry,
                                const QString &id)
{
    return registry.model(id);
}

QString firstOnnxPath(const ModelRegistry &registry,
                      const AiModelRecord &record)
{
    for (auto it = record.files.constBegin(); it != record.files.constEnd();
         ++it) {
        if (it.value().path.endsWith(QLatin1String(".onnx"),
                                     Qt::CaseInsensitive)) {
            return QDir(registry.rootDir()).filePath(it.value().path);
        }
    }
    return {};
}

QImage texturedCrop(int width, int height, const BinaryMask &mask)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Textured but smooth background, with the hole painted magenta so
            // a no-op model would be detected.
            const int r = 60 + (x * 120) / width;
            const int g = 80 + (y * 120) / height;
            const int b = 90 + ((x + y) * 60) / (width + height);
            image.setPixel(x, y,
                           mask.at(x, y) ? qRgb(255, 0, 255) : qRgb(r, g, b));
        }
    }
    return image;
}

BinaryMask centeredHole(int width, int height)
{
    BinaryMask mask;
    mask.width = width;
    mask.height = height;
    mask.bits.assign(size_t(width) * size_t(height), 0);
    const QRect hole(width / 4, height / 4, width / 2, height / 2);
    for (int y = hole.top(); y <= hole.bottom(); ++y)
        for (int x = hole.left(); x <= hole.right(); ++x)
            mask.set(x, y, true);
    return mask;
}

qreal meanHoleDifference(const QImage &before, const QImage &after,
                         const BinaryMask &mask)
{
    qreal total = 0.0;
    int count = 0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (!mask.at(x, y))
                continue;
            const QColor a = before.pixelColor(x, y);
            const QColor b = after.pixelColor(x, y);
            total += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green())
                   + qAbs(a.blue() - b.blue());
            count += 3;
        }
    }
    return count == 0 ? 0.0 : total / count;
}

} // namespace

TEST(InpaintModelContract, MiGanRepairsSyntheticCrop)
{
    const QString manifestPath = modelsManifestPath();
    if (manifestPath.isEmpty())
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = findRecord(*registry, QStringLiteral("migan-512"));
    if (!record)
        GTEST_SKIP() << "migan-512 is not in the manifest";
    const QString path = firstOnnxPath(*registry, *record);
    if (path.isEmpty())
        GTEST_SKIP() << "migan-512 has no ONNX file";

    MiGanInpainter inpainter(path, 0, /*allowDml=*/true, /*allowCpu=*/true);
    if (!inpainter.isReady())
        GTEST_SKIP() << inpainter.lastError().toStdString();

    constexpr int kSize = 128;
    const BinaryMask mask = centeredHole(kSize, kSize);
    const QImage crop = texturedCrop(kSize, kSize, mask);
    QElapsedTimer clock;
    clock.start();
    const auto result = inpainter.repair(crop, mask, &error);
    std::cout << "MI-GAN 128x128 repair (" << inpainter.provider().toStdString()
              << "): " << clock.elapsed() << " ms" << std::endl;
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_EQ(result->rgba.size(), QSize(kSize, kSize));
    EXPECT_EQ(result->rgba.format(), QImage::Format_ARGB32);
    EXPECT_EQ(qAlpha(result->rgba.pixel(kSize / 2, kSize / 2)), 255);
    EXPECT_GT(meanHoleDifference(crop, result->rgba, mask), 10.0);
}

TEST(InpaintModelContract, MiGanPadsNonMultipleOfEightCrop)
{
    const QString manifestPath = modelsManifestPath();
    if (manifestPath.isEmpty())
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = findRecord(*registry, QStringLiteral("migan-512"));
    if (!record)
        GTEST_SKIP() << "migan-512 is not in the manifest";
    const QString path = firstOnnxPath(*registry, *record);
    if (path.isEmpty())
        GTEST_SKIP() << "migan-512 has no ONNX file";

    MiGanInpainter inpainter(path, 0, /*allowDml=*/true, /*allowCpu=*/true);
    if (!inpainter.isReady())
        GTEST_SKIP() << inpainter.lastError().toStdString();

    // 130x98 is not a multiple of 8: the adapter must reflect-pad, run and
    // crop the result back to the input size.
    constexpr int kWidth = 130;
    constexpr int kHeight = 98;
    const BinaryMask mask = centeredHole(kWidth, kHeight);
    const QImage crop = texturedCrop(kWidth, kHeight, mask);
    QElapsedTimer clock;
    clock.start();
    const auto result = inpainter.repair(crop, mask, &error);
    std::cout << "MI-GAN padded 130x98 repair: " << clock.elapsed() << " ms"
              << std::endl;
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_EQ(result->rgba.size(), QSize(kWidth, kHeight));
    EXPECT_GT(meanHoleDifference(crop, result->rgba, mask), 10.0);
}

TEST(InpaintModelContract, LamaTilesCropLargerThan512)
{
    const QString manifestPath = modelsManifestPath();
    if (manifestPath.isEmpty())
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record =
        findRecord(*registry, QStringLiteral("lama-512-int8-allowzero0"));
    if (!record)
        GTEST_SKIP() << "lama-512-int8-allowzero0 is not in the manifest";
    const QString path = firstOnnxPath(*registry, *record);
    if (path.isEmpty())
        GTEST_SKIP() << "lama-512-int8-allowzero0 has no ONNX file";

    LamaInpainter inpainter(path, 0);
    if (!inpainter.isReady())
        GTEST_SKIP() << inpainter.lastError().toStdString();

    // Two 512 tiles with a 64 px overlap and feather blending.
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    const BinaryMask mask = centeredHole(kWidth, kHeight);
    const QImage crop = texturedCrop(kWidth, kHeight, mask);
    QElapsedTimer clock;
    clock.start();
    const auto result = inpainter.repair(crop, mask, &error);
    std::cout << "LaMa tiled 640x360 repair: " << clock.elapsed() << " ms"
              << std::endl;
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_EQ(result->rgba.size(), QSize(kWidth, kHeight));
    EXPECT_EQ(qAlpha(result->rgba.pixel(kWidth / 2, kHeight / 2)), 255);
    EXPECT_GT(meanHoleDifference(crop, result->rgba, mask), 10.0);
}

TEST(InpaintModelContract, LamaRepairsSyntheticCrop)
{
    const QString manifestPath = modelsManifestPath();
    if (manifestPath.isEmpty())
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record =
        findRecord(*registry, QStringLiteral("lama-512-int8-allowzero0"));
    if (!record)
        GTEST_SKIP() << "lama-512-int8-allowzero0 is not in the manifest";
    const QString path = firstOnnxPath(*registry, *record);
    if (path.isEmpty())
        GTEST_SKIP() << "lama-512-int8-allowzero0 has no ONNX file";

    LamaInpainter inpainter(path, 0);
    if (!inpainter.isReady())
        GTEST_SKIP() << inpainter.lastError().toStdString();
    EXPECT_EQ(inpainter.provider(), QStringLiteral("cpu"));

    constexpr int kSize = 512;
    const BinaryMask mask = centeredHole(kSize, kSize);
    const QImage crop = texturedCrop(kSize, kSize, mask);
    QElapsedTimer clock;
    clock.start();
    const auto result = inpainter.repair(crop, mask, &error);
    std::cout << "LaMa 512x512 repair (" << inpainter.provider().toStdString()
              << "): " << clock.elapsed() << " ms" << std::endl;
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_EQ(result->rgba.size(), QSize(kSize, kSize));
    EXPECT_EQ(result->rgba.format(), QImage::Format_ARGB32);
    EXPECT_EQ(qAlpha(result->rgba.pixel(kSize / 2, kSize / 2)), 255);
    EXPECT_GT(meanHoleDifference(crop, result->rgba, mask), 10.0);
}

#endif // VIDEOEDITOR_HAVE_ORT
