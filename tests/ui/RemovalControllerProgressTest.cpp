#include <gtest/gtest.h>

#include <memory>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "AiFeatureGate.h"
#include "AiJobManager.h"
#include "AiProtocol.h"
#include "AiTransport.h"
#include "MaskSidecarStore.h"
#include "ModelRegistry.h"
#include "ProjectDocument.h"
#include "RemovalCache.h"
#include "RemovalCacheKey.h"
#include "RemovalController.h"
#include "RemovalErrors.h"
#include "RemovalPreflight.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

namespace {

class FakeTransport : public AiTransport {
public:
    enum class Mode { Complete, Silent, Error, ProgressHold, DoneAfterCancel };

    explicit FakeTransport(Mode mode, AiJobSpec *captured)
        : m_mode(mode)
        , m_captured(captured)
    {
    }

    bool start(const AiJobSpec &spec, QString *error) override
    {
        Q_UNUSED(error)
        m_spec = spec;
        if (m_captured)
            *m_captured = spec;
        if (m_mode == Mode::ProgressHold) {
            QTimer::singleShot(0, this, [this] {
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("progress")},
                    {QStringLiteral("stage"), QStringLiteral("tracking")},
                    {QStringLiteral("percent"), 42.0},
                    {QStringLiteral("etaText"), QStringLiteral("10 秒")}});
            });
        } else if (m_mode == Mode::Complete) {
            QTimer::singleShot(0, this, [this] {
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("done")},
                    {QStringLiteral("cacheKey"), m_spec.cacheKey},
                    {QStringLiteral("confidenceHash"),
                     QString(64, QLatin1Char('a'))},
                    {QStringLiteral("trackingState"),
                     QStringLiteral("complete")}});
                emit finished(0, QProcess::NormalExit);
            });
        } else if (m_mode == Mode::Error) {
            QTimer::singleShot(0, this, [this] {
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("error")},
                    {QStringLiteral("code"), QStringLiteral("vram")},
                    {QStringLiteral("message"),
                     QStringLiteral("out of memory")}});
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("done")},
                    {QStringLiteral("cacheKey"), m_spec.cacheKey}});
                emit finished(1, QProcess::NormalExit);
            });
        }
        return true;
    }

    void cancel() override
    {
        if (m_mode == Mode::DoneAfterCancel)
            return; // the test queues a done after the cancel request
        emit message(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("cancelled")},
            {QStringLiteral("completedFrames"), 30},
            {QStringLiteral("trackingState"), QStringLiteral("partial")}});
        emit finished(0, QProcess::NormalExit);
    }

    void emitMessage(const QJsonObject &message) { emit this->message(message); }

    AiJobSpec m_spec;

private:
    Mode m_mode;
    AiJobSpec *m_captured;
};

class RejectingTransport : public AiTransport {
public:
    bool start(const AiJobSpec &, QString *error) override
    {
        if (error)
            *error = QStringLiteral("无法启动 AI 推理引擎。");
        return false;
    }
    void cancel() override {}
};

ModelRegistry testRegistry(const QString &rootDir)
{
    ModelManifest manifest;
    manifest.schemaVersion = 1;
    manifest.pipelineVersion = QStringLiteral("m4.0");

    AiModelRecord tracking;
    tracking.id = QStringLiteral("efficienttam-ti-512-fp32");
    tracking.family = QStringLiteral("tracking");
    tracking.files.insert(QStringLiteral("image_encoder"),
                          AiModelFile{QStringLiteral("t.onnx"), QString(64, QLatin1Char('a'))});
    tracking.files.insert(QStringLiteral("prompt_encoder"),
                          AiModelFile{QStringLiteral("t.onnx"), QString(64, QLatin1Char('a'))});
    tracking.files.insert(QStringLiteral("mask_decoder"),
                          AiModelFile{QStringLiteral("t.onnx"), QString(64, QLatin1Char('a'))});

    AiModelRecord fast;
    fast.id = QStringLiteral("migan-512");
    fast.family = QStringLiteral("inpainting");

    AiModelRecord quality;
    quality.id = QStringLiteral("lama-512-int8-allowzero0");
    quality.family = QStringLiteral("inpainting");

    manifest.models = {tracking, fast, quality};
    return ModelRegistry::forTest(manifest, rootDir);
}

Project projectWithRemovalLayer(const QString &confidenceHash = {})
{
    Project project = projectWithClip();
    project.assets[0].info.frameRate = Rational{30, 1};
    RemovalLayer layer = sampleLayer();
    layer.trackingConfidenceHash = confidenceHash;
    project.tracks[0].clips[0].removalLayerId = layer.id;
    project.removalLayers.append(layer);
    return project;
}

RemovalCacheManifest makeManifest(const QString &key, const QString &status,
                                  const QList<int> &chunkIndices,
                                  int totalFrames = 150)
{
    RemovalCacheManifest manifest;
    manifest.schema = QStringLiteral("m4.removal.cache.1");
    manifest.cacheKey = key;
    manifest.clipId = QStringLiteral("clip-1");
    manifest.sourceInUs = 0;
    manifest.sourceOutUs = 10'000'000;
    manifest.width = 1920;
    manifest.height = 1080;
    manifest.fps = Rational{30, 1};
    manifest.trackingModelId = QStringLiteral("efficienttam-ti-512-fp32");
    manifest.inpaintModelId = QStringLiteral("migan-512");
    manifest.pipelineVersion = QStringLiteral("m4.0");
    manifest.status = status;
    manifest.totalFrames = totalFrames;
    manifest.createdAt = QStringLiteral("2026-11-16T00:00:00Z");
    for (const int index : chunkIndices) {
        manifest.chunks.append(
            RemovalCacheChunk{index, index * 30, 30, QString(64, QLatin1Char('a'))});
    }
    return manifest;
}

void makeChunkDir(const RemovalCache &cache, const QString &key, int index)
{
    QDir().mkpath(QDir(cache.cacheDir(key))
                      .filePath(QStringLiteral("chunks/%1").arg(index)));
}

qint64 frameUs(int frame)
{
    return qint64(frame) * 1'000'000 / 30;
}

struct Harness {
    QTemporaryDir dir;
    ProjectDocument doc;
    MaskSidecarStore sidecars;
    RemovalCache cache;
    AiFeatureGate gate;
    AiJobManager manager;
    RemovalController controller;
    FakeTransport::Mode mode = FakeTransport::Mode::Silent;
    FakeTransport *transport = nullptr;
    AiJobSpec captured;
    QSignalSpy finished;

    Harness()
        : sidecars(dir.filePath(QStringLiteral("p.veproj")))
        , cache(QDir(dir.path()).filePath(QStringLiteral("cache")))
        , manager(gate)
        , controller(doc, sidecars)
        , finished(&manager, &AiJobManager::jobFinished)
    {
        gate.setRegistryForTest(testRegistry(dir.path()));
        gate.setEnabledForTest(true);
        controller.setJobManager(&manager);
        controller.setCache(&cache);
        controller.setFeatureGate(&gate);
        manager.setTransportFactoryForTest([this] {
            auto transport =
                std::make_unique<FakeTransport>(mode, &captured);
            this->transport = transport.get();
            return std::unique_ptr<AiTransport>(transport.release());
        });
    }

    QString keyForProject() const
    {
        const Project &project = doc.project();
        const Clip &clip = project.tracks[0].clips[0];
        const RemovalLayer &layer = project.removalLayers.first();
        return computeRemovalCacheKey(RemovalPreflight::keyInputFor(
            project, clip, layer, *gate.registry()));
    }
};

} // namespace

TEST(RemovalErrors, MapsCodesToChineseActionableMessages)
{
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("vram"), QString())
                    .contains(QStringLiteral("显存不足")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("device_lost"), QString())
                    .contains(QStringLiteral("设备已重置")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("model_missing"), QString())
                    .contains(QStringLiteral("重新安装")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("disk_full"), QString())
                    .contains(QStringLiteral("磁盘空间不足")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("low_confidence"), QString())
                    .contains(QStringLiteral("置信度低")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("cancelled"), QString())
                    .contains(QStringLiteral("已取消")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("unknown"),
                                    QStringLiteral("x"))
                    .contains(QStringLiteral("x")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("unknown"), QString())
                    .contains(QStringLiteral("重试")));
}

TEST(RemovalControllerProgress, PartialRedoInvalidatesOnlyIntersectingChunks)
{
    Harness harness;
    harness.doc.setProject(projectWithRemovalLayer());
    const QString key = harness.keyForProject();
    ASSERT_FALSE(key.isEmpty());

    // Simulate an earlier complete run with five 30-frame chunks.
    RemovalCacheManifest manifest = makeManifest(
        key, QStringLiteral("complete"), {0, 1, 2, 3, 4}, 150);
    QString error;
    ASSERT_TRUE(harness.cache.writeManifest(manifest, &error))
        << error.toStdString();
    for (int index = 0; index < 5; ++index)
        makeChunkDir(harness.cache, key, index);

    // Persist the key so redoRange() invalidates the manifest we just wrote.
    {
        Project project = projectWithRemovalLayer();
        project.removalLayers[0].cacheKey = key;
        project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
        harness.doc.setProject(project);
    }

    harness.controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(harness.controller.maskMode());

    // Frames 70..80 live in chunk 2; ±1 chunk covers chunks 1..3, so chunks 0
    // and 4 survive while the manifest becomes partial.
    harness.controller.redoRange(frameUs(70), frameUs(80));

    const QString chunksRoot =
        QDir(harness.cache.cacheDir(key)).filePath(QStringLiteral("chunks"));
    EXPECT_TRUE(QDir(QDir(chunksRoot).filePath(QStringLiteral("0"))).exists());
    EXPECT_FALSE(QDir(QDir(chunksRoot).filePath(QStringLiteral("1"))).exists());
    EXPECT_FALSE(QDir(QDir(chunksRoot).filePath(QStringLiteral("2"))).exists());
    EXPECT_FALSE(QDir(QDir(chunksRoot).filePath(QStringLiteral("3"))).exists());
    EXPECT_TRUE(QDir(QDir(chunksRoot).filePath(QStringLiteral("4"))).exists());

    const auto updated = harness.cache.readManifest(key, &error);
    ASSERT_TRUE(updated.has_value()) << error.toStdString();
    EXPECT_EQ(updated->status, QStringLiteral("partial"));
    ASSERT_EQ(updated->chunks.size(), 2);
    EXPECT_EQ(updated->chunks[0].index, 0);
    EXPECT_EQ(updated->chunks[1].index, 4);

    // The redo job was accepted before invalidation: the engine owns the
    // repair writes from here and sees the chunks already removed.
    EXPECT_FALSE(harness.captured.jobId.isEmpty());
    EXPECT_TRUE(harness.captured.resume);
    EXPECT_EQ(harness.captured.redoFirstFrame, 30);
    EXPECT_EQ(harness.captured.redoLastFrame, 119);

    harness.controller.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
}

TEST(RemovalControllerProgress, CancelKeepsCompletedChunksAndMarksPartial)
{
    Harness harness;
    harness.doc.setProject(projectWithRemovalLayer());
    const QString key = harness.keyForProject();

    RemovalCacheManifest manifest = makeManifest(
        key, QStringLiteral("partial"), {0}, 150);
    QString error;
    ASSERT_TRUE(harness.cache.writeManifest(manifest, &error))
        << error.toStdString();
    makeChunkDir(harness.cache, key, 0);

    Project project = projectWithRemovalLayer();
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    harness.doc.setProject(project);

    harness.controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(harness.controller.maskMode());
    ASSERT_TRUE(harness.controller.generate());
    EXPECT_TRUE(harness.controller.generating());

    harness.controller.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);

    const RemovalLayer &layer = harness.doc.project().removalLayers.first();
    EXPECT_EQ(layer.trackingState, RemovalTrackingState::Partial);
    EXPECT_TRUE(QDir(QDir(harness.cache.cacheDir(key))
                         .filePath(QStringLiteral("chunks/0")))
                    .exists());
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("已取消")));
    EXPECT_FALSE(harness.controller.generating());
}

TEST(RemovalControllerProgress, PartialManifestOffersResumeAndReadsConfidence)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    QString trackingHash;
    {
        QJsonArray frames;
        for (int i = 0; i < 3; ++i) {
            frames.append(QJsonObject{
                {QStringLiteral("timeUs"), i * 1'000'000},
                {QStringLiteral("state"),
                 i == 1 ? QStringLiteral("low") : QStringLiteral("good")}});
        }
        const QJsonObject tracking{{QStringLiteral("frames"), frames}};
        ASSERT_TRUE(sidecars.writeTracking(tracking, &trackingHash, nullptr));
    }

    RemovalCache cache(QDir(dir.path()).filePath(QStringLiteral("cache")));
    AiFeatureGate gate;
    gate.setRegistryForTest(testRegistry(dir.path()));
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    RemovalController controller(doc, sidecars);
    controller.setJobManager(&manager);
    controller.setFeatureGate(&gate);
    controller.setCache(&cache);

    Project project = projectWithRemovalLayer(trackingHash);
    const QString key = computeRemovalCacheKey(RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], project.removalLayers[0],
        *gate.registry()));
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    RemovalCacheManifest manifest = makeManifest(
        key, QStringLiteral("partial"), {0}, 90);
    QString error;
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();
    doc.setProject(project);
    controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(controller.maskMode());

    EXPECT_EQ(controller.cacheStatusText(), QStringLiteral("部分可继续"));
    EXPECT_TRUE(controller.canContinueGeneration());
    EXPECT_EQ(controller.lowConfidenceCount(), 1);
    EXPECT_EQ(controller.confidenceBand().size(), 3);
}

TEST(RemovalControllerProgress, ConfidenceBandMapsSourceToTimelineAndSeeks)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    QString trackingHash;
    {
        QJsonArray frames{
            QJsonObject{{QStringLiteral("timeUs"), 0},
                        {QStringLiteral("state"), QStringLiteral("good")}},
            QJsonObject{{QStringLiteral("timeUs"), 1'000'000},
                        {QStringLiteral("state"), QStringLiteral("low")}},
            QJsonObject{{QStringLiteral("timeUs"), 2'000'000},
                        {QStringLiteral("state"), QStringLiteral("fair")}},
            QJsonObject{{QStringLiteral("timeUs"), 3'000'000},
                        {QStringLiteral("state"), QStringLiteral("missing")}}};
        const QJsonObject tracking{{QStringLiteral("frames"), frames}};
        ASSERT_TRUE(sidecars.writeTracking(tracking, &trackingHash, nullptr));
    }

    AiFeatureGate gate;
    gate.setRegistryForTest(testRegistry(dir.path()));
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    RemovalController controller(doc, sidecars);
    controller.setJobManager(&manager);
    controller.setFeatureGate(&gate);

    Project project = projectWithRemovalLayer(trackingHash);
    project.tracks[0].clips[0].timelineStartUs = 2'000'000;
    doc.setProject(project);
    controller.beginMask(QStringLiteral("clip-1"));

    const QVariantList band = controller.confidenceBand();
    ASSERT_EQ(band.size(), 4);
    EXPECT_EQ(controller.lowConfidenceCount(), 2);
    EXPECT_EQ(band[0].toMap().value(QStringLiteral("color")).toString(),
              QStringLiteral("#4CAF50"));
    EXPECT_EQ(band[1].toMap().value(QStringLiteral("color")).toString(),
              QStringLiteral("#F44336"));
    EXPECT_EQ(band[2].toMap().value(QStringLiteral("color")).toString(),
              QStringLiteral("#FFC107"));
    EXPECT_EQ(band[3].toMap().value(QStringLiteral("color")).toString(),
              QStringLiteral("#9E9E9E"));
    EXPECT_EQ(band[1].toMap().value(QStringLiteral("timelineUs")).toLongLong(),
              3'000'000);

    qint64 sought = -1;
    controller.setSeekCallback([&sought](qint64 timelineUs) {
        sought = timelineUs;
    });
    controller.jumpToConfidence(3);
    EXPECT_EQ(sought, 5'000'000);
}

TEST(RemovalControllerProgress, GenerateCacheKeyMatchesPreflightPerMode)
{
    for (const RepairMode mode : {RepairMode::Fast, RepairMode::Quality}) {
        Harness harness;
        harness.mode = FakeTransport::Mode::Complete;
        Project project = projectWithRemovalLayer();
        project.removalLayers[0].parameters.mode = mode;
        project.removalLayers[0].cacheKey.clear();
        project.removalLayers[0].cachePath.clear();
        harness.doc.setProject(project);

        const Project &stored = harness.doc.project();
        const QString expected = computeRemovalCacheKey(
            RemovalPreflight::keyInputFor(
                stored, stored.tracks[0].clips[0], stored.removalLayers[0],
                *harness.gate.registry()));

        harness.controller.beginMask(QStringLiteral("clip-1"));
        const int undoBefore = harness.doc.undoStack()->index();
        ASSERT_TRUE(harness.controller.generate());
        // A job start must not mutate the project; the completion command writes
        // the key exactly once.
        EXPECT_TRUE(harness.doc.project().removalLayers[0].cacheKey.isEmpty());
        EXPECT_EQ(harness.doc.undoStack()->index(), undoBefore);
        EXPECT_EQ(harness.captured.cacheKey, expected);

        QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
        EXPECT_EQ(harness.doc.project().removalLayers[0].cacheKey, expected);
        EXPECT_EQ(harness.doc.project().removalLayers[0].cachePath,
                  QStringLiteral("removal/") + expected);
        EXPECT_EQ(harness.doc.undoStack()->index(), undoBefore + 1);
    }
}

TEST(RemovalControllerProgress, AdvancedParameterChangesClearStaleKey)
{
    Harness harness;
    Project project = projectWithRemovalLayer();
    project.removalLayers[0].cacheKey = QString(64, QLatin1Char('d'));
    project.removalLayers[0].cachePath = QStringLiteral("removal/")
                                         + project.removalLayers[0].cacheKey;
    harness.doc.setProject(project);
    harness.controller.beginMask(QStringLiteral("clip-1"));

    int index = harness.doc.undoStack()->index();
    harness.controller.setAdvancedMode(QStringLiteral("quality"));
    EXPECT_EQ(harness.doc.undoStack()->index(), index + 1);
    EXPECT_TRUE(harness.doc.project().removalLayers[0].cacheKey.isEmpty());
    EXPECT_TRUE(harness.doc.project().removalLayers[0].cachePath.isEmpty());
    EXPECT_EQ(harness.doc.project().removalLayers[0].parameters.mode,
              RepairMode::Quality);

    index = harness.doc.undoStack()->index();
    harness.controller.setProcessingHeight(720);
    EXPECT_EQ(harness.doc.undoStack()->index(), index + 1);
    EXPECT_EQ(harness.doc.project().removalLayers[0].parameters.processingHeight,
              720);
    EXPECT_EQ(harness.controller.cacheStatusText(), QStringLiteral("缺失"));

    index = harness.doc.undoStack()->index();
    harness.controller.setStabilizationStrength(100);
    EXPECT_EQ(harness.doc.undoStack()->index(), index + 1);
    EXPECT_EQ(
        harness.doc.project().removalLayers[0].parameters.stabilizationStrength,
        100);

    // A no-op selection must not create an undo step.
    index = harness.doc.undoStack()->index();
    harness.controller.setStabilizationStrength(100);
    EXPECT_EQ(harness.doc.undoStack()->index(), index);
}

TEST(RemovalControllerProgress, ProgressStageAndEtaAreForwardedAndCancelWorks)
{
    Harness harness;
    harness.mode = FakeTransport::Mode::ProgressHold;
    harness.doc.setProject(projectWithRemovalLayer());
    harness.controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(harness.controller.generate()) << harness.controller.error().toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(harness.controller.progress() > 41.0, 2000);
    EXPECT_EQ(harness.controller.stageText(), QStringLiteral("正在跟踪遮罩"));
    EXPECT_EQ(harness.controller.etaText(), QStringLiteral("10 秒"));

    harness.controller.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
    EXPECT_FALSE(harness.controller.generating());
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("已取消")));
}

TEST(RemovalControllerProgress, ErrorResultMarksFailedAndUsesMappedMessage)
{
    Harness harness;
    harness.mode = FakeTransport::Mode::Error;
    harness.doc.setProject(projectWithRemovalLayer());
    harness.controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(harness.controller.generate());

    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
    const RemovalLayer &layer = harness.doc.project().removalLayers.first();
    EXPECT_EQ(layer.trackingState, RemovalTrackingState::Failed);
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("显存不足")));
    EXPECT_FALSE(harness.controller.generating());
}

TEST(RemovalControllerProgress, CompareSinkReceivesInitialAndChangedValues)
{
    Harness harness;
    harness.doc.setProject(projectWithRemovalLayer());

    QString mode;
    int split = -1;
    harness.controller.setCompareSink(
        [&mode, &split](const QString &value, int permille) {
            mode = value;
            split = permille;
        });
    EXPECT_EQ(mode, QStringLiteral("repair"));
    EXPECT_EQ(split, 500);

    harness.controller.setCompareMode(QStringLiteral("split"));
    EXPECT_EQ(mode, QStringLiteral("split"));
    harness.controller.setSplitPermille(250);
    EXPECT_EQ(split, 250);
}

TEST(RemovalControllerProgress, ExportGenerationPendingIsCancelledCleanly)
{
    Harness harness;
    harness.mode = FakeTransport::Mode::Silent;
    harness.doc.setProject(projectWithRemovalLayer());
    harness.controller.beginMask(QStringLiteral("clip-1"));
    QSignalSpy exportSpy(&harness.controller,
                         &RemovalController::generationFinishedForExport);

    harness.controller.generateForExport();
    EXPECT_TRUE(harness.controller.exportGenerationPending());
    EXPECT_TRUE(harness.controller.generating());

    harness.controller.cancelExportGeneration();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
    EXPECT_FALSE(harness.controller.exportGenerationPending());
    EXPECT_EQ(exportSpy.count(), 0);
    EXPECT_FALSE(harness.controller.generating());
}

TEST(RemovalControllerProgress, DoneAfterCancelNeverStartsAnExport)
{
    Harness harness;
    harness.mode = FakeTransport::Mode::DoneAfterCancel;
    harness.doc.setProject(projectWithRemovalLayer());
    harness.controller.beginMask(QStringLiteral("clip-1"));
    QSignalSpy exportSpy(&harness.controller,
                         &RemovalController::generationFinishedForExport);

    harness.controller.generateForExport();
    ASSERT_TRUE(harness.controller.exportGenerationPending());
    harness.controller.cancelExportGeneration();
    EXPECT_TRUE(harness.manager.running())
        << "the fake has not acknowledged the cancel yet";

    // The engine races a successful done after the cancel request. It must be
    // reported as cancelled, and the disarmed export must not start.
    harness.transport->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("cacheKey"), harness.captured.cacheKey},
        {QStringLiteral("completedFrames"), 30},
        {QStringLiteral("trackingState"), QStringLiteral("complete")}});
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);

    const AiJobResult result =
        qvariant_cast<AiJobResult>(harness.finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(exportSpy.count(), 0);
    EXPECT_FALSE(harness.controller.exportGenerationPending());
}

TEST(RemovalControllerProgress, ExportGenerationSuccessEmitsOnce)
{
    Harness harness;
    harness.mode = FakeTransport::Mode::Complete;
    harness.doc.setProject(projectWithRemovalLayer());
    harness.controller.beginMask(QStringLiteral("clip-1"));
    QSignalSpy exportSpy(&harness.controller,
                         &RemovalController::generationFinishedForExport);

    harness.controller.generateForExport();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
    EXPECT_EQ(exportSpy.count(), 1);
    EXPECT_FALSE(harness.controller.exportGenerationPending());
}

TEST(RemovalControllerProgress, StartFailureLeavesLayerAndUndoUntouched)
{
    Harness harness;
    Project project = projectWithRemovalLayer();
    project.removalLayers[0].cacheKey = QString(64, QLatin1Char('d'));
    project.removalLayers[0].cachePath = QStringLiteral("removal/")
                                         + project.removalLayers[0].cacheKey;
    harness.doc.setProject(project);
    harness.controller.beginMask(QStringLiteral("clip-1"));
    const int undoBefore = harness.doc.undoStack()->index();

    harness.manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new RejectingTransport);
    });
    EXPECT_FALSE(harness.controller.generate());
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("无法启动")));
    EXPECT_EQ(harness.doc.project().removalLayers[0].cacheKey,
              QString(64, QLatin1Char('d')));
    EXPECT_EQ(harness.doc.undoStack()->index(), undoBefore);
    EXPECT_FALSE(harness.controller.generating());
}

TEST(RemovalControllerProgress, AdvancedEditsAreIgnoredWhileGenerating)
{
    Harness harness;
    harness.mode = FakeTransport::Mode::Silent;
    harness.doc.setProject(projectWithRemovalLayer());
    harness.controller.beginMask(QStringLiteral("clip-1"));
    const int undoBefore = harness.doc.undoStack()->index();
    ASSERT_TRUE(harness.controller.generate());

    harness.controller.setAdvancedMode(QStringLiteral("quality"));
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("运行中")));
    EXPECT_EQ(harness.doc.project().removalLayers[0].parameters.mode,
              RepairMode::Fast);
    EXPECT_EQ(harness.doc.undoStack()->index(), undoBefore);

    harness.controller.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
}

TEST(RemovalControllerProgress, RedoWhileRunningKeepsEveryChunk)
{
    Harness harness;
    Project project = projectWithRemovalLayer();
    const QString key = computeRemovalCacheKey(RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], project.removalLayers[0],
        *harness.gate.registry()));
    RemovalCacheManifest manifest = makeManifest(
        key, QStringLiteral("complete"), {0, 1, 2, 3, 4}, 150);
    QString error;
    ASSERT_TRUE(harness.cache.writeManifest(manifest, &error))
        << error.toStdString();
    for (int index = 0; index < 5; ++index)
        makeChunkDir(harness.cache, key, index);
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    harness.doc.setProject(project);
    harness.controller.beginMask(QStringLiteral("clip-1"));

    harness.mode = FakeTransport::Mode::Silent;
    ASSERT_TRUE(harness.controller.generate());
    harness.controller.redoRange(frameUs(70), frameUs(80));
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("已有")));

    const QString chunksRoot =
        QDir(harness.cache.cacheDir(key)).filePath(QStringLiteral("chunks"));
    for (int index = 0; index < 5; ++index) {
        EXPECT_TRUE(QDir(QDir(chunksRoot)
                             .filePath(QString::number(index)))
                        .exists())
            << index;
    }
    const auto survived = harness.cache.readManifest(key, &error);
    ASSERT_TRUE(survived.has_value()) << error.toStdString();
    EXPECT_EQ(survived->status, QStringLiteral("complete"));

    harness.controller.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
}

TEST(RemovalControllerProgress, RedoStartFailureKeepsEveryChunk)
{
    Harness harness;
    Project project = projectWithRemovalLayer();
    const QString key = computeRemovalCacheKey(RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], project.removalLayers[0],
        *harness.gate.registry()));
    RemovalCacheManifest manifest = makeManifest(
        key, QStringLiteral("complete"), {0, 1, 2, 3, 4}, 150);
    QString error;
    ASSERT_TRUE(harness.cache.writeManifest(manifest, &error))
        << error.toStdString();
    for (int index = 0; index < 5; ++index)
        makeChunkDir(harness.cache, key, index);
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    harness.doc.setProject(project);
    harness.controller.beginMask(QStringLiteral("clip-1"));

    harness.manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new RejectingTransport);
    });
    harness.controller.redoRange(frameUs(70), frameUs(80));
    EXPECT_TRUE(harness.controller.error().contains(QStringLiteral("无法启动")))
        << harness.controller.error().toStdString();

    const QString chunksRoot =
        QDir(harness.cache.cacheDir(key)).filePath(QStringLiteral("chunks"));
    for (int index = 0; index < 5; ++index) {
        EXPECT_TRUE(QDir(QDir(chunksRoot)
                             .filePath(QString::number(index)))
                        .exists())
            << index;
    }
    const auto survived = harness.cache.readManifest(key, &error);
    ASSERT_TRUE(survived.has_value()) << error.toStdString();
    EXPECT_EQ(survived->status, QStringLiteral("complete"));
}

TEST(RemovalControllerProgress, RedoRangeClampsToLayerBounds)
{
    Harness harness;
    Project project = projectWithRemovalLayer();
    const QString key = computeRemovalCacheKey(RemovalPreflight::keyInputFor(
        project, project.tracks[0].clips[0], project.removalLayers[0],
        *harness.gate.registry()));
    RemovalCacheManifest manifest = makeManifest(
        key, QStringLiteral("complete"), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 300);
    QString error;
    ASSERT_TRUE(harness.cache.writeManifest(manifest, &error))
        << error.toStdString();
    project.removalLayers[0].cacheKey = key;
    project.removalLayers[0].cachePath = QStringLiteral("removal/") + key;
    harness.doc.setProject(project);
    harness.controller.beginMask(QStringLiteral("clip-1"));

    harness.controller.redoRange(-5'000'000, 20'000'000);
    EXPECT_EQ(harness.captured.redoFirstFrame, 0);
    EXPECT_EQ(harness.captured.redoLastFrame, 299);

    harness.controller.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(harness.finished.count() == 1, 2000);
}

TEST(RemovalControllerProgress, ConfidenceMappingHandlesHalfSpeedAndOutOfRange)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    QString trackingHash;
    {
        QJsonArray frames{
            QJsonObject{{QStringLiteral("timeUs"), -2'000'000},
                        {QStringLiteral("state"), QStringLiteral("missing")}},
            QJsonObject{{QStringLiteral("timeUs"), 1'000'000},
                        {QStringLiteral("state"), QStringLiteral("low")}},
            QJsonObject{{QStringLiteral("timeUs"), 20'000'000},
                        {QStringLiteral("state"), QStringLiteral("good")}}};
        ASSERT_TRUE(sidecars.writeTracking(
            QJsonObject{{QStringLiteral("frames"), frames}}, &trackingHash,
            nullptr));
    }
    AiFeatureGate gate;
    gate.setRegistryForTest(testRegistry(dir.path()));
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    RemovalController controller(doc, sidecars);
    controller.setJobManager(&manager);
    controller.setFeatureGate(&gate);

    Project project = projectWithRemovalLayer(trackingHash);
    project.tracks[0].clips[0].timelineStartUs = 0;
    project.tracks[0].clips[0].speedMilli = 500; // 1 s source = 2 s timeline
    doc.setProject(project);
    controller.beginMask(QStringLiteral("clip-1"));

    const QVariantList band = controller.confidenceBand();
    ASSERT_EQ(band.size(), 3);
    EXPECT_EQ(band[0].toMap().value(QStringLiteral("timelineUs")).toLongLong(),
              0); // -2 s clamps to sourceIn
    EXPECT_EQ(band[1].toMap().value(QStringLiteral("timelineUs")).toLongLong(),
              2'000'000); // 1 s source at 0.5x
    EXPECT_EQ(band[2].toMap().value(QStringLiteral("timelineUs")).toLongLong(),
              20'000'000); // 20 s clamps to sourceOut, then 10 s at 0.5x

    qint64 sought = -1;
    controller.setSeekCallback([&sought](qint64 timelineUs) {
        sought = timelineUs;
    });
    controller.jumpToConfidence(2);
    EXPECT_EQ(sought, 20'000'000);
}

TEST(RemovalControllerProgress, ConfidenceMappingHonoursNonUnitSpeed)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    QString trackingHash;
    {
        QJsonArray frames{
            QJsonObject{{QStringLiteral("timeUs"), 0},
                        {QStringLiteral("state"), QStringLiteral("good")}},
            QJsonObject{{QStringLiteral("timeUs"), 1'000'000},
                        {QStringLiteral("state"), QStringLiteral("low")}}};
        ASSERT_TRUE(sidecars.writeTracking(
            QJsonObject{{QStringLiteral("frames"), frames}}, &trackingHash,
            nullptr));
    }
    AiFeatureGate gate;
    gate.setRegistryForTest(testRegistry(dir.path()));
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    RemovalController controller(doc, sidecars);
    controller.setJobManager(&manager);
    controller.setFeatureGate(&gate);

    Project project = projectWithRemovalLayer(trackingHash);
    project.tracks[0].clips[0].timelineStartUs = 1'000'000;
    project.tracks[0].clips[0].speedMilli = 2000; // 2x: 1 s source = 0.5 s timeline
    doc.setProject(project);
    controller.beginMask(QStringLiteral("clip-1"));

    const QVariantList band = controller.confidenceBand();
    ASSERT_EQ(band.size(), 2);
    EXPECT_EQ(band[1].toMap().value(QStringLiteral("timelineUs")).toLongLong(),
              1'500'000);

    qint64 sought = -1;
    controller.setSeekCallback([&sought](qint64 timelineUs) {
        sought = timelineUs;
    });
    controller.jumpToConfidence(1);
    EXPECT_EQ(sought, 1'500'000);
}

TEST(RemovalControllerProgress, ExportGenerationTargetsTheFailingClip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectDocument doc;
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    AiFeatureGate gate;
    gate.setRegistryForTest(testRegistry(dir.path()));
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    RemovalCache cache(QDir(dir.path()).filePath(QStringLiteral("cache")));
    RemovalController controller(doc, sidecars);
    controller.setJobManager(&manager);
    controller.setCache(&cache);
    controller.setFeatureGate(&gate);
    // The selected clip is clip-1; generation must follow the failing clip.
    controller.setSelectedClipProvider(
        [] { return QStringLiteral("clip-1"); });

    Project project = projectWithClip();
    project.assets[0].info.frameRate = Rational{30, 1};
    RemovalLayer first = sampleLayer();
    first.id = QStringLiteral("rl-1");
    first.clipId = QStringLiteral("clip-1");
    project.removalLayers.append(first);
    project.tracks[0].clips[0].removalLayerId = first.id;

    Clip second = project.tracks[0].clips[0];
    second.id = QStringLiteral("clip-2");
    second.timelineStartUs = 2'000'000;
    second.sourceInUs = 0;
    second.sourceOutUs = 10'000'000;
    second.removalLayerId = QStringLiteral("rl-2");
    project.tracks[0].clips.append(second);
    RemovalLayer secondLayer = sampleLayer();
    secondLayer.id = QStringLiteral("rl-2");
    secondLayer.clipId = second.id;
    project.removalLayers.append(secondLayer);
    doc.setProject(project);

    AiJobSpec captured;
    manager.setTransportFactoryForTest([&captured] {
        return std::unique_ptr<AiTransport>(
            new FakeTransport(FakeTransport::Mode::Silent, &captured));
    });

    controller.generateForExportForClip(QStringLiteral("clip-2"));
    ASSERT_FALSE(captured.clipId.isEmpty())
        << controller.error().toStdString();
    EXPECT_EQ(captured.clipId, QStringLiteral("clip-2"))
        << "the prompt must generate the failing clip, not the selection";
    EXPECT_TRUE(controller.exportGenerationPending());
    EXPECT_TRUE(manager.running());

    controller.cancelExportGeneration();
    EXPECT_FALSE(controller.exportGenerationPending());
}
