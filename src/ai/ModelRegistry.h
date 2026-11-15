// src/ai/ModelRegistry.h
#pragma once

#include <optional>

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace VideoEditor {

struct AiModelFile {
    QString path;
    QString sha256;
};

struct AiModelRecord {
    QString id;
    QString family; // tracking | inpaint | inpainting
    QString version;
    QString codeLicense;
    QString weightsLicense;
    QStringList trainingData;
    bool commercialApproved = false;
    QMap<QString, AiModelFile> files;
    QJsonObject io;

    bool isTracking() const;
    bool isInpaint() const;
};

struct ModelManifest {
    int schemaVersion = 1;
    QString pipelineVersion;
    bool commercialApproved = false;
    QString legalSignOffBy;
    QString legalSignOffDate;
    QString legalScope;
    QList<AiModelRecord> models;
};

// Fail-closed view over assets/models/models.manifest.json. load() validates
// the schema, every model role and every file path (relative, no '..');
// verifyFiles() hashes the model binaries. forTest() skips disk hashing but
// keeps the approval and role checks.
class ModelRegistry {
public:
    static std::optional<ModelRegistry> load(const QString &manifestPath,
                                             QString *error = nullptr);
    static ModelRegistry forTest(const ModelManifest &manifest,
                                 const QString &rootDir = {});

    bool verifyFiles(QString *error = nullptr) const;
    bool featureEnabled(QString *reason = nullptr) const;
    const AiModelRecord *model(const QString &id) const;
    const AiModelRecord *defaultTrackingModel() const;
    const AiModelRecord *defaultInpaintModel(const QString &mode) const;
    QString manifestPath() const { return m_manifestPath; }
    QString rootDir() const { return m_rootDir; }
    const ModelManifest &manifest() const { return m_manifest; }

private:
    ModelRegistry() = default;

    ModelManifest m_manifest;
    QString m_manifestPath;
    QString m_rootDir;
    bool m_testMode = false;
};

} // namespace VideoEditor
