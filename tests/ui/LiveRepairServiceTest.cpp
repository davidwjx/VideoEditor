#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <optional>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include "FrameReader.h"
#include "InpaintPipeline.h"
#include "LiveRepairService.h"
#include "MaskCodec.h"
#include "MediaProbe.h"

using namespace VideoEditor;

namespace {

// QTRY's timeout path returns without a gtest failure, so wait explicitly.
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

// Deterministic inpainter: paints the hole red, the pipeline adds the crop and
// feather exactly as it does for MI-GAN/LaMa.
class SolidInpainter : public Inpainter {
public:
    std::optional<PatchResult> repair(const QImage &crop,
                                      const BinaryMask &maskCrop,
                                      QString *) override
    {
        QImage out = crop.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x)
                if (maskCrop.at(x, y))
                    out.setPixelColor(x, y, QColor(200, 30, 30, 255));
        return PatchResult{out, QRect()};
    }
};

constexpr int kDisplay = 64;

QString maskPathFor(const QString &cacheDir, int frame)
{
    return QDir(QDir(cacheDir).filePath(QStringLiteral("tracking/masks")))
        .filePath(QStringLiteral("frame_%1.rle")
                      .arg(frame, 6, 10, QLatin1Char('0')));
}

bool writeMaskFor(const QString &cacheDir, int frame)
{
    const QString directory =
        QDir(cacheDir).filePath(QStringLiteral("tracking/masks"));
    if (!QDir().mkpath(directory))
        return false;
    BinaryMask mask;
    mask.width = kDisplay;
    mask.height = kDisplay;
    mask.bits.assign(size_t(kDisplay) * size_t(kDisplay), 0);
    for (int y = 24; y < 40; ++y)
        for (int x = 24; x < 40; ++x)
            mask.set(x, y, true);
    QFile file(maskPathFor(cacheDir, frame));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray encoded = MaskCodec::encode(mask);
    return !encoded.isEmpty() && file.write(encoded) == encoded.size();
}

struct Fixture {
    QString path;
    Rational fps{30, 1};
    int64_t sourceOutUs = 0;
    int frameCount = 0;
};

bool makeFixture(Fixture *fixture)
{
    fixture->path = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                        .filePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(fixture->path))
        return false;
    const auto info = MediaProbe::probe(fixture->path);
    if (!info || !info->hasVideo || !info->frameRate.isValid())
        return false;
    fixture->fps = info->frameRate;
    fixture->sourceOutUs = qMin<int64_t>(info->durationUs, 1'000'000);
    FrameReader reader;
    QString error;
    if (!reader.open(fixture->path, fixture->fps, 0, fixture->sourceOutUs,
                     QSize(kDisplay, kDisplay), &error)) {
        return false;
    }
    fixture->frameCount = reader.frameCount();
    return fixture->frameCount > 0;
}

LiveRepairSpec specFor(const Fixture &fixture, const QString &cacheDir)
{
    LiveRepairSpec spec;
    spec.assetPath = fixture.path;
    spec.cacheDir = cacheDir;
    spec.layerId = QStringLiteral("rl-live");
    spec.sourceInUs = 0;
    spec.sourceOutUs = fixture.sourceOutUs;
    spec.fps = fixture.fps;
    spec.displaySize = QSize(kDisplay, kDisplay);
    spec.parameters.mode = RepairMode::Fast;
    spec.parameters.processingWidth = kDisplay;
    spec.parameters.processingHeight = kDisplay;
    spec.featherPx = 4;
    spec.inpainterModelPath = QStringLiteral("not-used.inpaint.onnx");
    spec.mode = RepairMode::Fast;
    return spec;
}

int64_t targetUsFor(int frame, Rational fps)
{
    return framesToMicros(frame, fps) + 1000;
}

} // namespace

TEST(LiveRepairService, RepairsTargetFirstMapsTimesAndBoundsCache)
{
    Fixture fixture;
    if (!makeFixture(&fixture))
        GTEST_SKIP() << "h264.mp4 could not be opened";

    QTemporaryDir cacheDir;
    ASSERT_TRUE(cacheDir.isValid());
    for (int frame = 0; frame < fixture.frameCount; ++frame)
        ASSERT_TRUE(writeMaskFor(cacheDir.path(), frame)) << frame;

    LiveRepairSpec spec = specFor(fixture, cacheDir.path());
    const int targetFrame = qMin(5, fixture.frameCount - 1);
    const int64_t targetUs = targetUsFor(targetFrame, fixture.fps);

    LiveRepairService service;
    service.setInpainterForTest(std::make_unique<SolidInpainter>());
    QString error;
    ASSERT_TRUE(service.configure(spec, &error)) << error.toStdString();
    ASSERT_EQ(service.frameCountForTest(), fixture.frameCount);

    QSignalSpy frameReady(&service, &LiveRepairService::frameReady);
    service.setTarget(targetUs);
    QElapsedTimer clock;
    clock.start();
    service.start();
    ASSERT_TRUE(service.isRunning());
    EXPECT_TRUE(service.isConfigured());

    LiveRepairPatch patch;
    ASSERT_TRUE(waitUntil(
        [&] { return service.patchFor(spec.layerId, targetUs, &patch); },
        10000));
    const qint64 targetLatencyMs = clock.elapsed();
    EXPECT_FALSE(patch.image.isNull());
    EXPECT_TRUE(patch.crop.isValid());
    EXPECT_EQ(patch.image.size(), patch.crop.size());

    // Target-first: the first repaired frame must be the playhead frame, not
    // the start of the +-2 s window.
    ASSERT_TRUE(waitUntil([&] { return frameReady.count() > 0; }, 10000));
    EXPECT_EQ(frameReady.at(0).at(0).toInt(), targetFrame);

    // patchFor maps source instants to global frame indices and refuses
    // anything outside this layer.
    LiveRepairPatch mapped;
    EXPECT_TRUE(service.patchFor(spec.layerId, targetUs, &mapped));
    EXPECT_FALSE(
        service.patchFor(QStringLiteral("other-layer"), targetUs, &mapped));
    EXPECT_FALSE(service.patchFor(spec.layerId, -1, &mapped));
    EXPECT_FALSE(service.patchFor(spec.layerId, spec.sourceOutUs, &mapped));

    // The LRU cache is bounded: lowering the limit evicts immediately.
    service.setCacheLimitForTest(4);
    EXPECT_LE(service.cachedPatchCountForTest(), 4);

    service.stop();
    EXPECT_FALSE(service.isRunning());
    EXPECT_FALSE(service.patchFor(spec.layerId, targetUs, &mapped))
        << "a stopped service must not serve stale patches";
    std::fprintf(stderr,
                 "LIVE_REPAIR_SERVICE_METRICS targetLatencyMs=%lld "
                 "frameCount=%d\n",
                 static_cast<long long>(targetLatencyMs), fixture.frameCount);
}

TEST(LiveRepairService, MissingMaskStopsWithChineseError)
{
    Fixture fixture;
    if (!makeFixture(&fixture))
        GTEST_SKIP() << "h264.mp4 could not be opened";

    QTemporaryDir cacheDir;
    ASSERT_TRUE(cacheDir.isValid());
    // Configure() only needs one mask to accept the cache; the worker then
    // hits the missing target mask and must stop instead of showing original.
    ASSERT_TRUE(writeMaskFor(cacheDir.path(), 0));

    LiveRepairSpec spec = specFor(fixture, cacheDir.path());
    const int targetFrame = qMin(5, fixture.frameCount - 1);
    const int64_t targetUs = targetUsFor(targetFrame, fixture.fps);
    if (targetFrame == 0)
        GTEST_SKIP() << "fixture is shorter than six frames";

    LiveRepairService service;
    service.setInpainterForTest(std::make_unique<SolidInpainter>());
    QString error;
    ASSERT_TRUE(service.configure(spec, &error)) << error.toStdString();

    QSignalSpy errors(&service, &LiveRepairService::error);
    service.setTarget(targetUs);
    service.start();
    ASSERT_TRUE(waitUntil([&] { return errors.count() > 0; }, 10000));
    const QString message = errors.at(0).at(0).toString();
    EXPECT_TRUE(message.contains(QStringLiteral("跟踪遮罩")))
        << message.toStdString();
    EXPECT_TRUE(message.contains(QStringLiteral("请先运行 AI 跟踪")))
        << message.toStdString();

    LiveRepairPatch patch;
    EXPECT_FALSE(service.patchFor(spec.layerId, targetUs, &patch));
    service.stop();
}

TEST(LiveRepairService, ConfigureRequiresTrackingMasks)
{
    Fixture fixture;
    if (!makeFixture(&fixture))
        GTEST_SKIP() << "h264.mp4 could not be opened";

    QTemporaryDir cacheDir;
    ASSERT_TRUE(cacheDir.isValid());
    LiveRepairSpec spec = specFor(fixture, cacheDir.path());

    LiveRepairService service;
    service.setInpainterForTest(std::make_unique<SolidInpainter>());
    QString error;
    EXPECT_FALSE(service.configure(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("请先运行 AI 跟踪")))
        << error.toStdString();
    EXPECT_FALSE(service.isConfigured());
}
