#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "ModelRegistry.h"

using namespace VideoEditor;

namespace {

AiModelFile fileRecord(const QString &path)
{
    return AiModelFile{path, QString(64, QLatin1Char('0'))};
}

AiModelRecord trackingRecord(const QString &id, bool offlineParts)
{
    AiModelRecord record;
    record.id = id;
    record.family = QStringLiteral("tracking");
    record.version = QStringLiteral("1.0.0");
    if (offlineParts) {
        record.files.insert(QStringLiteral("image_encoder"),
                            fileRecord(QStringLiteral("image_encoder.onnx")));
        record.files.insert(QStringLiteral("prompt_encoder"),
                            fileRecord(QStringLiteral("prompt_encoder.onnx")));
        record.files.insert(QStringLiteral("mask_decoder"),
                            fileRecord(QStringLiteral("mask_decoder.onnx")));
    } else {
        record.files.insert(QStringLiteral("vision_encoder"),
                            fileRecord(QStringLiteral("vision_encoder.onnx")));
        record.files.insert(QStringLiteral("mask_decoder"),
                            fileRecord(QStringLiteral("mask_decoder.onnx")));
    }
    return record;
}

QString sha256(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

} // namespace

TEST(ModelRegistry, DefaultTrackingModelPrefersOfflineCapableFp32)
{
    ModelManifest manifest;
    manifest.schemaVersion = 1;
    manifest.models = {
        trackingRecord(QStringLiteral("sam2.1-hiera-tiny"), false),
        trackingRecord(QStringLiteral("efficienttam-ti-512-fp16"), true),
        trackingRecord(QStringLiteral("efficienttam-ti-512-fp32"), true),
    };

    const ModelRegistry registry =
        ModelRegistry::forTest(manifest, QStringLiteral("C:/models"));
    const AiModelRecord *record = registry.defaultTrackingModel();
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->id, QStringLiteral("efficienttam-ti-512-fp32"));
}

TEST(ModelRegistry, VerifyFilesByIdOnlyHashesRequestedModels)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QByteArray bytesA = QByteArrayLiteral("model-a");
    ASSERT_TRUE(writeFile(temp.filePath(QStringLiteral("a.onnx")), bytesA));

    const auto modelJson = [](const QString &id, const QString &path,
                              const QString &hash) {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("family"), QStringLiteral("tracking")},
            {QStringLiteral("files"),
             QJsonObject{
                 {QStringLiteral("image_encoder"),
                  QJsonObject{{QStringLiteral("path"), path},
                              {QStringLiteral("sha256"), hash}}}}}};
    };
    const QJsonObject manifestJson{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("models"),
         QJsonArray{modelJson(QStringLiteral("a"), QStringLiteral("a.onnx"),
                              sha256(bytesA)),
                    modelJson(QStringLiteral("b"), QStringLiteral("b.onnx"),
                              QString(64, QLatin1Char('c')))}}};
    const QString manifestPath =
        temp.filePath(QStringLiteral("models.manifest.json"));
    ASSERT_TRUE(writeFile(
        manifestPath, QJsonDocument(manifestJson).toJson(QJsonDocument::Compact)));

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();

    EXPECT_TRUE(registry->verifyFiles({QStringLiteral("a")}, &error))
        << error.toStdString();
    // Model b's file is intentionally absent: the full check fails, the
    // per-id check for a still succeeds.
    EXPECT_FALSE(registry->verifyFiles(&error));
    EXPECT_FALSE(registry->verifyFiles({QStringLiteral("b")}, &error));
    EXPECT_FALSE(registry->verifyFiles({QStringLiteral("missing")}, &error));
}
