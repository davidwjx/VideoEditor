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

TEST(ModelRegistry, DefaultTrackingModelNeverPicksApprovedFp16)
{
    // Mirror the real manifest: the fp16 bundle is "efficienttam-ti-512"
    // version 0.8.0 and only its file paths contain "_fp16/".
    AiModelRecord approvedFp16 =
        trackingRecord(QStringLiteral("efficienttam-ti-512"), true);
    approvedFp16.version = QStringLiteral("0.8.0");
    approvedFp16.commercialApproved = true;
    approvedFp16.files[QStringLiteral("image_encoder")].path =
        QStringLiteral("efficienttam_ti_512_fp16/image_encoder.onnx");
    approvedFp16.files[QStringLiteral("prompt_encoder")].path =
        QStringLiteral("efficienttam_ti_512_fp16/prompt_encoder.onnx");
    approvedFp16.files[QStringLiteral("mask_decoder")].path =
        QStringLiteral("efficienttam_ti_512_fp16/mask_decoder.onnx");

    AiModelRecord unapprovedFp32 =
        trackingRecord(QStringLiteral("efficienttam-ti-512-fp32"), true);
    unapprovedFp32.commercialApproved = false;

    ModelManifest manifest;
    manifest.schemaVersion = 1;
    manifest.models = {approvedFp16, unapprovedFp32};
    const ModelRegistry registry =
        ModelRegistry::forTest(manifest, QStringLiteral("C:/models"));
    const AiModelRecord *record = registry.defaultTrackingModel();
    ASSERT_NE(record, nullptr);
    // "fp16 must not be used" wins over commercial approval.
    EXPECT_EQ(record->id, QStringLiteral("efficienttam-ti-512-fp32"));

    // With no fp32 candidate the path-detected fp16 record is still skipped;
    // the remaining tracking fallback is returned instead.
    ModelManifest onlyFp16;
    onlyFp16.schemaVersion = 1;
    onlyFp16.models = {approvedFp16,
                       trackingRecord(QStringLiteral("sam2.1-hiera-tiny"),
                                      false)};
    const ModelRegistry fallbackRegistry =
        ModelRegistry::forTest(onlyFp16, QStringLiteral("C:/models"));
    ASSERT_NE(fallbackRegistry.defaultTrackingModel(), nullptr);
    EXPECT_EQ(fallbackRegistry.defaultTrackingModel()->id,
              QStringLiteral("sam2.1-hiera-tiny"));
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
