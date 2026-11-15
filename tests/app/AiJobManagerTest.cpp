#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include "AppController.h"
#include "AudioSink.h"
#include "LocalAiTransport.h"
#include "MaskSidecarStore.h"
#include "ModelRegistry.h"
#include "ProjectDocument.h"
#include "RemovalController.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

namespace {

// The fake only emits the signals inherited from AiTransport, so it needs no
// Q_OBJECT/metaobject of its own (and therefore no .moc include).
class FakeTransport : public AiTransport {
public:
    enum class Mode { ProgressDone, ErrorThenDone, Silent };

    explicit FakeTransport(Mode mode = Mode::ProgressDone) : m_mode(mode) {}

    bool start(const AiJobSpec &spec, QString *error) override
    {
        Q_UNUSED(error)
        m_spec = spec;
        if (m_mode == Mode::ProgressDone) {
            QTimer::singleShot(0, this, [this] {
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("progress")},
                    {QStringLiteral("stage"), QStringLiteral("tracking")},
                    {QStringLiteral("frame"), 1},
                    {QStringLiteral("totalFrames"), 10},
                    {QStringLiteral("percent"), 10.0}});
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("done")},
                    {QStringLiteral("cacheKey"), m_spec.cacheKey},
                    {QStringLiteral("confidenceHash"),
                     QString(64, QLatin1Char('a'))}});
                emit finished(0, QProcess::NormalExit);
            });
        } else if (m_mode == Mode::ErrorThenDone) {
            QTimer::singleShot(0, this, [this] {
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("error")},
                    {QStringLiteral("code"),
                     QStringLiteral("model_unavailable")},
                    {QStringLiteral("message"),
                     QStringLiteral("AI 模型尚未安装。")}});
                emit message(QJsonObject{
                    {QStringLiteral("event"), QStringLiteral("done")},
                    {QStringLiteral("cacheKey"), m_spec.cacheKey}});
                emit finished(3, QProcess::NormalExit);
            });
        }
        return true;
    }

    void cancel() override { emit finished(0, QProcess::NormalExit); }

    void sendCommand(const QJsonObject &command) override
    {
        commands.append(command);
    }

    void emitMessage(const QJsonObject &message) { emit this->message(message); }
    void emitFinished(int code, QProcess::ExitStatus status)
    {
        emit finished(code, status);
    }

    AiJobSpec m_spec;
    QList<QJsonObject> commands;

private:
    Mode m_mode;
};

AiJobSpec makeSpec(const QString &jobId = QStringLiteral("job-1"))
{
    AiJobSpec spec;
    spec.jobId = jobId;
    spec.clipId = QStringLiteral("clip-1");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    return spec;
}

bool writeText(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

QString writeManifest(const QTemporaryDir &dir, const QJsonObject &manifest)
{
    const QString path = dir.filePath(QStringLiteral("models.manifest.json"));
    if (!writeText(path, QJsonDocument(manifest).toJson(QJsonDocument::Compact)))
        return QString();
    return path;
}

QJsonObject fileEntry(const QString &path, const QString &hash)
{
    return QJsonObject{{QStringLiteral("model"),
                        QJsonObject{{QStringLiteral("path"), path},
                                    {QStringLiteral("sha256"), hash}}}};
}

QJsonObject modelEntry(const QString &id, const QString &family,
                       bool approved, const QJsonObject &files)
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("family"), family},
                       {QStringLiteral("version"), QStringLiteral("1.0.0")},
                       {QStringLiteral("commercialApproved"), approved},
                       {QStringLiteral("files"), files}};
}

QJsonObject manifestEntry(bool approved, const QJsonArray &models)
{
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("commercialApproved"), approved},
        {QStringLiteral("legalSignOff"),
         QJsonObject{{QStringLiteral("approvedBy"),
                      approved ? QStringLiteral("Legal") : QString()},
                     {QStringLiteral("date"),
                      approved ? QStringLiteral("2026-11-15") : QString()},
                     {QStringLiteral("scope"),
                      approved ? QStringLiteral("internal") : QString()}}},
        {QStringLiteral("models"), models}};
}

} // namespace

TEST(AiProtocol, RoundTripsJobSpec)
{
    AiJobSpec spec;
    spec.jobId = QStringLiteral("job-1");
    spec.clipId = QStringLiteral("clip-1");
    spec.assetPath = QStringLiteral("C:/视频/示例.mp4");
    spec.sidecarRoot = QStringLiteral("C:/工程");
    spec.cacheDir = QStringLiteral("C:/缓存");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    spec.modelsManifestPath = QStringLiteral("C:/models/models.manifest.json");
    spec.trackingModelId = QStringLiteral("tracking-1");
    spec.inpaintModelId = QStringLiteral("inpaint-1");
    spec.sourceInUs = 1'000'000;
    spec.sourceOutUs = 9'000'000;
    spec.width = 1280;
    spec.height = 720;
    spec.fps = Rational{30, 1};
    spec.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('b')), 4, 1});
    spec.parameters.mode = RepairMode::Fast;
    spec.parameters.temporalStabilization = false;
    spec.parameters.stabilizationStrength = 70;
    spec.parameters.processingWidth = 1280;
    spec.parameters.processingHeight = 720;
    spec.redoFirstFrame = 3;
    spec.redoLastFrame = 8;
    spec.resume = true;
    spec.trackingOnly = true;

    QString error;
    const auto restored = aiJobSpecFromJson(aiJobSpecToJson(spec), &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(restored->assetPath, spec.assetPath);
    EXPECT_EQ(restored->parameters.mode, RepairMode::Fast);
    EXPECT_EQ(restored->sourceInUs, spec.sourceInUs);
    EXPECT_EQ(restored->sourceOutUs, spec.sourceOutUs);
    EXPECT_EQ(restored->width, 1280);
    EXPECT_EQ(restored->fps, (Rational{30, 1}));
    EXPECT_TRUE(restored->trackingOnly);
    EXPECT_TRUE(restored->resume);
    EXPECT_EQ(restored->redoFirstFrame, 3);
    EXPECT_EQ(restored->redoLastFrame, 8);
    ASSERT_EQ(restored->maskKeyframes.size(), 1);
    EXPECT_EQ(restored->maskKeyframes.at(0).id, QStringLiteral("mk-1"));
    EXPECT_EQ(restored->maskKeyframes.at(0).featherPx, 4);
}

TEST(AiProtocol, RejectsMalformedAndOversizedLines)
{
    QJsonObject object;
    QString error;
    EXPECT_FALSE(AiProtocol::parseLine("{not json", &object, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(AiProtocol::parseLine(QByteArray(2 * 1024 * 1024, 'x'),
                                       &object, &error));
    EXPECT_FALSE(AiProtocol::parseLine("\n", &object, &error));
    EXPECT_FALSE(AiProtocol::parseLine("[]", &object, &error));
}

TEST(AiProtocol, RequiresEventOrCmdString)
{
    QJsonObject object;
    QString error;
    EXPECT_FALSE(AiProtocol::parseLine("{\"foo\":1}", &object, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(AiProtocol::parseLine("{\"event\":1}", &object, &error));
    EXPECT_TRUE(AiProtocol::parseLine("{\"cmd\":\"pause\"}", &object, &error))
        << error.toStdString();
    EXPECT_TRUE(AiProtocol::parseLine("{\"event\":\"done\"}", &object, &error))
        << error.toStdString();
}

TEST(AiProtocol, AcceptsExactlyMaxLineAndRejectsOneByteMore)
{
    const QByteArray head = "{\"event\":\"";
    const QByteArray tail = "\"}";
    const int padding = AiProtocol::kMaxLineBytes - head.size() - tail.size();
    ASSERT_GT(padding, 0);
    QByteArray line = head + QByteArray(padding, 'x') + tail;
    ASSERT_EQ(line.size(), AiProtocol::kMaxLineBytes);

    QJsonObject object;
    QString error;
    EXPECT_TRUE(AiProtocol::parseLine(line, &object, &error))
        << error.toStdString();

    QByteArray oversized = line;
    oversized.append(' ');
    EXPECT_FALSE(AiProtocol::parseLine(oversized, &object, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(AiProtocol, EncodeLineAppendsNewline)
{
    const QByteArray line = AiProtocol::encodeLine(
        QJsonObject{{QStringLiteral("event"), QStringLiteral("done")}});
    EXPECT_TRUE(line.endsWith('\n'));
    EXPECT_EQ(line.count('\n'), 1);
}

TEST(AiFeatureGate, FailsClosedWithoutLegalApproval)
{
    QTemporaryDir dir;
    const QString manifestPath = writeManifest(
        dir, manifestEntry(false, QJsonArray{}));
    ASSERT_FALSE(manifestPath.isEmpty());

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    QString reason;
    EXPECT_FALSE(registry->featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证")));
}

TEST(AiFeatureGate, ShippedManifestParsesButStaysDisabled)
{
    const QString path =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("../../assets/models/models.manifest.json"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "shipped manifest is missing";

    QString error;
    const auto registry = ModelRegistry::load(path, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    QString reason;
    EXPECT_FALSE(registry->featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证"))) << reason.toStdString();
    EXPECT_NE(registry->defaultTrackingModel(), nullptr);
    EXPECT_NE(registry->defaultInpaintModel(QStringLiteral("quality")), nullptr);
}

TEST(AiFeatureGate, ReloadFailsClosedWithoutApproval)
{
    QTemporaryDir dir;
    const QString manifestPath = writeManifest(
        dir, manifestEntry(false, QJsonArray{}));
    ASSERT_FALSE(manifestPath.isEmpty());

    AiFeatureGate gate;
    QString error;
    EXPECT_FALSE(gate.reload(dir.path(), &error));
    EXPECT_FALSE(gate.enabled());
    EXPECT_TRUE(gate.disabledReason().contains(QStringLiteral("许可证")));
    ASSERT_NE(gate.registry(), nullptr);
}

TEST(AiFeatureGate, ApprovedManifestWithMissingFilesFailsVerification)
{
    QTemporaryDir dir;
    const QJsonArray models{
        modelEntry(QStringLiteral("track-1"), QStringLiteral("tracking"), true,
                   fileEntry(QStringLiteral("missing/tracking.onnx"),
                             QString(64, QLatin1Char('a')))),
        modelEntry(QStringLiteral("inpaint-1"),
                   QStringLiteral("inpainting"), true,
                   fileEntry(QStringLiteral("missing/inpaint.onnx"),
                             QString(64, QLatin1Char('b'))))};
    const QString manifestPath = writeManifest(dir, manifestEntry(true, models));
    ASSERT_FALSE(manifestPath.isEmpty());

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    QString reason;
    EXPECT_FALSE(registry->featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("模型文件缺失或校验失败")))
        << reason.toStdString();

    QString verifyError;
    EXPECT_FALSE(registry->verifyFiles(&verifyError));
    EXPECT_FALSE(verifyError.isEmpty());
}

TEST(AiFeatureGate, ForTestSkipsHashingButKeepsApprovalChecks)
{
    ModelManifest manifest;
    manifest.schemaVersion = 1;
    manifest.commercialApproved = true;
    manifest.legalSignOffBy = QStringLiteral("Legal");
    manifest.legalSignOffDate = QStringLiteral("2026-11-15");
    manifest.legalScope = QStringLiteral("internal");

    AiModelRecord tracking;
    tracking.id = QStringLiteral("track-1");
    tracking.family = QStringLiteral("tracking");
    tracking.commercialApproved = true;
    AiModelRecord inpaint;
    inpaint.id = QStringLiteral("inpaint-1");
    inpaint.family = QStringLiteral("inpainting");
    inpaint.commercialApproved = true;
    manifest.models = {tracking, inpaint};

    ModelRegistry registry = ModelRegistry::forTest(manifest);
    QString reason;
    EXPECT_TRUE(registry.featureEnabled(&reason)) << reason.toStdString();
    EXPECT_TRUE(registry.verifyFiles(nullptr));
    ASSERT_NE(registry.defaultTrackingModel(), nullptr);
    EXPECT_EQ(registry.defaultTrackingModel()->id, QStringLiteral("track-1"));
    ASSERT_NE(registry.defaultInpaintModel(QStringLiteral("fast")), nullptr);
    EXPECT_EQ(registry.defaultInpaintModel(QStringLiteral("fast"))->id,
              QStringLiteral("inpaint-1"));

    ModelManifest unapproved = manifest;
    unapproved.commercialApproved = false;
    ModelRegistry closed = ModelRegistry::forTest(unapproved);
    EXPECT_FALSE(closed.featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证")));
}

TEST(ModelRegistry, RejectsTraversalPath)
{
    QTemporaryDir dir;
    const QJsonArray models{
        modelEntry(QStringLiteral("track-1"), QStringLiteral("tracking"), false,
                   fileEntry(QStringLiteral("../evil.onnx"),
                             QString(64, QLatin1Char('a'))))};
    const QString manifestPath = writeManifest(dir, manifestEntry(false, models));
    ASSERT_FALSE(manifestPath.isEmpty());

    QString error;
    EXPECT_FALSE(ModelRegistry::load(manifestPath, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ModelRegistry, RejectsDuplicateIdsAndUnknownRoles)
{
    QTemporaryDir dir;
    const QJsonArray duplicate{
        modelEntry(QStringLiteral("same"), QStringLiteral("tracking"), false,
                   fileEntry(QStringLiteral("a.onnx"),
                             QString(64, QLatin1Char('a')))),
        modelEntry(QStringLiteral("same"), QStringLiteral("inpainting"), false,
                   fileEntry(QStringLiteral("b.onnx"),
                             QString(64, QLatin1Char('b'))))};
    QString error;
    const QString duplicatePath =
        writeManifest(dir, manifestEntry(false, duplicate));
    ASSERT_FALSE(duplicatePath.isEmpty());
    EXPECT_FALSE(ModelRegistry::load(duplicatePath, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    const QJsonArray unknown{
        modelEntry(QStringLiteral("mystery"), QStringLiteral("segmentation"),
                   false,
                   fileEntry(QStringLiteral("c.onnx"),
                             QString(64, QLatin1Char('c'))))};
    const QString unknownPath = writeManifest(dir, manifestEntry(false, unknown));
    ASSERT_FALSE(unknownPath.isEmpty());
    error.clear();
    EXPECT_FALSE(ModelRegistry::load(unknownPath, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ModelRegistry, RejectsMalformedManifest)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("models.manifest.json"));
    ASSERT_TRUE(writeText(path, "{not json"));
    QString error;
    EXPECT_FALSE(ModelRegistry::load(path, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    ASSERT_TRUE(writeText(
        path, "{\"schemaVersion\":2,\"commercialApproved\":true,"
              "\"models\":[]}"));
    error.clear();
    EXPECT_FALSE(ModelRegistry::load(path, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(AiJobManager, RunsProgressAndDoneWithFakeTransport)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new FakeTransport);
    });

    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QSignalSpy progress(&manager, &AiJobManager::progressChanged);
    const AiJobSpec spec = makeSpec();
    QString error;
    ASSERT_TRUE(manager.start(spec, &error)) << error.toStdString();
    EXPECT_TRUE(manager.running());

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.cacheKey, spec.cacheKey);
    EXPECT_FALSE(manager.running());
    EXPECT_GE(progress.count(), 1);
}

TEST(AiJobManager, RejectsDuplicateAndGateClosedJobs)
{
    AiFeatureGate disabledGate;
    AiJobManager closedManager(disabledGate);
    QString error;
    EXPECT_FALSE(closedManager.start(makeSpec(), &error));
    EXPECT_FALSE(error.isEmpty());

    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new FakeTransport);
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    ASSERT_TRUE(manager.start(makeSpec(), &error)) << error.toStdString();
    error.clear();
    EXPECT_FALSE(manager.start(makeSpec(QStringLiteral("job-2")), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("已有")));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
}

TEST(AiJobManager, RejectsInvalidCacheKey)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    AiJobSpec spec = makeSpec();
    spec.cacheKey = QString(64, QLatin1Char('A')); // uppercase is invalid
    QString error;
    EXPECT_FALSE(manager.start(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("参数无效")));

    spec.cacheKey = QString(63, QLatin1Char('a'));
    error.clear();
    EXPECT_FALSE(manager.start(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("参数无效")));
}

TEST(AiJobManager, ErrorThenDoneStillFails)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(
            new FakeTransport(FakeTransport::Mode::ErrorThenDone));
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(makeSpec(), &error)) << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.errorCode, QStringLiteral("model_unavailable"));
    EXPECT_EQ(result.errorMessage, QStringLiteral("AI 模型尚未安装。"));
}

TEST(AiJobManager, CancelYieldsCancelledResult)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(
            new FakeTransport(FakeTransport::Mode::Silent));
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(makeSpec(), &error)) << error.toStdString();
    manager.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
}

TEST(AiJobManager, CrashExitYieldsCrashError)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    FakeTransport *raw = nullptr;
    manager.setTransportFactoryForTest([&raw] {
        auto transport =
            std::make_unique<FakeTransport>(FakeTransport::Mode::Silent);
        raw = transport.get();
        return std::unique_ptr<AiTransport>(transport.release());
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(makeSpec(), &error)) << error.toStdString();
    ASSERT_NE(raw, nullptr);
    raw->emitFinished(1, QProcess::CrashExit);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("crash"));
    EXPECT_TRUE(result.errorMessage.contains(QStringLiteral("已保留")));
}

TEST(AiJobManager, IgnoresMessagesFromStaleJobs)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    FakeTransport *raw = nullptr;
    manager.setTransportFactoryForTest([&raw] {
        auto transport =
            std::make_unique<FakeTransport>(FakeTransport::Mode::Silent);
        raw = transport.get();
        return std::unique_ptr<AiTransport>(transport.release());
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    const AiJobSpec spec = makeSpec();
    QString error;
    ASSERT_TRUE(manager.start(spec, &error)) << error.toStdString();
    ASSERT_NE(raw, nullptr);

    raw->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("jobId"), QStringLiteral("stale-job")},
        {QStringLiteral("cacheKey"), spec.cacheKey}});
    EXPECT_EQ(finished.count(), 0);
    EXPECT_TRUE(manager.running());

    raw->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("jobId"), spec.jobId},
        {QStringLiteral("cacheKey"), spec.cacheKey}});
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    EXPECT_TRUE(qvariant_cast<AiJobResult>(finished.at(0).at(0)).ok);
}

TEST(AiJobManager, PauseAndResumeSendCommands)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    FakeTransport *raw = nullptr;
    manager.setTransportFactoryForTest([&raw] {
        auto transport =
            std::make_unique<FakeTransport>(FakeTransport::Mode::Silent);
        raw = transport.get();
        return std::unique_ptr<AiTransport>(transport.release());
    });
    QString error;
    ASSERT_TRUE(manager.start(makeSpec(), &error)) << error.toStdString();
    ASSERT_NE(raw, nullptr);

    manager.pause();
    ASSERT_FALSE(raw->commands.isEmpty());
    EXPECT_EQ(raw->commands.last().value(QStringLiteral("cmd")).toString(),
              QStringLiteral("pause"));
    EXPECT_TRUE(manager.paused());

    manager.resume();
    EXPECT_EQ(raw->commands.last().value(QStringLiteral("cmd")).toString(),
              QStringLiteral("resume"));
    EXPECT_FALSE(manager.paused());

    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    manager.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
}

TEST(AiJobManager, StartFailureLeavesManagerIdle)
{
    // An enabled gate but a transport that refuses to start.
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

    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new RejectingTransport);
    });
    QString error;
    EXPECT_FALSE(manager.start(makeSpec(), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(manager.running());
}

TEST(LocalAiTransport, StubEngineReportsModelUnavailable)
{
    const QString engine = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("VisionAiEngine.exe"));
    if (!QFileInfo::exists(engine))
        GTEST_SKIP() << "VisionAiEngine.exe is not built next to app_tests";

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    AiJobSpec spec = makeSpec();
    spec.width = 64;
    spec.height = 64;
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 15000);
    bool sawError = false;
    bool sawDone = false;
    for (const QList<QVariant> &arguments : messages) {
        const QJsonObject message = arguments.at(0).toJsonObject();
        const QString event = message.value(QStringLiteral("event")).toString();
        if (event == QLatin1String("error")) {
            sawError = true;
            EXPECT_EQ(message.value(QStringLiteral("code")).toString(),
                      QStringLiteral("model_unavailable"));
        } else if (event == QLatin1String("done")) {
            sawDone = true;
            EXPECT_EQ(message.value(QStringLiteral("cacheKey")).toString(),
                      spec.cacheKey);
        }
    }
    EXPECT_TRUE(sawError);
    EXPECT_TRUE(sawDone);
}

TEST(RemovalControllerAi, TrackAndGenerateWithoutManagerReportError)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    EXPECT_FALSE(controller.track());
    EXPECT_EQ(controller.error(), QStringLiteral("AI 任务管理器未初始化。"));
    EXPECT_FALSE(controller.generate());
    EXPECT_EQ(controller.error(), QStringLiteral("AI 任务管理器未初始化。"));
}

TEST(AppControllerAi, ExposesJobManager)
{
    QTemporaryDir dir;
    const auto sinkFactory = [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
    AppController controller(nullptr, dir.filePath(QStringLiteral("recovery.json")),
                             sinkFactory, dir.path());
    ASSERT_NE(controller.aiJobs(), nullptr);
    EXPECT_FALSE(controller.aiJobs()->running());
    EXPECT_EQ(controller.removal()->jobManager(), controller.aiJobs());
}
