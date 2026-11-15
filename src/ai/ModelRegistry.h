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

    // Hashes every model file; used by the feature gate.
    bool verifyFiles(QString *error = nullptr) const;
    // Hashes only the requested model ids; used by the engine so a technical
    // run only needs the files it will actually load.
    bool verifyFiles(const QStringList &ids, QString *error = nullptr) const;
    // verifyModelFiles=false skips hashing the model binaries when the caller
    // already proved them (for example AiFeatureGate::reload()).
    bool featureEnabled(QString *reason = nullptr,
                        bool verifyModelFiles = true) const;
    const AiModelRecord *model(const QString &id) const;
    // Returns the record only when it is commercially approved, so engine
    // callers can never load an unapproved id while the gate is enabled.
    const AiModelRecord *approvedModel(const QString &id) const;
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
