#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <ostream>
#include <string>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
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
    spec.assetPath = QStringLiteral("C:/媒体/示例.mp4");
    spec.sidecarRoot = QStringLiteral("C:/工程/removal-sidecars");
    spec.cacheDir = QStringLiteral("C:/缓存/removal/")
                    + QString(64, QLatin1Char('a'));
    spec.cacheKey = QString(64, QLatin1Char('a'));
    spec.modelsManifestPath = QStringLiteral("C:/models/models.manifest.json");
    spec.trackingModelId = QStringLiteral("track-1");
    spec.inpaintModelId = QStringLiteral("inpaint-1");
    spec.sourceInUs = 1'000'000;
    spec.sourceOutUs = 9'000'000;
    spec.width = 1920;
    spec.height = 1080;
    spec.fps = Rational{30, 1};
    spec.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 1'000'000, QString(64, QLatin1Char('b')), 4,
        1});
    return spec;
}

bool writeText(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
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

AiModelRecord modelRecord(const QString &id, const QString &family,
                          bool approved)
{
    AiModelRecord record;
    record.id = id;
    record.family = family;
    record.commercialApproved = approved;
    return record;
}

ModelManifest approvedManifest()
{
    ModelManifest manifest;
    manifest.schemaVersion = 1;
    manifest.commercialApproved = true;
    manifest.legalSignOffBy = QStringLiteral("Legal");
    manifest.legalSignOffDate = QStringLiteral("2026-11-15");
    manifest.legalScope = QStringLiteral("internal");
    return manifest;
}

QString fakeEnginePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("fake_ai_engine.exe"));
}

QString visionEnginePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("VisionAiEngine.exe"));
}

// Sets VIDEOEDITOR_AI_ENGINE for the lifetime of the guard.
class EnginePathOverride {
public:
    explicit EnginePathOverride(const QString &path)
        : m_previous(qgetenv("VIDEOEDITOR_AI_ENGINE"))
    {
        qputenv("VIDEOEDITOR_AI_ENGINE", path.toUtf8());
    }
    ~EnginePathOverride()
    {
        if (m_previous.isNull())
            qunsetenv("VIDEOEDITOR_AI_ENGINE");
        else
            qputenv("VIDEOEDITOR_AI_ENGINE", m_previous);
    }

private:
    QByteArray m_previous;
};

bool sawEvent(const QSignalSpy &spy, const QString &event, QString *code = nullptr)
{
    for (const QList<QVariant> &arguments : spy) {
        const QJsonObject message = arguments.at(0).toJsonObject();
        if (message.value(QStringLiteral("event")).toString() == event) {
            if (code)
                *code = message.value(QStringLiteral("code")).toString();
            return true;
        }
    }
    return false;
}

} // namespace

struct InvalidSpecCase {
    const char *name;
    std::function<void(AiJobSpec &)> mutate;
};

void PrintTo(const InvalidSpecCase &value, std::ostream *os)
{
    *os << value.name;
}

class InvalidJobSpecTest : public testing::TestWithParam<InvalidSpecCase> {};

TEST_P(InvalidJobSpecTest, RejectsInvalidSpec)
{
    AiJobSpec spec = makeSpec();
    GetParam().mutate(spec);
    QString error;
    EXPECT_FALSE(AiProtocol::validateJobSpec(spec, &error));
    EXPECT_TRUE(error.startsWith(QStringLiteral("AI 任务参数无效：")))
        << error.toStdString();
    EXPECT_FALSE(error.trimmed().isEmpty());
}

INSTANTIATE_TEST_SUITE_P(
    AiProtocol, InvalidJobSpecTest,
    testing::Values(
        InvalidSpecCase{"emptyJobId",
                        [](AiJobSpec &s) { s.jobId.clear(); }},
        InvalidSpecCase{"emptyAssetPath",
                        [](AiJobSpec &s) { s.assetPath.clear(); }},
        InvalidSpecCase{"emptySidecarRoot",
                        [](AiJobSpec &s) { s.sidecarRoot.clear(); }},
        InvalidSpecCase{"emptyCacheDir",
                        [](AiJobSpec &s) { s.cacheDir.clear(); }},
        InvalidSpecCase{"invalidCacheKey",
                        [](AiJobSpec &s) {
                            s.cacheKey = QString(64, QLatin1Char('A'));
                        }},
        InvalidSpecCase{"emptyModelsManifestPath",
                        [](AiJobSpec &s) { s.modelsManifestPath.clear(); }},
        InvalidSpecCase{"emptyTrackingModelId",
                        [](AiJobSpec &s) { s.trackingModelId.clear(); }},
        InvalidSpecCase{"emptyInpaintModelId",
                        [](AiJobSpec &s) { s.inpaintModelId.clear(); }},
        InvalidSpecCase{"negativeSourceIn",
                        [](AiJobSpec &s) { s.sourceInUs = -1; }},
        InvalidSpecCase{"sourceInTooLarge",
                        [](AiJobSpec &s) {
                            s.sourceInUs = 9007199254740994LL;
                        }},
        InvalidSpecCase{"sourceOutTooLarge",
                        [](AiJobSpec &s) {
                            s.sourceOutUs = 9007199254740994LL;
                        }},
        InvalidSpecCase{"emptySourceRange",
                        [](AiJobSpec &s) { s.sourceOutUs = s.sourceInUs; }},
        InvalidSpecCase{"widthZero", [](AiJobSpec &s) { s.width = 0; }},
        InvalidSpecCase{"widthTooLarge", [](AiJobSpec &s) { s.width = 8193; }},
        InvalidSpecCase{"heightZero", [](AiJobSpec &s) { s.height = 0; }},
        InvalidSpecCase{"heightTooLarge",
                        [](AiJobSpec &s) { s.height = 8193; }},
        InvalidSpecCase{"processingWidthZero",
                        [](AiJobSpec &s) {
                            s.parameters.processingWidth = 0;
                        }},
        InvalidSpecCase{"processingWidthTooLarge",
                        [](AiJobSpec &s) {
                            s.parameters.processingWidth = 8193;
                        }},
        InvalidSpecCase{"processingHeightZero",
                        [](AiJobSpec &s) {
                            s.parameters.processingHeight = 0;
                        }},
        InvalidSpecCase{"processingHeightTooLarge",
                        [](AiJobSpec &s) {
                            s.parameters.processingHeight = 8193;
                        }},
        InvalidSpecCase{"stabilizationTooHigh",
                        [](AiJobSpec &s) {
                            s.parameters.stabilizationStrength = 101;
                        }},
        InvalidSpecCase{"stabilizationNegative",
                        [](AiJobSpec &s) {
                            s.parameters.stabilizationStrength = -1;
                        }},
        InvalidSpecCase{"zeroFpsNum", [](AiJobSpec &s) { s.fps.num = 0; }},
        InvalidSpecCase{"zeroFpsDen", [](AiJobSpec &s) { s.fps.den = 0; }},
        InvalidSpecCase{"noMaskKeyframes",
                        [](AiJobSpec &s) { s.maskKeyframes.clear(); }},
        InvalidSpecCase{"emptyKeyframeId",
                        [](AiJobSpec &s) { s.maskKeyframes[0].id.clear(); }},
        InvalidSpecCase{"duplicateKeyframeIds",
                        [](AiJobSpec &s) {
                            MaskKeyframe second = s.maskKeyframes[0];
                            second.sourceTimeUs += 1'000'000;
                            s.maskKeyframes.append(second);
                        }},
        InvalidSpecCase{"nonIncreasingKeyframeTimes",
                        [](AiJobSpec &s) {
                            MaskKeyframe second = s.maskKeyframes[0];
                            second.id = QStringLiteral("mk-2");
                            second.sourceTimeUs = s.maskKeyframes[0].sourceTimeUs;
                            s.maskKeyframes.append(second);
                        }},
        InvalidSpecCase{"badMaskHash",
                        [](AiJobSpec &s) {
                            s.maskKeyframes[0].maskHash =
                                QStringLiteral("not-a-hash");
                        }},
        InvalidSpecCase{"uppercaseMaskHash",
                        [](AiJobSpec &s) {
                            s.maskKeyframes[0].maskHash =
                                QString(64, QLatin1Char('B'));
                        }},
        InvalidSpecCase{"keyframeTimeTooLarge",
                        [](AiJobSpec &s) {
                            s.maskKeyframes[0].sourceTimeUs =
                                9007199254740994LL;
                        }},
        InvalidSpecCase{"featherNegative",
                        [](AiJobSpec &s) {
                            s.maskKeyframes[0].featherPx = -1;
                        }},
        InvalidSpecCase{"featherTooLarge",
                        [](AiJobSpec &s) {
                            s.maskKeyframes[0].featherPx = 65;
                        }},
        InvalidSpecCase{"redoFirstOnly",
                        [](AiJobSpec &s) { s.redoFirstFrame = 3; }},
        InvalidSpecCase{"redoLastOnly",
                        [](AiJobSpec &s) { s.redoLastFrame = 3; }},
        InvalidSpecCase{"redoFirstAfterLast",
                        [](AiJobSpec &s) {
                            s.redoFirstFrame = 5;
                            s.redoLastFrame = 4;
                        }},
        InvalidSpecCase{"redoOutsideFrames",
                        [](AiJobSpec &s) {
                            s.redoFirstFrame = 0;
                            s.redoLastFrame = 100000;
                        }},
        InvalidSpecCase{"negativeRedoFrame",
                        [](AiJobSpec &s) {
                            s.redoFirstFrame = -2;
                            s.redoLastFrame = -2;
                        }}),
    [](const testing::TestParamInfo<InvalidSpecCase> &info) {
        return std::string(info.param.name);
    });

TEST(AiProtocol, AcceptsValidFullJobSpec)
{
    AiJobSpec spec = makeSpec();
    spec.trackingOnly = true; // non-empty keyframes are still valid
    spec.redoFirstFrame = 0;
    spec.redoLastFrame = 10;
    QString error;
    EXPECT_TRUE(AiProtocol::validateJobSpec(spec, &error))
        << error.toStdString();
    EXPECT_TRUE(error.isEmpty());
}

TEST(AiProtocol, AcceptsChunkAlignedRedoAtSegmentEnd)
{
    AiJobSpec spec = makeSpec();
    spec.sourceInUs = 0;
    spec.sourceOutUs = 1'333'333; // 40 frames at 30 fps
    spec.redoFirstFrame = 30;
    spec.redoLastFrame = 59; // chunk-padded end of the last 30-frame chunk
    QString error;
    EXPECT_TRUE(AiProtocol::validateJobSpec(spec, &error))
        << error.toStdString();
}

TEST(AiProtocol, RejectsRedoBeyondChunkPaddedBound)
{
    AiJobSpec spec = makeSpec();
    spec.sourceInUs = 0;
    spec.sourceOutUs = 1'333'333;
    spec.redoFirstFrame = 30;
    spec.redoLastFrame = 60;
    QString error;
    EXPECT_FALSE(AiProtocol::validateJobSpec(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("重做范围")))
        << error.toStdString();
}

TEST(AiProtocol, RejectsInvertedRedoRange)
{
    AiJobSpec spec = makeSpec();
    spec.sourceInUs = 0;
    spec.sourceOutUs = 1'333'333;
    spec.redoFirstFrame = 30;
    spec.redoLastFrame = 29;
    QString error;
    EXPECT_FALSE(AiProtocol::validateJobSpec(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("重做范围")))
        << error.toStdString();
}

TEST(AiProtocol, RoundTripsJobSpec)
{
    AiJobSpec spec = makeSpec();
    spec.assetPath = QStringLiteral("C:/视频/示例.mp4");
    spec.parameters.mode = RepairMode::Fast;
    spec.parameters.temporalStabilization = false;
    spec.parameters.stabilizationStrength = 70;
    spec.parameters.processingWidth = 1280;
    spec.parameters.processingHeight = 720;
    spec.width = 1280;
    spec.height = 720;
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

TEST(AiProtocol, RejectsWrongTypesButAllowsMissingFields)
{
    const QJsonObject empty;
    QString error;
    ASSERT_TRUE(aiJobSpecFromJson(empty, &error).has_value())
        << error.toStdString();

    const QJsonObject valid = aiJobSpecToJson(makeSpec());
    const QList<QPair<const char *, QJsonValue>> wrongTypes = {
        {"jobId", QJsonValue(1)},
        {"sourceInUs", QJsonValue(QStringLiteral("x"))},
        {"width", QJsonValue(QJsonArray())},
        {"fps", QJsonValue(QStringLiteral("30/1"))},
        {"parameters", QJsonValue(QJsonArray())},
        {"maskKeyframes", QJsonValue(QJsonObject())},
        {"resume", QJsonValue(QStringLiteral("yes"))},
        {"trackingOnly", QJsonValue(1)},
    };
    for (const auto &entry : wrongTypes) {
        QJsonObject mutated = valid;
        mutated.insert(QLatin1String(entry.first), entry.second);
        error.clear();
        EXPECT_FALSE(aiJobSpecFromJson(mutated, &error).has_value())
            << entry.first;
        EXPECT_FALSE(error.isEmpty()) << entry.first;
    }

    QJsonObject fractional = valid;
    fractional.insert(QStringLiteral("sourceInUs"), 1.5);
    EXPECT_FALSE(aiJobSpecFromJson(fractional, &error).has_value());
    QJsonObject overflow = valid;
    overflow.insert(QStringLiteral("sourceInUs"), 1e17);
    EXPECT_FALSE(aiJobSpecFromJson(overflow, &error).has_value());

    QJsonObject badKeyframe = valid;
    QJsonArray keyframes = badKeyframe.value(QStringLiteral("maskKeyframes"))
                               .toArray();
    QJsonObject keyframeObject = keyframes.at(0).toObject();
    keyframeObject.insert(QStringLiteral("sourceTimeUs"), QJsonObject());
    keyframes.replace(0, keyframeObject);
    badKeyframe.insert(QStringLiteral("maskKeyframes"), keyframes);
    EXPECT_FALSE(aiJobSpecFromJson(badKeyframe, &error).has_value());
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
    EXPECT_FALSE(AiProtocol::parseLine("{\"event\":1,\"cmd\":\"pause\"}",
                                       &object, &error));
    EXPECT_FALSE(AiProtocol::parseLine("{\"event\":\"done\",\"cmd\":1}",
                                       &object, &error));
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
    EXPECT_EQ(registry->approvedModel(QStringLiteral("sam2.1-hiera-tiny")),
              nullptr);
}

TEST(AiFeatureGate, WhitespaceSignOffIsRejected)
{
    ModelManifest manifest = approvedManifest();
    manifest.legalSignOffBy = QStringLiteral("   ");
    manifest.models = {
        modelRecord(QStringLiteral("track-1"), QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("inpaint-1"),
                    QStringLiteral("inpainting"), true)};

    ModelRegistry registry = ModelRegistry::forTest(manifest);
    QString reason;
    EXPECT_FALSE(registry.featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证"))) << reason.toStdString();
}

TEST(AiFeatureGate, WhitespaceSignOffDateIsRejected)
{
    ModelManifest manifest = approvedManifest();
    manifest.legalSignOffDate = QStringLiteral("\t ");
    manifest.models = {
        modelRecord(QStringLiteral("track-1"), QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("inpaint-1"),
                    QStringLiteral("inpainting"), true)};

    ModelRegistry registry = ModelRegistry::forTest(manifest);
    QString reason;
    EXPECT_FALSE(registry.featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证"))) << reason.toStdString();
}

TEST(AiFeatureGate, WhitespaceSignOffScopeIsRejected)
{
    ModelManifest manifest = approvedManifest();
    manifest.legalScope = QStringLiteral("  ");
    manifest.models = {
        modelRecord(QStringLiteral("track-1"), QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("inpaint-1"),
                    QStringLiteral("inpainting"), true)};

    ModelRegistry registry = ModelRegistry::forTest(manifest);
    QString reason;
    EXPECT_FALSE(registry.featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证"))) << reason.toStdString();
}

TEST(AiFeatureGate, ApprovedSignOffButNoApprovedInpaintIsFileFailure)
{
    ModelManifest manifest = approvedManifest();
    manifest.models = {
        modelRecord(QStringLiteral("track-1"), QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("inpaint-1"),
                    QStringLiteral("inpainting"), false)};

    ModelRegistry registry = ModelRegistry::forTest(manifest);
    QString reason;
    EXPECT_FALSE(registry.featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("模型文件缺失或校验失败")))
        << reason.toStdString();
    EXPECT_TRUE(reason.contains(QStringLiteral("inpaint")))
        << reason.toStdString();
}

TEST(AiFeatureGate, ApprovedSignOffAndBothRolesEnable)
{
    ModelManifest manifest = approvedManifest();
    manifest.models = {
        modelRecord(QStringLiteral("track-1"), QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("inpaint-1"),
                    QStringLiteral("inpainting"), true)};

    ModelRegistry registry = ModelRegistry::forTest(manifest);
    QString reason;
    EXPECT_TRUE(registry.featureEnabled(&reason)) << reason.toStdString();
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
    ModelManifest manifest = approvedManifest();
    manifest.models = {
        modelRecord(QStringLiteral("track-1"), QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("inpaint-1"),
                    QStringLiteral("inpainting"), true)};

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

TEST(ModelRegistry, ApprovedModelRejectsUnapprovedIds)
{
    ModelManifest manifest = approvedManifest();
    manifest.models = {
        modelRecord(QStringLiteral("approved-track"),
                    QStringLiteral("tracking"), true),
        modelRecord(QStringLiteral("unapproved-inpaint"),
                    QStringLiteral("inpainting"), false)};
    ModelRegistry registry = ModelRegistry::forTest(manifest);
    EXPECT_NE(registry.approvedModel(QStringLiteral("approved-track")),
              nullptr);
    EXPECT_EQ(registry.approvedModel(QStringLiteral("unapproved-inpaint")),
              nullptr);
    EXPECT_EQ(registry.approvedModel(QStringLiteral("missing")), nullptr);
}

TEST(ModelRegistry, AcceptsValidRelativePathsAndUppercaseHashes)
{
    QTemporaryDir dir;
    const QByteArray trackingData = "tracking-bytes";
    const QByteArray inpaintData = "inpaint-bytes";
    ASSERT_TRUE(writeText(
        QDir(dir.path()).filePath(QStringLiteral("nested/tracking.onnx")),
        trackingData));
    ASSERT_TRUE(writeText(
        QDir(dir.path()).filePath(QStringLiteral("nested/inpaint.onnx")),
        inpaintData));
    const QJsonArray models{
        modelEntry(QStringLiteral("track-1"), QStringLiteral("tracking"), true,
                   fileEntry(QStringLiteral("nested/tracking.onnx"),
                             sha256Hex(trackingData).toUpper())),
        modelEntry(QStringLiteral("inpaint-1"),
                   QStringLiteral("inpainting"), true,
                   fileEntry(QStringLiteral("nested/inpaint.onnx"),
                             sha256Hex(inpaintData).toUpper()))};
    const QString manifestPath = writeManifest(dir, manifestEntry(true, models));
    ASSERT_FALSE(manifestPath.isEmpty());

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    QString verifyError;
    EXPECT_TRUE(registry->verifyFiles(&verifyError))
        << verifyError.toStdString();
    QString reason;
    EXPECT_TRUE(registry->featureEnabled(&reason)) << reason.toStdString();
}

TEST(ModelRegistry, AcceptsBackslashRelativePath)
{
    QTemporaryDir dir;
    const QByteArray data = "model-bytes";
    ASSERT_TRUE(writeText(
        QDir(dir.path()).filePath(QStringLiteral("nested/model.onnx")), data));
    const QJsonArray models{
        modelEntry(QStringLiteral("track-1"), QStringLiteral("tracking"), true,
                   fileEntry(QStringLiteral("nested\\model.onnx"),
                             sha256Hex(data))),
        modelEntry(QStringLiteral("inpaint-1"),
                   QStringLiteral("inpainting"), true,
                   fileEntry(QStringLiteral("nested/model.onnx"),
                             sha256Hex(data)))};
    const QString manifestPath = writeManifest(dir, manifestEntry(true, models));
    ASSERT_FALSE(manifestPath.isEmpty());

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    EXPECT_TRUE(registry->verifyFiles(&error)) << error.toStdString();
}

TEST(ModelRegistry, ExistingFileHashMismatchFails)
{
    QTemporaryDir dir;
    const QByteArray data = "model-bytes";
    ASSERT_TRUE(writeText(
        QDir(dir.path()).filePath(QStringLiteral("model.onnx")), data));
    const QJsonArray models{
        modelEntry(QStringLiteral("track-1"), QStringLiteral("tracking"), true,
                   fileEntry(QStringLiteral("model.onnx"),
                             QString(64, QLatin1Char('f')))),
        modelEntry(QStringLiteral("inpaint-1"),
                   QStringLiteral("inpainting"), true,
                   fileEntry(QStringLiteral("model.onnx"), sha256Hex(data)))};
    const QString manifestPath = writeManifest(dir, manifestEntry(true, models));
    ASSERT_FALSE(manifestPath.isEmpty());

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    EXPECT_FALSE(registry->verifyFiles(&error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(ModelRegistry, RejectsUnsafePathsAndDeviceNames)
{
    QTemporaryDir dir;
    const QList<QString> paths = {
        QDir(dir.path()).filePath(QStringLiteral("absolute.onnx")),
        QStringLiteral("C:\\temp\\absolute.onnx"),
        QStringLiteral("\\\\server\\share\\model.onnx"),
        QStringLiteral("/absolute/model.onnx"),
        QStringLiteral("../evil.onnx"),
        QStringLiteral("NUL"),
        QStringLiteral("sub/CON.onnx"),
        QStringLiteral("com1.bin"),
    };
    for (const QString &path : paths) {
        const QJsonArray models{
            modelEntry(QStringLiteral("track-1"), QStringLiteral("tracking"),
                       false, fileEntry(path, QString(64, QLatin1Char('a'))))};
        const QString manifestPath =
            writeManifest(dir, manifestEntry(false, models));
        ASSERT_FALSE(manifestPath.isEmpty());
        QString error;
        EXPECT_FALSE(ModelRegistry::load(manifestPath, &error).has_value())
            << path.toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
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

TEST(AiJobManager, RejectsInvalidSpecsWithParameterError)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    AiJobSpec spec = makeSpec();
    spec.cacheKey = QString(64, QLatin1Char('A'));
    QString error;
    EXPECT_FALSE(manager.start(spec, &error));
    EXPECT_TRUE(error.startsWith(QStringLiteral("AI 任务参数无效：")))
        << error.toStdString();

    spec = makeSpec();
    spec.maskKeyframes.clear();
    error.clear();
    EXPECT_FALSE(manager.start(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩关键帧")))
        << error.toStdString();
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

TEST(AiJobManager, CancelledEventCarriesTrackingState)
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
    const QString confidenceHash(64, QLatin1Char('d'));
    raw->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("cancelled")},
        {QStringLiteral("completedFrames"), 4},
        {QStringLiteral("confidenceHash"), confidenceHash},
        {QStringLiteral("trackingState"), QStringLiteral("partial")}});
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.completedFrames, 4);
    EXPECT_EQ(result.confidenceHash, confidenceHash);
    EXPECT_EQ(result.trackingState, RemovalTrackingState::Partial);
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

TEST(AiJobManager, ProtocolErrorWinsOverCrashExit)
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
    raw->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("error")},
        {QStringLiteral("code"), QStringLiteral("protocol")},
        {QStringLiteral("message"), QStringLiteral("消息行超出长度上限。")}});
    raw->emitFinished(1, QProcess::CrashExit);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("protocol"));
    EXPECT_EQ(result.errorMessage, QStringLiteral("消息行超出长度上限。"));
}

TEST(AiJobManager, RejectsMismatchedDoneCacheKey)
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
        {QStringLiteral("cacheKey"), QString(64, QLatin1Char('c'))}});
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("protocol"));
    EXPECT_EQ(result.cacheKey, spec.cacheKey);
}

TEST(AiJobManager, OldTransportCannotAbortChainedJob)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    QList<FakeTransport *> created;
    manager.setTransportFactoryForTest([&created] {
        auto transport =
            std::make_unique<FakeTransport>(FakeTransport::Mode::Silent);
        created.append(transport.get());
        return std::unique_ptr<AiTransport>(transport.release());
    });

    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    const AiJobSpec firstJob = makeSpec(QStringLiteral("job-1"));
    const AiJobSpec secondJob = makeSpec(QStringLiteral("job-2"));
    bool chained = false;
    QObject::connect(&manager, &AiJobManager::jobFinished, &manager,
                     [&](const AiJobResult &result) {
                         if (chained)
                             return;
                         chained = true;
                         EXPECT_TRUE(result.ok);
                         QString startError;
                         EXPECT_TRUE(manager.start(secondJob, &startError))
                             << startError.toStdString();
                     });

    QString error;
    ASSERT_TRUE(manager.start(firstJob, &error)) << error.toStdString();
    ASSERT_EQ(created.size(), 1);
    created[0]->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("jobId"), firstJob.jobId},
        {QStringLiteral("cacheKey"), firstJob.cacheKey}});
    ASSERT_EQ(finished.count(), 1);
    ASSERT_EQ(created.size(), 2);
    EXPECT_TRUE(manager.running());

    // The superseded transport's trailing signals must be ignored.
    created[0]->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("progress")},
        {QStringLiteral("percent"), 90.0}});
    created[0]->emitFinished(0, QProcess::NormalExit);
    EXPECT_TRUE(manager.running());

    created[1]->emitMessage(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("jobId"), secondJob.jobId},
        {QStringLiteral("cacheKey"), secondJob.cacheKey}});
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 2, 2000);
    EXPECT_TRUE(qvariant_cast<AiJobResult>(finished.at(1).at(0)).ok);
}

TEST(AiJobManager, DestructionWhileRunningDoesNotEmitFinished)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    auto manager = std::make_unique<AiJobManager>(gate);
    manager->setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(
            new FakeTransport(FakeTransport::Mode::Silent));
    });
    QSignalSpy finished(manager.get(), &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager->start(makeSpec(), &error)) << error.toStdString();
    manager.reset();
    EXPECT_EQ(finished.count(), 0);
}

TEST(AiJobManager, CanStartAgainAfterFinish)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(
            new FakeTransport(FakeTransport::Mode::ProgressDone));
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(makeSpec(QStringLiteral("job-1")), &error))
        << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    ASSERT_TRUE(manager.start(makeSpec(QStringLiteral("job-2")), &error))
        << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 2, 2000);
    EXPECT_TRUE(qvariant_cast<AiJobResult>(finished.at(0).at(0)).ok);
    EXPECT_TRUE(qvariant_cast<AiJobResult>(finished.at(1).at(0)).ok);
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

TEST(LocalAiTransport, MissingEngineFailsClosed)
{
    EnginePathOverride override(
        QStringLiteral("C:/definitely-missing/VisionAiEngine.exe"));
    LocalAiTransport transport;
    QString error;
    EXPECT_FALSE(transport.start(makeSpec(), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("AI 推理引擎")))
        << error.toStdString();
}

TEST(LocalAiTransport, StubEngineReportsModelUnavailable)
{
    if (!QFileInfo::exists(visionEnginePath()))
        GTEST_SKIP() << "VisionAiEngine.exe is not built next to app_tests";
    EnginePathOverride override(visionEnginePath());

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    QString error;
    ASSERT_TRUE(transport.start(makeSpec(), &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 15000);
    QString code;
    EXPECT_TRUE(sawEvent(messages, QStringLiteral("error"), &code));
    EXPECT_EQ(code, QStringLiteral("model_unavailable"));
    EXPECT_TRUE(sawEvent(messages, QStringLiteral("done")));
}

TEST(LocalAiTransport, OversizedLineOverSocketBecomesProtocolError)
{
    if (!QFileInfo::exists(fakeEnginePath()))
        GTEST_SKIP() << "fake_ai_engine.exe is not built next to app_tests";
    EnginePathOverride override(fakeEnginePath());

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    AiJobSpec spec = makeSpec();
    spec.assetPath = QStringLiteral("C:/oversized/asset.mp4");
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 15000);
    QString code;
    EXPECT_TRUE(sawEvent(messages, QStringLiteral("error"), &code));
    EXPECT_EQ(code, QStringLiteral("protocol"));
}

TEST(LocalAiTransport, InvalidLineOverSocketBecomesProtocolError)
{
    if (!QFileInfo::exists(fakeEnginePath()))
        GTEST_SKIP() << "fake_ai_engine.exe is not built next to app_tests";
    EnginePathOverride override(fakeEnginePath());

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    AiJobSpec spec = makeSpec();
    spec.assetPath = QStringLiteral("C:/invalid/asset.mp4");
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 15000);
    QString code;
    EXPECT_TRUE(sawEvent(messages, QStringLiteral("error"), &code));
    EXPECT_EQ(code, QStringLiteral("protocol"));
}

TEST(LocalAiTransport, TrailingPartialLineIsDropped)
{
    if (!QFileInfo::exists(fakeEnginePath()))
        GTEST_SKIP() << "fake_ai_engine.exe is not built next to app_tests";
    EnginePathOverride override(fakeEnginePath());

    LocalAiTransport transport;
    QSignalSpy messages(&transport, &AiTransport::message);
    QSignalSpy finished(&transport, &AiTransport::finished);
    AiJobSpec spec = makeSpec();
    spec.assetPath = QStringLiteral("C:/partial/asset.mp4");
    QString error;
    ASSERT_TRUE(transport.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 15000);
    EXPECT_EQ(messages.count(), 0);
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
