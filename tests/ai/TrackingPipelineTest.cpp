#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_ORT) && VIDEOEDITOR_HAVE_ORT

#include <algorithm>
#include <functional>
#include <iostream>
#include <optional>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "FrameReader.h"
#include "ModelRegistry.h"
#include "OnnxExecutor.h"
#include "RemovalEvalMetrics.h"
#include "Sam2Tracker.h"
#include "TimeBase.h"
#include "TrackingPipeline.h"

using namespace VideoEditor;

namespace {

uint32_t hashPixel(int x, int y)
{
    uint32_t value = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    value = (value ^ (value >> 13)) * 1274126177u;
    return value ^ (value >> 16);
}

QRect objectRect(int shiftX, int shiftY)
{
    return QRect(30 + shiftX, 30 + shiftY, 48, 36);
}

QImage makeFrame(int width, int height, int shiftX, int shiftY)
{
    QImage image(width, height, QImage::Format_RGB32);
    const QRect object = objectRect(shiftX, shiftY);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = object.contains(x, y);
            const int sampleX = inside ? x - object.x() : x;
            const int sampleY = inside ? y - object.y() : y;
            int value = 40 + int(hashPixel(sampleX, sampleY) % 70u);
            if (inside)
                value = std::min(255, value + 130);
            image.setPixel(x, y, qRgb(value, value, value));
        }
    }
    return image;
}

BinaryMask makeMask(const QRect &rect, int width, int height)
{
    BinaryMask mask;
    mask.width = width;
    mask.height = height;
    mask.bits.assign(size_t(width) * size_t(height), 0);
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x)
            mask.set(x, y, true);
    }
    return mask;
}

const AiModelRecord *findEfficientTam(const ModelRegistry &registry)
{
    if (const AiModelRecord *record =
            registry.model(QStringLiteral("efficienttam-ti-512-fp32"))) {
        return record;
    }
    for (const AiModelRecord &record : registry.manifest().models) {
        if (record.isTracking() && record.id.contains(QStringLiteral("fp32"))
            && record.files.contains(QStringLiteral("image_encoder"))
            && record.files.contains(QStringLiteral("prompt_encoder"))
            && record.files.contains(QStringLiteral("mask_decoder"))) {
            return &record;
        }
    }
    return nullptr;
}

TrackingConfig configFor(const ModelRegistry &registry,
                         const AiModelRecord &record)
{
    TrackingConfig config;
    config.io = record.io;
    config.allowDml = false;
    config.allowCpu = true;
    for (auto it = record.files.constBegin(); it != record.files.constEnd();
         ++it) {
        if (!it.value().path.endsWith(QLatin1String(".onnx"),
                                      Qt::CaseInsensitive)) {
            continue;
        }
        config.partPaths.insert(
            it.key(), QDir(registry.rootDir()).filePath(it.value().path));
    }
    return config;
}

AiJobSpec makeSpec(int frames, int width, int height)
{
    AiJobSpec spec;
    spec.jobId = QStringLiteral("tracking-test");
    spec.clipId = QStringLiteral("clip-test");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    spec.sourceInUs = 0;
    spec.sourceOutUs = framesToMicros(frames, Rational{30, 1});
    spec.width = width;
    spec.height = height;
    spec.fps = Rational{30, 1};
    spec.trackingOnly = true;
    return spec;
}

bool fileExists(const QString &path) { return QFileInfo::exists(path); }

QString sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
            .toHex());
}

QString writeMaskSidecar(const QString &sidecarRoot, const BinaryMask &mask)
{
    const QByteArray encoded = MaskCodec::encode(mask);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
    QDir().mkpath(QDir(sidecarRoot).filePath(QStringLiteral("masks")));
    QFile file(QDir(sidecarRoot).filePath(
        QStringLiteral("masks/%1.rle").arg(hash)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(encoded);
    file.close();
    return hash;
}

} // namespace

TEST(TrackingPipeline, EfficientTamSegmentsSyntheticRectangle)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    const QString manifestPath =
        modelsDir + QStringLiteral("/models.manifest.json");
    if (modelsDir.isEmpty() || !QFileInfo::exists(manifestPath))
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set or the manifest is "
                        "missing";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = findEfficientTam(*registry);
    if (!record)
        GTEST_SKIP() << "no EfficientTAM fp32 tracking model in the manifest";

    Sam2Tracker tracker;
    ASSERT_TRUE(tracker.initialize(configFor(*registry, *record), &error))
        << error.toStdString();
    EXPECT_EQ(tracker.provider(), QStringLiteral("cpu"));

    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    const QImage image = makeFrame(kWidth, kHeight, 0, 0);
    const BinaryMask groundTruth = makeMask(objectRect(0, 0), kWidth, kHeight);
    const TrackingPrompt prompt = Sam2Tracker::promptFromMask(groundTruth);
    ASSERT_EQ(prompt.points.size(), 3);

    QElapsedTimer clock;
    clock.start();
    const auto result = tracker.segment(image, prompt, &error);
    std::cout << "EfficientTAM keyframe segmentation: " << clock.elapsed()
              << " ms" << std::endl;
    ASSERT_TRUE(result.has_value()) << error.toStdString();
    EXPECT_GT(result->areaPx, 0);
    EXPECT_GE(maskIou(result->mask, groundTruth), 0.5);
    EXPECT_GE(result->predictedIou, 0.0f);
    EXPECT_LE(result->predictedIou, 1.0f);

    QString propagateError;
    EXPECT_FALSE(tracker.propagate(image, &propagateError));
    EXPECT_EQ(propagateError, QStringLiteral("逐帧记忆传播未启用。"));
}

TEST(TrackingPipeline, RunsWithKeyframesAndWritesArtifacts)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    const QString manifestPath =
        modelsDir + QStringLiteral("/models.manifest.json");
    if (modelsDir.isEmpty() || !QFileInfo::exists(manifestPath))
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set or the manifest is "
                        "missing";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = findEfficientTam(*registry);
    if (!record)
        GTEST_SKIP() << "no EfficientTAM fp32 tracking model in the manifest";

    Sam2Tracker tracker;
    ASSERT_TRUE(tracker.initialize(configFor(*registry, *record), &error))
        << error.toStdString();

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    constexpr int kFrames = 12;
    constexpr int kSize = 128;
    QList<QImage> frames;
    for (int i = 0; i < kFrames; ++i)
        frames.append(makeFrame(kSize, kSize, i * 2, i));

    AiJobSpec spec = makeSpec(kFrames, kSize, kSize);
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));

    TrackingPipeline pipeline;
    pipeline.setTracker(&tracker);
    QElapsedTimer clock;
    clock.start();
    const TrackingRunResult result = pipeline.runFrames(
        spec, frames, {0}, {makeMask(objectRect(0, 0), kSize, kSize)});
    std::cout << "TrackingPipeline 12-frame model run: " << clock.elapsed()
              << " ms" << std::endl;

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.completedFrames, kFrames);
    EXPECT_NE(result.state, RemovalTrackingState::Failed);
    ASSERT_EQ(result.confidenceHash.size(), 64);

    for (int i = 0; i < kFrames; ++i) {
        const QString maskPath = QDir(spec.cacheDir)
                                     .filePath(QStringLiteral(
                                         "tracking/masks/frame_%1.rle")
                                                   .arg(i, 6, 10,
                                                        QLatin1Char('0')));
        EXPECT_TRUE(fileExists(maskPath)) << maskPath.toStdString();
    }
    const QString confidencePath =
        QDir(spec.sidecarRoot)
            .filePath(QStringLiteral("tracking/%1.json")
                          .arg(result.confidenceHash));
    ASSERT_TRUE(fileExists(confidencePath));
    QFile confidenceFile(confidencePath);
    ASSERT_TRUE(confidenceFile.open(QIODevice::ReadOnly));
    const QJsonObject summary =
        QJsonDocument::fromJson(confidenceFile.readAll()).object();
    EXPECT_EQ(summary.value(QStringLiteral("frames")).toArray().size(),
              kFrames);
}

TEST(TrackingPipeline, RunsOnFixtureVideoWithModel)
{
    const QString fixturePath = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)
        + QStringLiteral("/h264.mp4");
    if (!QFileInfo::exists(fixturePath))
        GTEST_SKIP() << "tests/fixtures/h264.mp4 is missing";

    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    const QString manifestPath =
        modelsDir + QStringLiteral("/models.manifest.json");
    if (modelsDir.isEmpty() || !QFileInfo::exists(manifestPath))
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set or the manifest is "
                        "missing";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = findEfficientTam(*registry);
    if (!record)
        GTEST_SKIP() << "no EfficientTAM fp32 tracking model in the manifest";

    Sam2Tracker tracker;
    ASSERT_TRUE(tracker.initialize(configFor(*registry, *record), &error))
        << error.toStdString();

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    constexpr int kFrames = 30;
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    AiJobSpec spec = makeSpec(kFrames, kWidth, kHeight);
    spec.assetPath = fixturePath;
    spec.sourceOutUs = framesToMicros(kFrames, spec.fps);
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));

    const QString maskHash = writeMaskSidecar(
        spec.sidecarRoot, makeMask(QRect(200, 120, 120, 90), kWidth, kHeight));
    ASSERT_EQ(maskHash.size(), 64);
    spec.maskKeyframes.append(
        MaskKeyframe{QStringLiteral("mk-1"), 0, maskHash, 4, 1});
    // A second keyframe at frame 15 exercises the backward DIS pass from the
    // right anchor back to the midpoint.
    const QString secondHash = writeMaskSidecar(
        spec.sidecarRoot, makeMask(QRect(220, 130, 120, 90), kWidth, kHeight));
    ASSERT_EQ(secondHash.size(), 64);
    spec.maskKeyframes.append(MaskKeyframe{QStringLiteral("mk-2"),
                                           framesToMicros(15, spec.fps),
                                           secondHash, 4, 1});

    TrackingPipeline pipeline;
    pipeline.setTracker(&tracker);
    QElapsedTimer clock;
    clock.start();
    const TrackingRunResult result = pipeline.run(spec);
    std::cout << "Fixture 30-frame model pipeline: " << clock.elapsed()
              << " ms" << std::endl;

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.completedFrames, kFrames);
    ASSERT_EQ(result.confidenceHash.size(), 64);
    EXPECT_TRUE(fileExists(QDir(spec.sidecarRoot)
                               .filePath(QStringLiteral("tracking/%1.json")
                                             .arg(result.confidenceHash))));
}

TEST(TrackingPipeline, CancelPreservesCompletedMasks)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    constexpr int kFrames = 20;
    constexpr int kSize = 128;
    QList<QImage> frames;
    for (int i = 0; i < kFrames; ++i)
        frames.append(makeFrame(kSize, kSize, i * 2, i));

    AiJobSpec spec = makeSpec(kFrames, kSize, kSize);
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));

    int polls = 0;
    TrackingRunCallbacks callbacks;
    callbacks.cancel = [&polls]() {
        ++polls;
        return polls > 5;
    };

    TrackingPipeline pipeline; // No model: DIS-only.
    const TrackingRunResult result = pipeline.runFrames(
        spec, frames, {0}, {makeMask(objectRect(0, 0), kSize, kSize)},
        callbacks);

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.state, RemovalTrackingState::Partial);
    EXPECT_EQ(result.completedFrames, 5);
    ASSERT_EQ(result.confidenceHash.size(), 64);
    for (int i = 0; i < 5; ++i) {
        const QString maskPath = QDir(spec.cacheDir)
                                     .filePath(QStringLiteral(
                                         "tracking/masks/frame_%1.rle")
                                                   .arg(i, 6, 10,
                                                        QLatin1Char('0')));
        EXPECT_TRUE(fileExists(maskPath)) << maskPath.toStdString();
    }
    const QString notWritten = QDir(spec.cacheDir)
                                   .filePath(QStringLiteral(
                                       "tracking/masks/frame_000005.rle"));
    EXPECT_FALSE(fileExists(notWritten));
}

TEST(TrackingPipeline, ClampsOvershootingSourceOutToStream)
{
    const QString fixture = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)
        + QStringLiteral("/h264.mp4");
    if (!QFileInfo::exists(fixture))
        GTEST_SKIP() << "tests/fixtures/h264.mp4 is missing";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    AiJobSpec spec = makeSpec(1, 640, 360);
    spec.assetPath = fixture;
    spec.sourceInUs = 0;
    // h264.mp4 is 2 s / 60 frames; the job asks for 3 s.
    spec.sourceOutUs = 3'000'000;
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString maskHash = writeMaskSidecar(
        spec.sidecarRoot, makeMask(QRect(200, 120, 120, 90), 640, 360));
    ASSERT_EQ(maskHash.size(), 64);
    spec.maskKeyframes.append(
        MaskKeyframe{QStringLiteral("mk-1"), 0, maskHash, 4, 1});

    TrackingPipeline pipeline;
    const TrackingRunResult result = pipeline.run(spec);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.completedFrames, 60);
    ASSERT_EQ(result.confidenceHash.size(), 64);

    // A keyframe past the clamped frame count keeps the clear range error.
    AiJobSpec late = spec;
    late.maskKeyframes.clear();
    const QString lateHash = writeMaskSidecar(
        late.sidecarRoot, makeMask(QRect(200, 120, 120, 90), 640, 360));
    late.maskKeyframes.append(MaskKeyframe{QStringLiteral("mk-late"), 2'500'000,
                                           lateHash, 4, 1});
    const TrackingRunResult lateResult = pipeline.run(late);
    EXPECT_FALSE(lateResult.ok);
    EXPECT_NE(lateResult.error.indexOf(QStringLiteral("超出跟踪帧范围")), -1)
        << lateResult.error.toStdString();
}

TEST(TrackingPipeline, CancelDuringBackwardPreservesSamples)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    constexpr int kFrames = 20;
    constexpr int kSize = 128;
    QList<QImage> frames;
    for (int i = 0; i < kFrames; ++i)
        frames.append(makeFrame(kSize, kSize, i * 2, i));

    AiJobSpec spec = makeSpec(kFrames, kSize, kSize);
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));

    int polls = 0;
    TrackingRunCallbacks callbacks;
    callbacks.cancel = [&polls]() {
        ++polls;
        return polls > 8;
    };

    TrackingPipeline pipeline;
    const TrackingRunResult result =
        pipeline.runFrames(spec, frames, {0, 10},
                           {makeMask(objectRect(0, 0), kSize, kSize),
                            makeMask(objectRect(20, 10), kSize, kSize)},
                           callbacks);

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.state, RemovalTrackingState::Partial);
    // Masks written before the cancel must match the confidence samples.
    int maskFiles = 0;
    for (int i = 0; i < kFrames; ++i) {
        const QString path = QDir(spec.cacheDir)
                                 .filePath(QStringLiteral(
                                     "tracking/masks/frame_%1.rle")
                                               .arg(i, 6, 10, QLatin1Char('0')));
        if (fileExists(path))
            ++maskFiles;
    }
    EXPECT_EQ(maskFiles, result.completedFrames);
    ASSERT_EQ(result.confidenceHash.size(), 64);
    QFile confidenceFile(
        QDir(spec.sidecarRoot)
            .filePath(QStringLiteral("tracking/%1.json")
                          .arg(result.confidenceHash)));
    ASSERT_TRUE(confidenceFile.open(QIODevice::ReadOnly));
    const QJsonArray framesJson =
        QJsonDocument::fromJson(confidenceFile.readAll())
            .object()
            .value(QStringLiteral("frames"))
            .toArray();
    EXPECT_EQ(framesJson.size(), result.completedFrames);
}

TEST(TrackingPipeline, RunsWithoutModelBetweenKeyframes)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    constexpr int kFrames = 12;
    constexpr int kSize = 128;
    QList<QImage> frames;
    for (int i = 0; i < kFrames; ++i)
        frames.append(makeFrame(kSize, kSize, i * 2, i));

    AiJobSpec spec = makeSpec(kFrames, kSize, kSize);
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));

    QList<QJsonObject> events;
    TrackingRunCallbacks callbacks;
    callbacks.event = [&events](const QJsonObject &event) {
        events.append(event);
    };

    TrackingPipeline pipeline;
    QElapsedTimer clock;
    clock.start();
    const TrackingRunResult result =
        pipeline.runFrames(spec, frames, {0, 6},
                           {makeMask(objectRect(0, 0), kSize, kSize),
                            makeMask(objectRect(12, 6), kSize, kSize)},
                           callbacks);
    std::cout << "DIS-only 12-frame pipeline: " << clock.elapsed() << " ms"
              << std::endl;

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.completedFrames, kFrames);
    EXPECT_EQ(result.state, RemovalTrackingState::Complete);

    int progressCount = 0;
    int chunkDoneCount = 0;
    for (const QJsonObject &event : events) {
        const QString name = event.value(QStringLiteral("event")).toString();
        if (name == QLatin1String("progress"))
            ++progressCount;
        if (name == QLatin1String("chunkDone")) {
            ++chunkDoneCount;
            EXPECT_EQ(event.value(QStringLiteral("completedFrames")).toInt(),
                      kFrames);
        }
    }
    EXPECT_EQ(progressCount, kFrames);
    EXPECT_EQ(chunkDoneCount, 1);

    ASSERT_EQ(result.confidenceHash.size(), 64);
    const QString confidencePath =
        QDir(spec.sidecarRoot)
            .filePath(QStringLiteral("tracking/%1.json")
                          .arg(result.confidenceHash));
    ASSERT_TRUE(fileExists(confidencePath));
    // The hash must be over the exact file bytes.
    EXPECT_EQ(sha256File(confidencePath), result.confidenceHash);

    QFile confidenceFile(confidencePath);
    ASSERT_TRUE(confidenceFile.open(QIODevice::ReadOnly));
    const QJsonArray framesJson =
        QJsonDocument::fromJson(confidenceFile.readAll())
            .object()
            .value(QStringLiteral("frames"))
            .toArray();
    ASSERT_EQ(framesJson.size(), kFrames);
    for (int i = 0; i < framesJson.size(); ++i) {
        EXPECT_EQ(framesJson.at(i)
                      .toObject()
                      .value(QStringLiteral("timeUs"))
                      .toInt(),
                  spec.sourceInUs + framesToMicros(i, spec.fps));
    }
}

TEST(TrackingPipeline, EmptyKeyframeYieldsLowConfidence)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    constexpr int kFrames = 8;
    constexpr int kSize = 128;
    QList<QImage> frames;
    for (int i = 0; i < kFrames; ++i)
        frames.append(makeFrame(kSize, kSize, 0, 0));

    AiJobSpec spec = makeSpec(kFrames, kSize, kSize);
    spec.cacheDir = temp.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = temp.filePath(QStringLiteral("sidecar"));

    BinaryMask empty;
    empty.width = kSize;
    empty.height = kSize;
    empty.bits.assign(size_t(kSize) * kSize, 0);

    TrackingPipeline pipeline;
    const TrackingRunResult result =
        pipeline.runFrames(spec, frames, {0}, {empty});
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.completedFrames, kFrames);
    EXPECT_EQ(result.state, RemovalTrackingState::LowConfidence);
}

TEST(TrackingPipeline, RejectsKeyframeOutsideRange)
{
    AiJobSpec spec = makeSpec(30, 128, 128);
    spec.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), spec.sourceOutUs, QString(64, QLatin1Char('b')),
        4, 1});
    spec.trackingOnly = true;

    TrackingPipeline pipeline;
    const TrackingRunResult result = pipeline.run(spec);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.indexOf(QStringLiteral("跟踪区间")), -1)
        << result.error.toStdString();
}

// Acceptance gate: EfficientTAM keyframe IoU >= 0.85 against the authored mask
// on the committed static-logo fixture (not the synthetic rectangle).
TEST(TrackingPipeline, EfficientTamMatchesAuthoredMaskOnFixture)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    const QString manifestPath =
        modelsDir + QStringLiteral("/models.manifest.json");
    if (modelsDir.isEmpty() || !QFileInfo::exists(manifestPath))
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set or the manifest is "
                        "missing";
    const QString fixtures =
        QStringLiteral(VIDEOEDITOR_FIXTURE_DIR) + QStringLiteral("/removal");
    const QString video = fixtures + QStringLiteral("/static_logo.mp4");
    const QString maskVideo =
        fixtures + QStringLiteral("/static_logo_mask.mp4");
    if (!QFileInfo::exists(video) || !QFileInfo::exists(maskVideo))
        GTEST_SKIP() << "removal fixtures missing; run generate_fixtures.ps1";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = findEfficientTam(*registry);
    if (!record)
        GTEST_SKIP() << "no EfficientTAM fp32 tracking model in the manifest";

    const QSize display(640, 360);
    FrameReader reader;
    ASSERT_TRUE(reader.open(video, Rational{30, 1}, 0, 5'000'000, display,
                            &error))
        << error.toStdString();
    const QImage frame = reader.readFrame(0, &error);
    ASSERT_FALSE(frame.isNull()) << error.toStdString();

    FrameReader maskReader;
    ASSERT_TRUE(maskReader.open(maskVideo, Rational{30, 1}, 0, 5'000'000,
                                display, &error))
        << error.toStdString();
    const QImage maskImage = maskReader.readFrame(0, &error);
    ASSERT_FALSE(maskImage.isNull()) << error.toStdString();
    BinaryMask groundTruth;
    groundTruth.width = display.width();
    groundTruth.height = display.height();
    groundTruth.bits.assign(size_t(display.width()) * size_t(display.height()), 0);
    for (int y = 0; y < display.height(); ++y)
        for (int x = 0; x < display.width(); ++x)
            if (qGray(maskImage.pixel(x, y)) > 127)
                groundTruth.set(x, y, true);
    ASSERT_GT(groundTruth.bits.size(), size_t(0));

    Sam2Tracker tracker;
    ASSERT_TRUE(tracker.initialize(configFor(*registry, *record), &error))
        << error.toStdString();
    const TrackingPrompt prompt = Sam2Tracker::promptFromMask(groundTruth);
    QElapsedTimer clock;
    clock.start();
    const auto segmented = tracker.segment(frame, prompt, &error);
    ASSERT_TRUE(segmented.has_value()) << error.toStdString();
    const double iou = maskIou(segmented->mask, groundTruth);
    std::cout << "EfficientTAM fixture keyframe IoU=" << iou
              << " segmentation=" << clock.elapsed() << " ms" << std::endl;
    EXPECT_GE(iou, 0.85);
}

// Acceptance gate: DIS propagation with K=5 authored resets. Uses the
// slow-target corpus (not the static logo) and the ground-truth mask video.
TEST(TrackingPipeline, DisResetsAtK5KeyframesMaintainMeanIou)
{
    const QString fixtures =
        QStringLiteral(VIDEOEDITOR_FIXTURE_DIR) + QStringLiteral("/removal");
    const QString video = fixtures + QStringLiteral("/slow_target.mp4");
    const QString maskVideo =
        fixtures + QStringLiteral("/slow_target_mask.mp4");
    if (!QFileInfo::exists(video) || !QFileInfo::exists(maskVideo))
        GTEST_SKIP() << "removal fixtures missing; run generate_fixtures.ps1";

    QString error;
    const QSize display(640, 360);
    const Rational fps{30, 1};
    FrameReader reader;
    ASSERT_TRUE(reader.open(video, fps, 0, 5'000'000, display, &error))
        << error.toStdString();
    const QList<QImage> frames = reader.readAll(&error);
    ASSERT_FALSE(frames.isEmpty()) << error.toStdString();
    FrameReader maskReader;
    ASSERT_TRUE(maskReader.open(maskVideo, fps, 0, 5'000'000, display, &error))
        << error.toStdString();
    const QList<QImage> maskFrames = maskReader.readAll(&error);
    ASSERT_FALSE(maskFrames.isEmpty()) << error.toStdString();

    const int total = std::min(frames.size(), maskFrames.size());
    QList<BinaryMask> groundTruth;
    groundTruth.reserve(total);
    for (int i = 0; i < total; ++i) {
        BinaryMask mask;
        mask.width = display.width();
        mask.height = display.height();
        mask.bits.assign(size_t(display.width()) * size_t(display.height()), 0);
        for (int y = 0; y < display.height(); ++y)
            for (int x = 0; x < display.width(); ++x)
                if (qGray(maskFrames.at(i).pixel(x, y)) > 127)
                    mask.set(x, y, true);
        groundTruth.append(mask);
    }

    QList<int> keyframeIndices;
    QList<BinaryMask> keyframeMasks;
    for (int i = 0; i < total; i += 5) {
        keyframeIndices.append(i);
        keyframeMasks.append(groundTruth.at(i));
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    AiJobSpec spec = makeSpec(total, display.width(), display.height());
    spec.cacheDir = dir.filePath(QStringLiteral("cache"));
    spec.sidecarRoot = dir.filePath(QStringLiteral("sidecar"));
    TrackingPipeline pipeline; // no tracker: DIS-only between keyframes
    const TrackingRunResult result =
        pipeline.runFrames(spec, frames, keyframeIndices, keyframeMasks);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.completedFrames, total);

    const auto readWrittenMask = [&spec](int index,
                                         QString *readError)
        -> std::optional<BinaryMask> {
        const QString path = QDir(spec.cacheDir)
                                 .filePath(QStringLiteral("tracking/masks/"
                                                          "frame_%1.rle")
                                               .arg(index, 6, 10,
                                                    QLatin1Char('0')));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (readError)
                *readError = QStringLiteral("missing %1").arg(path);
            return std::nullopt;
        }
        return MaskCodec::decode(file.readAll(), readError);
    };

    double sum = 0.0;
    double minimum = 1.0;
    for (int i = 0; i < total; ++i) {
        QString readError;
        const auto mask = readWrittenMask(i, &readError);
        ASSERT_TRUE(mask.has_value()) << readError.toStdString();
        const double iou = maskIou(*mask, groundTruth.at(i));
        sum += iou;
        minimum = std::min(minimum, iou);
    }
    const double mean = sum / double(total);
    std::cout << "DIS K=5 propagation: mean IoU=" << mean
              << " min=" << minimum << " over " << total << " frames"
              << std::endl;
    EXPECT_GE(mean, 0.90);
}

TEST(OnnxExecutor, RejectsWhenNoExecutionProviderAllowed)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    const QString modelPath =
        modelsDir + QStringLiteral(
                        "/efficienttam_ti_512_fp32/prompt_encoder.onnx");
    if (modelsDir.isEmpty() || !QFileInfo::exists(modelPath))
        GTEST_SKIP() << "EfficientTAM prompt_encoder.onnx is unavailable";

    OnnxExecutor executor;
    EpConfig config;
    config.allowDml = false;
    config.allowCpu = false;
    QString error;
    EXPECT_FALSE(executor.initialize(
        {QStringLiteral("prompt_encoder")},
        {{QStringLiteral("prompt_encoder"), modelPath}}, config, &error));
    EXPECT_NE(error.indexOf(QStringLiteral("执行提供程序")), -1)
        << error.toStdString();
}

#endif // VIDEOEDITOR_HAVE_ORT
