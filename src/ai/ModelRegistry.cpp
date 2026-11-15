#include "ModelRegistry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

namespace VideoEditor {

namespace {

bool isHex64(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{64}$"));
    return pattern.match(text).hasMatch();
}

// Reject absolute paths, drive letters and any ".." traversal component. The
// path must be a plain relative file below the manifest directory.
bool isSafeRelativePath(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path))
        return false;
    if (path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('\\')))
        return false;
    const QString normalized = QString(path).replace(QLatin1Char('\\'),
                                                    QLatin1Char('/'));
    const QStringList parts = normalized.split(QLatin1Char('/'),
                                               Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    static const QSet<QString> reservedNames = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),
        QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QLatin1String(".")
            || part == QLatin1String(".."))
            return false;
        if (part.contains(QLatin1Char(':')))
            return false;
        // Windows treats CON.txt and CON as the same device.
        const QString base =
            part.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
        if (reservedNames.contains(base))
            return false;
    }
    return true;
}

// Both paths must already be canonical. The candidate must be strictly below
// the root, so a junction/symlink that resolves outside is rejected.
bool isCanonicalUnder(const QString &root, const QString &candidate)
{
    if (root.isEmpty() || candidate.isEmpty())
        return false;
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanCandidate = QDir::cleanPath(candidate);
    if (cleanCandidate.compare(cleanRoot, Qt::CaseInsensitive) == 0)
        return false;
    return cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'),
                                     Qt::CaseInsensitive)
           || cleanCandidate.startsWith(cleanRoot + QLatin1Char('\\'),
                                        Qt::CaseInsensitive);
}

bool parseModel(const QJsonObject &json, AiModelRecord *model, QString *error)
{
    const QString id = json.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        if (error)
            *error = QStringLiteral("模型缺少 id。");
        return false;
    }
    const QString family = json.value(QStringLiteral("family")).toString();
    if (family != QLatin1String("tracking")
        && family != QLatin1String("inpaint")
        && family != QLatin1String("inpainting")) {
        if (error)
            *error = QStringLiteral("模型 %1 的 family 无效：%2。")
                         .arg(id, family);
        return false;
    }
    const QJsonValue filesValue = json.value(QStringLiteral("files"));
    if (!filesValue.isObject()) {
        if (error)
            *error = QStringLiteral("模型 %1 的 files 必须是对象。").arg(id);
        return false;
    }

    model->id = id;
    model->family = family;
    model->version = json.value(QStringLiteral("version")).toString();
    model->codeLicense = json.value(QStringLiteral("codeLicense")).toString();
    model->weightsLicense =
        json.value(QStringLiteral("weightsLicense")).toString();
    for (const QJsonValue &entry : json.value(QStringLiteral("trainingData")).toArray()) {
        if (entry.isString())
            model->trainingData.append(entry.toString());
    }
    model->commercialApproved =
        json.value(QStringLiteral("commercialApproved")).toBool(false);
    model->io = json.value(QStringLiteral("io")).toObject();

    const QJsonObject files = filesValue.toObject();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        if (!it.value().isObject()) {
            if (error)
                *error = QStringLiteral("模型 %1 的文件 %2 格式无效。")
                             .arg(id, it.key());
            return false;
        }
        const QJsonObject file = it.value().toObject();
        AiModelFile record;
        record.path = file.value(QStringLiteral("path")).toString();
        record.sha256 = file.value(QStringLiteral("sha256")).toString();
        if (!isSafeRelativePath(record.path)) {
            if (error)
                *error = QStringLiteral("模型 %1 的文件路径不安全：%2。")
                             .arg(id, record.path);
            return false;
        }
        if (!isHex64(record.sha256)) {
            if (error)
                *error = QStringLiteral("模型 %1 的文件 %2 SHA-256 无效。")
                             .arg(id, it.key());
            return false;
        }
        model->files.insert(it.key(), record);
    }
    return true;
}

} // namespace

bool AiModelRecord::isTracking() const
{
    return family == QLatin1String("tracking");
}

bool AiModelRecord::isInpaint() const
{
    return family == QLatin1String("inpaint")
           || family == QLatin1String("inpainting");
}

std::optional<ModelRegistry> ModelRegistry::load(const QString &manifestPath,
                                                 QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::optional<ModelRegistry>{};
    };

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法读取模型清单：%1。").arg(file.errorString()));

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("模型清单 JSON 解析失败：%1。")
                        .arg(parseError.errorString()));
    if (!document.isObject())
        return fail(QStringLiteral("模型清单必须是 JSON 对象。"));

    const QJsonObject root = document.object();
    const int schemaVersion =
        root.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (schemaVersion != 1)
        return fail(QStringLiteral("模型清单 schemaVersion 不受支持：%1。")
                        .arg(schemaVersion));

    const QJsonValue modelsValue = root.value(QStringLiteral("models"));
    if (!modelsValue.isArray())
        return fail(QStringLiteral("模型清单 models 字段必须是数组。"));

    ModelRegistry registry;
    registry.m_manifestPath = QFileInfo(manifestPath).absoluteFilePath();
    registry.m_rootDir = QFileInfo(manifestPath).absolutePath();
    registry.m_manifest.schemaVersion = schemaVersion;
    registry.m_manifest.pipelineVersion =
        root.value(QStringLiteral("pipelineVersion")).toString();
    registry.m_manifest.commercialApproved =
        root.value(QStringLiteral("commercialApproved")).toBool(false);

    const QJsonObject signOff =
        root.value(QStringLiteral("legalSignOff")).toObject();
    registry.m_manifest.legalSignOffBy =
        signOff.value(QStringLiteral("approvedBy")).toString();
    registry.m_manifest.legalSignOffDate =
        signOff.value(QStringLiteral("date")).toString();
    registry.m_manifest.legalScope =
        signOff.value(QStringLiteral("scope")).toString();

    const QJsonArray models = modelsValue.toArray();
    for (const QJsonValue &value : models) {
        if (!value.isObject())
            return fail(QStringLiteral("模型清单中的模型必须是对象。"));
        AiModelRecord record;
        QString modelError;
        if (!parseModel(value.toObject(), &record, &modelError))
            return fail(modelError);
        for (const AiModelRecord &existing : registry.m_manifest.models) {
            if (existing.id == record.id)
                return fail(QStringLiteral("模型清单包含重复 id：%1。")
                                .arg(record.id));
        }
        registry.m_manifest.models.append(record);
    }

    if (error)
        error->clear();
    return registry;
}

ModelRegistry ModelRegistry::forTest(const ModelManifest &manifest,
                                     const QString &rootDir)
{
    ModelRegistry registry;
    registry.m_manifest = manifest;
    registry.m_rootDir = rootDir;
    registry.m_testMode = true;
    return registry;
}

bool ModelRegistry::verifyFiles(QString *error) const
{
    if (m_testMode)
        return true;

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QString rootCanonical =
        QFileInfo(m_rootDir).canonicalFilePath();
    if (rootCanonical.isEmpty())
        return fail(QStringLiteral("模型根目录无法解析"));

    for (const AiModelRecord &model : m_manifest.models) {
        if (model.files.isEmpty())
            return fail(QStringLiteral("%1 缺少文件清单").arg(model.id));
        for (auto it = model.files.constBegin(); it != model.files.constEnd();
             ++it) {
            const AiModelFile &file = it.value();
            if (!isSafeRelativePath(file.path))
                return fail(QStringLiteral("%1/%2 路径不安全")
                                .arg(model.id, it.key()));
            const QString resolved = QDir(m_rootDir).filePath(file.path);
            // canonicalFilePath() is empty for a missing file and resolves
            // junctions/symlinks; requiring both makes the check fail closed.
            const QString canonical =
                QFileInfo(resolved).canonicalFilePath();
            if (canonical.isEmpty())
                return fail(QStringLiteral("%1/%2 文件缺失或无法解析")
                                .arg(model.id, it.key()));
            if (!isCanonicalUnder(rootCanonical, canonical))
                return fail(QStringLiteral("%1/%2 路径越界")
                                .arg(model.id, it.key()));
            QFile handle(canonical);
            if (!handle.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("%1/%2 文件缺失")
                                .arg(model.id, it.key()));
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&handle))
                return fail(QStringLiteral("%1/%2 无法读取")
                                .arg(model.id, it.key()));
            const QString actual =
                QString::fromLatin1(hash.result().toHex());
            if (actual.compare(file.sha256, Qt::CaseInsensitive) != 0)
                return fail(QStringLiteral("%1/%2 SHA-256 不匹配")
                                .arg(model.id, it.key()));
        }
    }
    if (error)
        error->clear();
    return true;
}

bool ModelRegistry::featureEnabled(QString *reason) const
{
    const auto deny = [reason](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };

    if (m_manifest.schemaVersion != 1)
        return deny(QStringLiteral("AI 消除功能未启用：模型文件缺失或校验失败"
                                   "（schemaVersion）。"));

    if (!m_manifest.commercialApproved
        || m_manifest.legalSignOffBy.trimmed().isEmpty()
        || m_manifest.legalSignOffDate.trimmed().isEmpty()
        || m_manifest.legalScope.trimmed().isEmpty()) {
        return deny(QStringLiteral("AI 消除功能未启用：缺少模型许可证审批。"));
    }

    bool tracking = false;
    bool inpaint = false;
    for (const AiModelRecord &model : m_manifest.models) {
        if (!model.commercialApproved)
            continue;
        if (model.isTracking())
            tracking = true;
        else if (model.isInpaint())
            inpaint = true;
    }

    if (!tracking || !inpaint) {
        QStringList missing;
        if (!tracking)
            missing.append(QStringLiteral("tracking"));
        if (!inpaint)
            missing.append(QStringLiteral("inpaint"));
        return deny(QStringLiteral("AI 消除功能未启用：模型文件缺失或校验失败"
                                   "（%1）。")
                        .arg(missing.join(QLatin1Char('/'))));
    }

    if (!m_testMode) {
        QString fileError;
        if (!verifyFiles(&fileError)) {
            return deny(QStringLiteral("AI 消除功能未启用：模型文件缺失或校验"
                                       "失败（%1）。")
                            .arg(fileError));
        }
    }

    if (reason)
        reason->clear();
    return true;
}

const AiModelRecord *ModelRegistry::model(const QString &id) const
{
    for (const AiModelRecord &record : m_manifest.models) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

const AiModelRecord *ModelRegistry::approvedModel(const QString &id) const
{
    for (const AiModelRecord &record : m_manifest.models) {
        if (record.id == id)
            return record.commercialApproved ? &record : nullptr;
    }
    return nullptr;
}

const AiModelRecord *ModelRegistry::defaultTrackingModel() const
{
    const AiModelRecord *fallback = nullptr;
    for (const AiModelRecord &record : m_manifest.models) {
        if (!record.isTracking())
            continue;
        if (record.commercialApproved)
            return &record;
        if (!fallback)
            fallback = &record;
    }
    return fallback;
}

const AiModelRecord *ModelRegistry::defaultInpaintModel(
    const QString &mode) const
{
    const QString wanted = mode.trimmed().toLower();
    const AiModelRecord *approvedFallback = nullptr;
    const AiModelRecord *fallback = nullptr;
    for (const AiModelRecord &record : m_manifest.models) {
        if (!record.isInpaint())
            continue;
        if (!fallback)
            fallback = &record;
        if (!record.commercialApproved)
            continue;
        if (!approvedFallback)
            approvedFallback = &record;
        if (!wanted.isEmpty()
            && (record.id.toLower().contains(wanted)
                || record.version.toLower().contains(wanted))) {
            return &record;
        }
    }
    return approvedFallback ? approvedFallback : fallback;
}

} // namespace VideoEditor
