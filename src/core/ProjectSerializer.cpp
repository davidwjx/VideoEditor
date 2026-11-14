#include "ProjectSerializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QJsonObject rationalToJson(const Rational &value)
{
    return {{QStringLiteral("num"), value.num}, {QStringLiteral("den"), value.den}};
}

Rational rationalFromJson(const QJsonObject &json)
{
    return Rational{json.value(QStringLiteral("num")).toInt(0),
                    json.value(QStringLiteral("den")).toInt(1)};
}

QJsonObject infoToJson(const MediaInfo &info)
{
    return {
        {QStringLiteral("path"), info.path},
        {QStringLiteral("container"), info.container},
        {QStringLiteral("videoCodec"), info.videoCodec},
        {QStringLiteral("audioCodec"), info.audioCodec},
        {QStringLiteral("width"), info.width},
        {QStringLiteral("height"), info.height},
        {QStringLiteral("frameRate"), rationalToJson(info.frameRate)},
        {QStringLiteral("durationUs"), static_cast<double>(info.durationUs)},
        {QStringLiteral("rotationDegrees"), info.rotationDegrees},
        {QStringLiteral("hasAudio"), info.hasAudio},
        {QStringLiteral("supported"), info.supported},
        {QStringLiteral("unsupportedReason"), info.unsupportedReason},
    };
}

MediaInfo infoFromJson(const QJsonObject &json)
{
    MediaInfo info;
    info.path = json.value(QStringLiteral("path")).toString();
    info.container = json.value(QStringLiteral("container")).toString();
    info.videoCodec = json.value(QStringLiteral("videoCodec")).toString();
    info.audioCodec = json.value(QStringLiteral("audioCodec")).toString();
    info.width = json.value(QStringLiteral("width")).toInt();
    info.height = json.value(QStringLiteral("height")).toInt();
    info.frameRate = rationalFromJson(
        json.value(QStringLiteral("frameRate")).toObject());
    info.durationUs = static_cast<int64_t>(
        json.value(QStringLiteral("durationUs")).toDouble());
    info.rotationDegrees = json.value(QStringLiteral("rotationDegrees")).toInt();
    info.hasAudio = json.value(QStringLiteral("hasAudio")).toBool();
    info.supported = json.value(QStringLiteral("supported")).toBool();
    info.unsupportedReason = json.value(QStringLiteral("unsupportedReason")).toString();
    return info;
}

} // namespace

QJsonObject ProjectSerializer::toJson(const Project &project)
{
    QJsonArray assets;
    for (const Asset &asset : project.assets) {
        assets.append(QJsonObject{
            {QStringLiteral("id"), asset.id},
            {QStringLiteral("path"), asset.path},
            {QStringLiteral("info"), infoToJson(asset.info)},
            {QStringLiteral("proxyPath"), asset.proxyPath},
        });
    }

    return {
        {QStringLiteral("schemaVersion"), Project::kSchemaVersion},
        {QStringLiteral("name"), project.name},
        {QStringLiteral("assets"), assets},
    };
}

std::optional<Project> ProjectSerializer::fromJson(const QJsonObject &json,
                                                   QString *error)
{
    const int version = json.value(QStringLiteral("schemaVersion")).toInt(0);
    if (version <= 0) {
        setError(error, QStringLiteral("无效的工程文件：缺少 schemaVersion。"));
        return std::nullopt;
    }
    if (version > Project::kSchemaVersion) {
        setError(error, QStringLiteral("工程文件版本过新（%1），请升级应用。").arg(version));
        return std::nullopt;
    }

    Project project;
    project.name = json.value(QStringLiteral("name"))
                       .toString(QStringLiteral("未命名工程"));

    for (const QJsonValue &value : json.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject object = value.toObject();
        Asset asset;
        asset.id = object.value(QStringLiteral("id")).toString();
        asset.path = object.value(QStringLiteral("path")).toString();
        asset.info = infoFromJson(object.value(QStringLiteral("info")).toObject());
        asset.proxyPath = object.value(QStringLiteral("proxyPath")).toString();
        if (asset.id.isEmpty() || asset.path.isEmpty()) {
            setError(error, QStringLiteral("工程文件包含无效素材条目。"));
            return std::nullopt;
        }
        project.assets.append(asset);
    }

    return project;
}

bool ProjectSerializer::save(const Project &project, const QString &path,
                             QString *error)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(error, QStringLiteral("无法创建目录：%1").arg(info.absolutePath()));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("无法写入工程文件：%1").arg(file.errorString()));
        return false;
    }
    file.write(QJsonDocument(toJson(project)).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, QStringLiteral("保存工程失败：%1").arg(file.errorString()));
        return false;
    }
    return true;
}

std::optional<Project> ProjectSerializer::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("无法打开工程文件：%1").arg(file.errorString()));
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("工程文件不是有效的 JSON：%1")
                            .arg(parseError.errorString()));
        return std::nullopt;
    }

    return fromJson(document.object(), error);
}

} // namespace VideoEditor
