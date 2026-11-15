#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_ORT) && VIDEOEDITOR_HAVE_ORT

#include <algorithm>
#include <iostream>

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QList>

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
    // Only an absent environment variable may skip. If models are installed
    // but a session fails, the test must fail and surface the adapter error.
    return dir + QStringLiteral("/models.manifest.json");
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

qreal meanMaskDifference(const QImage &before, const QImage &after,
                         const BinaryMask &mask, bool inside)
{
    qreal total = 0.0;
    int count = 0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (mask.at(x, y) != inside)
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

qreal meanHoleDifference(const QImage &before, const QImage &after,
                         const BinaryMask &mask)
{
    return meanMaskDifference(before, after, mask, /*inside=*/true);
}

qreal meanOutsideDifference(const QImage &before, const QImage &after,
                            const BinaryMask &mask)
{
    return meanMaskDifference(before, after, mask, /*inside=*/false);
}

QImage swapRedBlue(const QImage &image)
{
    QImage swapped = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < swapped.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(swapped.scanLine(y));
        for (int x = 0; x < swapped.width(); ++x) {
            line[x] = qRgba(qBlue(line[x]), qGreen(line[x]), qRed(line[x]),
                            qAlpha(line[x]));
        }
    }
    return swapped;
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
    ASSERT_NE(record, nullptr) << "migan-512 is missing from the installed manifest";
    const QString path = firstOnnxPath(*registry, *record);
    ASSERT_FALSE(path.isEmpty()) << "migan-512 has no ONNX file";

    MiGanInpainter inpainter(path, 0, /*allowDml=*/true, /*allowCpu=*/true);
    ASSERT_TRUE(inpainter.isReady()) << inpainter.lastError().toStdString();
    if (inpainter.provider() != QStringLiteral("dml")) {
        // DML is genuinely unavailable: the CPU retry must record why.
        EXPECT_TRUE(inpainter.usingCpuFallback());
        EXPECT_FALSE(inpainter.cpuFallbackReason().isEmpty());
    }

    constexpr int kSize = 128;
    const BinaryMask mask = centeredHole(kSize, kSize);
    const QImage crop = texturedCrop(kSize, kSize, mask);
    QElapsedTimer clock;
    clock.start();
    const auto result = inpainter.repair(crop, mask, &error);
    const qint64 coldMs = clock.elapsed();
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_EQ(result->rgba.size(), QSize(kSize, kSize));
    EXPECT_EQ(result->rgba.format(), QImage::Format_ARGB32);
    EXPECT_EQ(qAlpha(result->rgba.pixel(kSize / 2, kSize / 2)), 255);
    EXPECT_GT(meanHoleDifference(crop, result->rgba, mask), 10.0);
    std::cout << "MI-GAN outside-hole mean diff="
              << meanOutsideDifference(crop, result->rgba, mask)
              << " swappedRB="
              << meanOutsideDifference(crop, swapRedBlue(result->rgba), mask)
              << std::endl;

    // Diagnostic: does the masked hole content bias the fill? The expected
    // smooth background at the centre of texturedCrop is (120,140,120).
    const QRgb expectedCenter = qRgb(120, 140, 120);
    const QRgb contentCenter = result->rgba.pixel(kSize / 2, kSize / 2);
    QImage zeroed = crop;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            if (mask.at(x, y))
                zeroed.setPixel(x, y, qRgb(0, 0, 0));
    const auto zeroedResult = inpainter.repair(zeroed, mask, &error);
    const QRgb zeroedCenter =
        zeroedResult ? zeroedResult->rgba.pixel(kSize / 2, kSize / 2)
                     : qRgb(0, 0, 0);
    std::cout << "MI-GAN hole centre: content="
              << qRed(contentCenter) << "," << qGreen(contentCenter) << ","
              << qBlue(contentCenter) << " zeroed=" << qRed(zeroedCenter)
              << "," << qGreen(zeroedCenter) << "," << qBlue(zeroedCenter)
              << " expected=" << qRed(expectedCenter) << ","
              << qGreen(expectedCenter) << "," << qBlue(expectedCenter)
              << std::endl;

    // Steady-state throughput: 3 warmups + 5 measured runs, gated on p50.
    for (int i = 0; i < 3; ++i) {
        const auto warm = inpainter.repair(crop, mask, &error);
        ASSERT_TRUE(warm.has_value()) << error.toStdString();
    }
    QList<double> warmMs;
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer runClock;
        runClock.start();
        const auto run = inpainter.repair(crop, mask, &error);
        warmMs.append(double(runClock.nsecsElapsed()) / 1'000'000.0);
        ASSERT_TRUE(run.has_value()) << error.toStdString();
    }
    std::sort(warmMs.begin(), warmMs.end());
    const double warmP50 = warmMs.at(warmMs.size() / 2);
    const double warmFps = warmP50 > 0.0 ? 1000.0 / warmP50 : 0.0;
    std::cout << "MI-GAN 128x128 repair (" << inpainter.provider().toStdString()
              << "): cold=" << coldMs << " ms, warm_p50=" << warmP50
              << " ms (" << warmFps << " fps) over 5 runs" << std::endl;
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
    ASSERT_NE(record, nullptr) << "migan-512 is missing from the installed manifest";
    const QString path = firstOnnxPath(*registry, *record);
    ASSERT_FALSE(path.isEmpty()) << "migan-512 has no ONNX file";

    MiGanInpainter inpainter(path, 0, /*allowDml=*/true, /*allowCpu=*/true);
    ASSERT_TRUE(inpainter.isReady()) << inpainter.lastError().toStdString();
    if (inpainter.provider() != QStringLiteral("dml")) {
        EXPECT_TRUE(inpainter.usingCpuFallback());
        EXPECT_FALSE(inpainter.cpuFallbackReason().isEmpty());
    }

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
    ASSERT_NE(record, nullptr)
        << "lama-512-int8-allowzero0 is missing from the installed manifest";
    const QString path = firstOnnxPath(*registry, *record);
    ASSERT_FALSE(path.isEmpty())
        << "lama-512-int8-allowzero0 has no ONNX file";

    LamaInpainter inpainter(path, 0);
    ASSERT_TRUE(inpainter.isReady()) << inpainter.lastError().toStdString();

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
    ASSERT_NE(record, nullptr)
        << "lama-512-int8-allowzero0 is missing from the installed manifest";
    const QString path = firstOnnxPath(*registry, *record);
    ASSERT_FALSE(path.isEmpty())
        << "lama-512-int8-allowzero0 has no ONNX file";

    LamaInpainter inpainter(path, 0);
    ASSERT_TRUE(inpainter.isReady()) << inpainter.lastError().toStdString();
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

// CPU fallback gate: with DML disabled MI-GAN must still repair one frame on
// the CPU provider (no silent DML retry).
TEST(InpaintModelContract, MiGanRepairsOneFrameOnCpuWhenDmlDisabled)
{
    const QString manifestPath = modelsManifestPath();
    if (manifestPath.isEmpty())
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record =
        findRecord(*registry, QStringLiteral("migan-512"));
    ASSERT_NE(record, nullptr)
        << "migan-512 is missing from the installed manifest";
    const QString path = firstOnnxPath(*registry, *record);
    ASSERT_FALSE(path.isEmpty()) << "migan-512 has no ONNX file";

    MiGanInpainter inpainter(path, 0, /*allowDml=*/false, /*allowCpu=*/true);
    ASSERT_TRUE(inpainter.isReady()) << inpainter.lastError().toStdString();
    EXPECT_EQ(inpainter.provider(), QStringLiteral("cpu"));

    constexpr int kSize = 128;
    const BinaryMask mask = centeredHole(kSize, kSize);
    const QImage crop = texturedCrop(kSize, kSize, mask);
    QElapsedTimer clock;
    clock.start();
    const auto result = inpainter.repair(crop, mask, &error);
    std::cout << "MI-GAN 128x128 CPU-only repair: " << clock.elapsed()
              << " ms" << std::endl;
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_EQ(result->rgba.size(), QSize(kSize, kSize));
    EXPECT_GT(meanHoleDifference(crop, result->rgba, mask), 10.0);
}

#endif // VIDEOEDITOR_HAVE_ORT
