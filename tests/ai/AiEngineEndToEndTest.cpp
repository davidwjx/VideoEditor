#include <gtest/gtest.h>

#if defined(VIDEOEDITOR_HAVE_ORT) && VIDEOEDITOR_HAVE_ORT

#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QRect>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <iostream>

#include "AiProtocol.h"
#include "LocalAiTransport.h"
#include "MaskCodec.h"
#include "RemovalCache.h"

using namespace VideoEditor;

namespace {

QString enginePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("VisionAiEngine.exe"));
}

QString fixturePath()
{
    return QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)
        + QStringLiteral("/h264.mp4");
}

QString modelsManifest(const QString &modelsDir)
{
    return modelsDir + QStringLiteral("/models.manifest.json");
}

// Sets the technical override and engine path for the child process for the
// duration of the test.
class EngineEnvironmentGuard {
public:
    EngineEnvironmentGuard()
        : m_oldTechnical(qgetenv("VIDEOEDITOR_M4_TECHNICAL"))
        , m_oldEngine(qgetenv("VIDEOEDITOR_AI_ENGINE"))
    {
        qputenv("VIDEOEDITOR_M4_TECHNICAL", "1");
        qputenv("VIDEOEDITOR_AI_ENGINE", enginePath().toUtf8());
    }

    ~EngineEnvironmentGuard()
    {
        restore("VIDEOEDITOR_M4_TECHNICAL", m_oldTechnical);
        restore("VIDEOEDITOR_AI_ENGINE", m_oldEngine);
    }

private:
    static void restore(const char *name, const QByteArray &value)
    {
        if (value.isNull())
            qunsetenv(name);
        else
            qputenv(name, value);
    }

    QByteArray m_oldTechnical;
    QByteArray m_oldEngine;
};

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

bool writeTrackingMask(const QString &cacheDir, int frame,
                       const BinaryMask &mask)
{
    const QString directory =
        QDir(cacheDir).filePath(QStringLiteral("tracking/masks"));
    if (!QDir().mkpath(directory))
        return false;
    const QByteArray encoded = MaskCodec::encode(mask);
    if (encoded.isEmpty())
        return false;
    QFile file(QDir(directory).filePath(
        QStringLiteral("frame_%1.rle").arg(frame, 6, 10, QLatin1Char('0'))));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(encoded) == encoded.size();
}

BinaryMask centeredMask(int width, int height)
{
    BinaryMask mask;
    mask.width = width;
    mask.height = height;
    mask.bits.assign(size_t(width) * size_t(height), 0);
    const QRect rect(width / 3, height / 3, width / 4, height / 4);
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x)
            mask.set(x, y, true);
    }
    return mask;
}

AiJobSpec makeEngineSpec(const QString &fixture, const QString &modelsDir,
                         const QString &sidecarRoot, const QString &cacheDir,
                         int frames, const QString &maskHash)
{
    AiJobSpec spec;
    spec.jobId = QStringLiteral("e2e-job");
    spec.clipId = QStringLiteral("clip-e2e");
    spec.assetPath = fixture;
    spec.sidecarRoot = sidecarRoot;
    spec.cacheDir = cacheDir;
    spec.cacheKey = QString(64, QLatin1Char('a'));
    spec.modelsManifestPath = modelsManifest(modelsDir);
    spec.trackingModelId = QStringLiteral("efficienttam-ti-512-fp32");
    spec.sourceInUs = 0;
    spec.sourceOutUs = framesToMicros(frames, Rational{30, 1});
    spec.width = 640;
    spec.height = 360;
    spec.fps = Rational{30, 1};
    spec.trackingOnly = true;
    spec.maskKeyframes.append(
        MaskKeyframe{QStringLiteral("mk-1"), 0, maskHash, 4, 1});
    return spec;
}

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
            .toHex());
}

bool sawEvent(const QSignalSpy &spy, const QString &event,
              QJsonObject *out = nullptr)
{
    for (const QList<QVariant> &arguments : spy) {
        const QJsonObject message = arguments.at(0).toJsonObject();
        if (message.value(QStringLiteral("event")).toString() == event) {
            if (out)
                *out = message;
            return true;
        }
    }
    return false;
}

int progressCount(const QSignalSpy &spy)
{
    int count = 0;
    for (const QList<QVariant> &arguments : spy) {
        if (arguments.at(0)
                .toJsonObject()
                .value(QStringLiteral("event"))
                .toString()
            == QLatin1String("progress")) {
            ++count;
        }
    }
    return count;
}

int lastProgressFrame(const QSignalSpy &spy)
{
    int frame = -1;
    for (const QList<QVariant> &arguments : spy) {
        const QJsonObject message = arguments.at(0).toJsonObject();
        if (message.value(QStringLiteral("event")).toString()
            == QLatin1String("progress")) {
            frame = message.value(QStringLiteral("frame")).toInt();
        }
    }
    return frame;
}

bool skipUnlessEngineAvailable()
{
    if (!QFileInfo::exists(enginePath()))
        return false;
    if (!QFileInfo::exists(fixturePath()))
        return false;
    const QString models = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    return !models.isEmpty() && QFileInfo::exists(modelsManifest(models));
}

} // namespace

TEST(AiEngineEndToEnd, TrackingOnlyJobWritesConfidenceAndCompletes)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheDir = temp.filePath(QStringLiteral("cache"));
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    const AiJobSpec spec = makeEngineSpec(
        fixturePath(), modelsDir, sidecarRoot, cacheDir, 30, maskHash);
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 60000);
    QJsonObject done;
    ASSERT_TRUE(sawEvent(messages, QStringLiteral("done"), &done));
    EXPECT_EQ(done.value(QStringLiteral("cacheKey")).toString(),
              spec.cacheKey);
    EXPECT_EQ(done.value(QStringLiteral("completedFrames")).toInt(), 30);
    const QString trackingState =
        done.value(QStringLiteral("trackingState")).toString();
    EXPECT_TRUE(trackingState == QLatin1String("complete")
                || trackingState == QLatin1String("lowConfidence"))
        << trackingState.toStdString();
    const QString confidenceHash =
        done.value(QStringLiteral("confidenceHash")).toString();
    EXPECT_EQ(confidenceHash.size(), 64);
    EXPECT_TRUE(QFileInfo::exists(
        QDir(sidecarRoot)
            .filePath(QStringLiteral("tracking/%1.json").arg(confidenceHash))));

    const QList<QVariant> finishedArgs = finished.at(0);
    EXPECT_EQ(finishedArgs.at(0).toInt(), 0);
    EXPECT_EQ(finishedArgs.at(1).value<QProcess::ExitStatus>(),
              QProcess::NormalExit);
}

TEST(AiEngineEndToEnd, FallsBackFromDefaultSam2ModelWithWarning)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheDir = temp.filePath(QStringLiteral("cache"));
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    AiJobSpec spec = makeEngineSpec(fixturePath(), modelsDir, sidecarRoot,
                                    cacheDir, 10, maskHash);
    // The manifest's first/default tracker is the SAM 2 bundle, which the
    // offline engine cannot use; it must fall back to EfficientTAM with a
    // warning instead of failing.
    spec.trackingModelId = QStringLiteral("sam2.1-hiera-tiny");
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 60000);
    EXPECT_TRUE(sawEvent(messages, QStringLiteral("warning")));
    QJsonObject done;
    ASSERT_TRUE(sawEvent(messages, QStringLiteral("done"), &done));
    EXPECT_EQ(done.value(QStringLiteral("completedFrames")).toInt(), 10);
}

TEST(AiEngineEndToEnd, PauseResumeThenCancelYieldsPartial)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheDir = temp.filePath(QStringLiteral("cache"));
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    const AiJobSpec spec = makeEngineSpec(
        fixturePath(), modelsDir, sidecarRoot, cacheDir, 60, maskHash);
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    // Wait until the engine has actually emitted progress, then pause it.
    QTRY_VERIFY_WITH_TIMEOUT(progressCount(messages) >= 1, 60000);
    transport.sendCommand(
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("pause")}});
    QTest::qWait(50);
    const int pausedProgress = progressCount(messages);
    QTest::qWait(300);
    // A paused engine must stop producing per-frame progress.
    EXPECT_LE(progressCount(messages), pausedProgress + 1);

    // Resume and confirm progress restarts before cancelling.
    transport.sendCommand(
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("resume")}});
    QTRY_VERIFY_WITH_TIMEOUT(progressCount(messages) > pausedProgress, 30000);
    transport.cancel();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);
    QJsonObject cancelled;
    ASSERT_TRUE(
        sawEvent(messages, QStringLiteral("cancelled"), &cancelled))
        << "engine did not emit cancelled (was it killed after 5 s?)";
    EXPECT_EQ(cancelled.value(QStringLiteral("trackingState")).toString(),
              QStringLiteral("partial"));
    const int completed =
        cancelled.value(QStringLiteral("completedFrames")).toInt();
    EXPECT_GT(completed, 0);
    EXPECT_LT(completed, 60);
    const QString confidenceHash =
        cancelled.value(QStringLiteral("confidenceHash")).toString();
    EXPECT_EQ(confidenceHash.size(), 64);

    const QList<QVariant> finishedArgs = finished.at(0);
    EXPECT_EQ(finishedArgs.at(0).toInt(), 0);
    EXPECT_EQ(finishedArgs.at(1).value<QProcess::ExitStatus>(),
              QProcess::NormalExit);
}

TEST(AiEngineEndToEnd, RepairJobWritesChunkAndManifest)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheRoot = temp.filePath(QStringLiteral("cache"));
    const QString cacheKey = QString(64, QLatin1Char('a'));
    const QString cacheDir = QDir(cacheRoot).filePath(cacheKey);
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    // Four frames keeps the MI-GAN run short; the full suite must not depend
    // on long inference.
    AiJobSpec spec = makeEngineSpec(fixturePath(), modelsDir, sidecarRoot,
                                    cacheDir, 4, maskHash);
    spec.trackingOnly = false;
    spec.inpaintModelId = QStringLiteral("migan-512");
    spec.parameters.mode = RepairMode::Fast;
    spec.parameters.temporalStabilization = true;
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    QElapsedTimer clock;
    clock.start();
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 90000);
    std::cout << "Engine repair E2E (4 frames, MI-GAN): " << clock.elapsed()
              << " ms" << std::endl;
    QJsonObject done;
    ASSERT_TRUE(sawEvent(messages, QStringLiteral("done"), &done));
    EXPECT_EQ(done.value(QStringLiteral("cacheKey")).toString(),
              spec.cacheKey);
    EXPECT_EQ(done.value(QStringLiteral("completedFrames")).toInt(), 4);

    const QList<QVariant> finishedArgs = finished.at(0);
    EXPECT_EQ(finishedArgs.at(0).toInt(), 0);
    EXPECT_EQ(finishedArgs.at(1).value<QProcess::ExitStatus>(),
              QProcess::NormalExit);

    RemovalCache cache(cacheRoot);
    QString cacheError;
    const auto manifest = cache.readManifest(cacheKey, &cacheError);
    ASSERT_TRUE(manifest.has_value()) << cacheError.toStdString();
    EXPECT_EQ(manifest->status, QStringLiteral("complete"));
    EXPECT_EQ(manifest->totalFrames, 4);
    EXPECT_EQ(manifest->inpaintModelId, QStringLiteral("migan-512"));
    ASSERT_FALSE(manifest->chunks.isEmpty());
    EXPECT_EQ(manifest->chunks.first().index, 0);
    EXPECT_EQ(manifest->chunks.first().frameCount, 4);
    EXPECT_TRUE(cache.verify(cacheKey, &cacheError))
        << cacheError.toStdString();

    const auto index =
        cache.readChunkIndex(cacheKey, manifest->chunks.first().index,
                             &cacheError);
    ASSERT_TRUE(index.has_value()) << cacheError.toStdString();
    ASSERT_FALSE(index->frames.isEmpty());
    const QImage patch = cache.readFrameImage(cacheKey, index->frames.first(),
                                              &cacheError);
    ASSERT_FALSE(patch.isNull()) << cacheError.toStdString();
    bool alphaPresent = false;
    for (int y = 0; y < patch.height() && !alphaPresent; ++y)
        for (int x = 0; x < patch.width() && !alphaPresent; ++x)
            alphaPresent = qAlpha(patch.pixel(x, y)) > 0;
    EXPECT_TRUE(alphaPresent);
}

TEST(AiEngineEndToEnd, ResumeSkipsChunksAlreadyInManifest)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheRoot = temp.filePath(QStringLiteral("cache"));
    const QString cacheKey = QString(64, QLatin1Char('a'));
    const QString cacheDir = QDir(cacheRoot).filePath(cacheKey);
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    AiJobSpec spec = makeEngineSpec(fixturePath(), modelsDir, sidecarRoot,
                                    cacheDir, 2, maskHash);
    spec.trackingOnly = false;
    spec.inpaintModelId = QStringLiteral("migan-512");
    EngineEnvironmentGuard guard;

    QString chunkFile;
    QDateTime chunkModified;
    QString chunkSha;
    QString error;
    {
        LocalAiTransport transport;
        QSignalSpy messages(&transport, &AiTransport::message);
        QSignalSpy finished(&transport, &AiTransport::finished);
        ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 90000);
        EXPECT_TRUE(sawEvent(messages, QStringLiteral("done")));
    }
    {
        RemovalCache cache(cacheRoot);
        const auto manifest = cache.readManifest(cacheKey, &error);
        ASSERT_TRUE(manifest.has_value()) << error.toStdString();
        ASSERT_FALSE(manifest->chunks.isEmpty());
        const auto index =
            cache.readChunkIndex(cacheKey, manifest->chunks.first().index,
                                 &error);
        ASSERT_TRUE(index.has_value()) << error.toStdString();
        ASSERT_FALSE(index->frames.isEmpty());
        chunkFile = QDir(cache.cacheDir(cacheKey))
                        .filePath(QStringLiteral("chunks/%1/%2")
                                      .arg(index->index)
                                      .arg(index->frames.first().file));
        ASSERT_TRUE(QFileInfo::exists(chunkFile));
        chunkModified = QFileInfo(chunkFile).lastModified();
        chunkSha = fileSha256(chunkFile);
        ASSERT_EQ(chunkSha.size(), 64);
    }

    // Resume must skip the completed chunk without rewriting it. The tracking
    // masks stay in place, so this proves the chunk itself was not re-run.
    AiJobSpec resumed = spec;
    resumed.resume = true;
    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    ASSERT_TRUE(transport.start(resumed, &error)) << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);
    QJsonObject done;
    ASSERT_TRUE(sawEvent(messages, QStringLiteral("done"), &done));
    EXPECT_EQ(done.value(QStringLiteral("completedFrames")).toInt(), 2);

    int repairChunkDone = 0;
    for (const QList<QVariant> &arguments : messages) {
        const QJsonObject message = arguments.at(0).toJsonObject();
        if (message.value(QStringLiteral("event")).toString()
                == QLatin1String("chunkDone")
            && message.value(QStringLiteral("stage")).toString()
                == QLatin1String("repair")) {
            ++repairChunkDone;
        }
    }
    EXPECT_EQ(repairChunkDone, 0);
    EXPECT_EQ(fileSha256(chunkFile), chunkSha);
    EXPECT_EQ(QFileInfo(chunkFile).lastModified(), chunkModified);

    RemovalCache cache(cacheRoot);
    const auto manifest = cache.readManifest(cacheKey, &error);
    ASSERT_TRUE(manifest.has_value()) << error.toStdString();
    EXPECT_EQ(manifest->status, QStringLiteral("complete"));
    EXPECT_FALSE(manifest->chunks.isEmpty());
    EXPECT_TRUE(cache.verify(cacheKey, &error)) << error.toStdString();
}

TEST(AiEngineEndToEnd, CancelDuringRepairWritesPartialManifest)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheRoot = temp.filePath(QStringLiteral("cache"));
    const QString cacheKey = QString(64, QLatin1Char('b'));
    const QString cacheDir = QDir(cacheRoot).filePath(cacheKey);
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    AiJobSpec spec = makeEngineSpec(fixturePath(), modelsDir, sidecarRoot,
                                    cacheDir, 2, maskHash);
    spec.trackingOnly = false;
    spec.inpaintModelId = QStringLiteral("migan-512");
    spec.cacheKey = cacheKey;
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    // Wait until the repair stage starts (tracking is done), then cancel.
    QTRY_VERIFY_WITH_TIMEOUT(
        [&messages]() {
            for (const QList<QVariant> &arguments : messages) {
                const QJsonObject message = arguments.at(0).toJsonObject();
                if (message.value(QStringLiteral("event")).toString()
                        == QLatin1String("progress")
                    && message.value(QStringLiteral("stage")).toString()
                        == QLatin1String("repair")) {
                    return true;
                }
            }
            return false;
        }(),
        90000);
    transport.cancel();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);
    QJsonObject cancelled;
    ASSERT_TRUE(
        sawEvent(messages, QStringLiteral("cancelled"), &cancelled));
    const QString cancelledState =
        cancelled.value(QStringLiteral("trackingState")).toString();
    EXPECT_TRUE(cancelledState == QLatin1String("complete")
                || cancelledState == QLatin1String("lowConfidence"))
        << cancelledState.toStdString();

    RemovalCache cache(cacheRoot);
    const auto manifest = cache.readManifest(cacheKey, &error);
    ASSERT_TRUE(manifest.has_value()) << error.toStdString();
    EXPECT_EQ(manifest->status, QStringLiteral("partial"));
    EXPECT_LE(manifest->chunks.size(), 1);
}

TEST(AiEngineEndToEnd, UnknownInpaintModelFailsInsteadOfSubstitutingDefault)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheRoot = temp.filePath(QStringLiteral("cache"));
    const QString cacheKey = QString(64, QLatin1Char('d'));
    const QString cacheDir = QDir(cacheRoot).filePath(cacheKey);
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    AiJobSpec spec = makeEngineSpec(fixturePath(), modelsDir, sidecarRoot,
                                    cacheDir, 1, maskHash);
    spec.trackingOnly = false;
    // The id was part of the cache key; it must not silently fall back to the
    // mode default.
    spec.inpaintModelId = QStringLiteral("made-up-inpaint-model");
    spec.cacheKey = cacheKey;
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);

    QJsonObject errorEvent;
    ASSERT_TRUE(sawEvent(messages, QStringLiteral("error"), &errorEvent));
    EXPECT_EQ(errorEvent.value(QStringLiteral("code")).toString(),
              QStringLiteral("model_unavailable"));
    EXPECT_NE(
        errorEvent.value(QStringLiteral("message")).toString().indexOf(
            QStringLiteral("修复模型不存在")),
        -1);
    EXPECT_EQ(finished.at(0).at(0).toInt(), 3);
}

TEST(AiEngineEndToEnd, RedoAfterPartialStaysPartialUntilAllChunksExist)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheRoot = temp.filePath(QStringLiteral("cache"));
    const QString cacheKey = QString(64, QLatin1Char('c'));
    const QString cacheDir = QDir(cacheRoot).filePath(cacheKey);
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(640, 360));
    ASSERT_EQ(maskHash.size(), 64);

    // 31 frames = two chunks. A redo skips tracking, so pre-write the masks:
    // frame 0 has a hole, frames 1..30 are empty.
    constexpr int kFrames = 31;
    AiJobSpec spec = makeEngineSpec(fixturePath(), modelsDir, sidecarRoot,
                                    cacheDir, kFrames, maskHash);
    spec.trackingOnly = false;
    spec.inpaintModelId = QStringLiteral("migan-512");
    spec.cacheKey = cacheKey;
    spec.redoFirstFrame = 0;
    spec.redoLastFrame = 0;

    const BinaryMask hole = centeredMask(640, 360);
    BinaryMask empty;
    empty.width = 640;
    empty.height = 360;
    empty.bits.assign(size_t(640) * size_t(360), 0);
    for (int frame = 0; frame < kFrames; ++frame) {
        ASSERT_TRUE(writeTrackingMask(cacheDir, frame,
                                      frame == 0 ? hole : empty));
    }
    EngineEnvironmentGuard guard;

    const auto runJob = [&](const AiJobSpec &job) -> bool {
        LocalAiTransport transport;
        QSignalSpy messages(&transport, &AiTransport::message);
        QSignalSpy finished(&transport, &AiTransport::finished);
        QString startError;
        if (!transport.start(job, &startError)) {
            ADD_FAILURE() << startError.toStdString();
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        while (finished.count() < 1 && timer.elapsed() < 60000)
            QTest::qWait(25);
        return finished.count() >= 1
            && sawEvent(messages, QStringLiteral("done"));
    };

    // Only chunk 0 is repaired; chunk 1 is still absent, so the manifest must
    // stay partial even though this run was not cancelled.
    ASSERT_TRUE(runJob(spec));
    RemovalCache cache(cacheRoot);
    QString error;
    auto manifest = cache.readManifest(cacheKey, &error);
    ASSERT_TRUE(manifest.has_value()) << error.toStdString();
    EXPECT_EQ(manifest->status, QStringLiteral("partial"));
    ASSERT_EQ(manifest->chunks.size(), 1);
    EXPECT_EQ(manifest->chunks.first().index, 0);

    // Repair chunk 1 (all-empty masks): now every index exists and its chunk
    // directory is present, so the manifest may become complete.
    AiJobSpec second = spec;
    second.redoFirstFrame = 30;
    second.redoLastFrame = 30;
    ASSERT_TRUE(runJob(second));
    manifest = cache.readManifest(cacheKey, &error);
    ASSERT_TRUE(manifest.has_value()) << error.toStdString();
    EXPECT_EQ(manifest->status, QStringLiteral("complete"));
    EXPECT_EQ(manifest->chunks.size(), 2);
    EXPECT_TRUE(cache.verify(cacheKey, &error)) << error.toStdString();

    const auto emptyChunk = cache.readChunkIndex(cacheKey, 1, &error);
    ASSERT_TRUE(emptyChunk.has_value()) << error.toStdString();
    EXPECT_EQ(emptyChunk->firstFrame, 30);
    EXPECT_TRUE(emptyChunk->frames.isEmpty());
}

// Field report: on a long portrait clip the 取消 button seemed to take a long
// time. Measures the engine-side latency between the cancel command and the
// cancelled event, so a cooperative-cancel regression (falling back to the
// transport's 5 s kill) fails here instead of only in manual UAT.
TEST(AiEngineEndToEnd, CancelLatencyOnLongTrackingJob)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (!skipUnlessEngineAvailable())
        GTEST_SKIP() << "VisionAiEngine.exe, h264.mp4 or "
                        "VIDEOEDITOR_M4_MODELS is unavailable";
    const QString longAsset = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                                  .absoluteFilePath(
                                      QStringLiteral("../../badminton.mp4"));
    if (!QFileInfo::exists(longAsset))
        GTEST_SKIP() << "missing badminton.mp4 next to the repo";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString sidecarRoot = temp.filePath(QStringLiteral("sidecar"));
    const QString cacheDir = temp.filePath(QStringLiteral("cache"));
    // The field job ran at 720x1280 (the source resolution), so reproduce that
    // geometry rather than the tiny fixture.
    const QString maskHash =
        writeMaskSidecar(sidecarRoot, centeredMask(720, 1280));
    ASSERT_EQ(maskHash.size(), 64);

    AiJobSpec spec = makeEngineSpec(longAsset, modelsDir, sidecarRoot,
                                    cacheDir, 2300, maskHash);
    spec.width = 720;
    spec.height = 1280;
    spec.sourceOutUs = framesToMicros(2300, Rational{30, 1});
    EngineEnvironmentGuard guard;

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    // Let tracking run for a few seconds so the cancel lands mid-stream, not
    // during model initialization.
    QTRY_VERIFY_WITH_TIMEOUT(progressCount(messages) >= 5, 60000);
    QTest::qWait(3000);

    QElapsedTimer cancelClock;
    cancelClock.start();
    transport.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 15000);
    const qint64 latencyMs = cancelClock.elapsed();

    QJsonObject cancelled;
    EXPECT_TRUE(sawEvent(messages, QStringLiteral("cancelled"), &cancelled))
        << "engine never emitted cancelled (transport kill fallback?)";
    std::cout << "CANCEL_LATENCY_MS=" << latencyMs << std::endl;
    // Cooperative cancel is checked once per tracked frame; even at 10 fps
    // that is well under two seconds. Anything near the 5 s kill timer means
    // the command was not polled.
    EXPECT_LT(latencyMs, 2000) << "cancel latency too high";
}

#endif // VIDEOEDITOR_HAVE_ORT
