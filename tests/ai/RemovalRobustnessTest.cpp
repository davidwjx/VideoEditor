#include <gtest/gtest.h>

#include <memory>

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
#include "MaskSidecarStore.h"
#include "ModelRegistry.h"

using namespace VideoEditor;

namespace {

// Fake transports emit only the signals inherited from AiTransport, so the
// classes need no Q_OBJECT/metaobject of their own (and no .moc include).
class CrashingTransport : public AiTransport {
public:
    bool start(const AiJobSpec &, QString *) override
    {
        QTimer::singleShot(0, this, [this] {
            emit finished(1, QProcess::CrashExit);
        });
        return true;
    }
    void cancel() override {}
};

class DiskFullTransport : public AiTransport {
public:
    bool start(const AiJobSpec &, QString *) override
    {
        QTimer::singleShot(0, this, [this] {
            emit message(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("error")},
                {QStringLiteral("code"), QStringLiteral("disk_full")},
                {QStringLiteral("message"), QStringLiteral("no space")}});
            emit finished(1, QProcess::NormalExit);
        });
        return true;
    }
    void cancel() override {}
};

// A fully valid spec exercises the real validation path; only the transport is
// replaced, so a crash/disk error is handled exactly as in production.
AiJobSpec validSpec(const QString &jobId, const QString &cacheKey)
{
    AiJobSpec spec;
    spec.jobId = jobId;
    spec.clipId = QStringLiteral("clip-1");
    spec.assetPath = QStringLiteral("C:/媒体/示例.mp4");
    spec.sidecarRoot = QStringLiteral("C:/工程/removal-sidecars");
    spec.cacheDir = QStringLiteral("C:/缓存/removal/") + cacheKey;
    spec.cacheKey = cacheKey;
    spec.modelsManifestPath = QStringLiteral("C:/models/models.manifest.json");
    spec.trackingModelId = QStringLiteral("tracking-1");
    spec.inpaintModelId = QStringLiteral("inpaint-1");
    spec.sourceInUs = 0;
    spec.sourceOutUs = 1'000'000;
    spec.width = 640;
    spec.height = 360;
    spec.fps = Rational{30, 1};
    spec.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('b')), 4, 1});
    return spec;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

QJsonObject modelRecord(const QString &id, const QString &family,
                        const QString &path)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("family"), family},
        {QStringLiteral("commercialApproved"), true},
        {QStringLiteral("files"),
         QJsonObject{{QStringLiteral("model"),
                      QJsonObject{{QStringLiteral("path"), path},
                                  {QStringLiteral("sha256"),
                                   QString(64, QLatin1Char('0'))}}}}}};
}

} // namespace

// Error-mapping only: the fake transport never touches the cache. Chunk
// retention on crash/disk-full is covered by the RemovalCache partial-manifest
// and RemovalController redo tests, not by this test.
TEST(RemovalRobustness, CrashReportsFailure)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new CrashingTransport);
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    ASSERT_TRUE(finished.isValid());

    QString error;
    ASSERT_TRUE(manager.start(
        validSpec(QStringLiteral("crash"), QString(64, QLatin1Char('a'))),
        &error))
        << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result = qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.errorCode, QStringLiteral("crash"));
    EXPECT_TRUE(result.errorMessage.contains(
        QStringLiteral("已完成的分段已保留")));
}

TEST(RemovalRobustness, DiskFullReportsError)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new DiskFullTransport);
    });
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    ASSERT_TRUE(finished.isValid());

    QString error;
    ASSERT_TRUE(manager.start(
        validSpec(QStringLiteral("disk"), QString(64, QLatin1Char('c'))),
        &error))
        << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result = qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("disk_full"));
    EXPECT_FALSE(result.errorMessage.isEmpty());
}

TEST(RemovalRobustness, ModelHashMismatchDisablesFeature)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // Relative paths keep the manifest loadable; the deliberately wrong hash
    // must make the feature gate fail closed.
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("tracking.onnx")),
                          "not-a-real-tracking-model"));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("inpaint.onnx")),
                          "not-a-real-inpaint-model"));

    QJsonObject manifest{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("commercialApproved"), true},
        {QStringLiteral("legalSignOff"),
         QJsonObject{{QStringLiteral("approvedBy"), QStringLiteral("legal")},
                     {QStringLiteral("date"), QStringLiteral("2026-11-15")},
                     {QStringLiteral("scope"), QStringLiteral("commercial")}}},
        {QStringLiteral("models"),
         QJsonArray{modelRecord(QStringLiteral("tracking-1"),
                                QStringLiteral("tracking"),
                                QStringLiteral("tracking.onnx")),
                    modelRecord(QStringLiteral("inpaint-1"),
                                QStringLiteral("inpaint"),
                                QStringLiteral("inpaint.onnx"))}}};
    const QString manifestPath =
        dir.filePath(QStringLiteral("models.manifest.json"));
    ASSERT_TRUE(writeFile(manifestPath, QJsonDocument(manifest).toJson()));

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();

    QString reason;
    EXPECT_FALSE(registry->featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("校验失败")))
        << reason.toStdString();
}

// Mask-sidecar round trip only. Full project save-as/relocation under a
// Chinese path is covered by MaskSidecarStore and ProjectStore tests.
TEST(RemovalRobustness, ChineseProjectPathRoundTripsMasks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString projectPath =
        dir.filePath(QStringLiteral("我的 工程.veproj"));
    MaskSidecarStore store(projectPath);
    ASSERT_TRUE(store.ensureLayout());

    BinaryMask mask;
    mask.width = mask.height = 16;
    mask.bits.assign(256, 0);
    mask.set(8, 8, true);
    QString hash;
    QString error;
    ASSERT_TRUE(store.writeMask(mask, &hash, &error))
        << error.toStdString();
    const auto read = store.readMask(hash, &error);
    ASSERT_TRUE(read.has_value()) << error.toStdString();
    EXPECT_EQ(read->bits, mask.bits);
    EXPECT_EQ(read->width, 16);
    EXPECT_EQ(read->height, 16);
}
