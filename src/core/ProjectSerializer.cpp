#include "ProjectSerializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

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

QString mediaKindToString(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Audio:
        return QStringLiteral("audio");
    case MediaKind::Video:
        return QStringLiteral("video");
    case MediaKind::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

MediaKind mediaKindFromJson(const QJsonObject &json)
{
    // Files written before M2 had no "kind" field and only ever contained
    // video assets, so a missing value means video.
    const QString value = json.value(QStringLiteral("kind"))
                              .toString(QStringLiteral("video"))
                              .trimmed()
                              .toLower();
    if (value == QStringLiteral("audio"))
        return MediaKind::Audio;
    if (value == QStringLiteral("unknown"))
        return MediaKind::Unknown;
    return MediaKind::Video;
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
        {QStringLiteral("kind"), mediaKindToString(info.kind)},
        {QStringLiteral("hasVideo"), info.hasVideo},
        {QStringLiteral("audioSampleRate"), info.audioSampleRate},
        {QStringLiteral("audioChannels"), info.audioChannels},
        {QStringLiteral("audioDurationUs"), static_cast<double>(info.audioDurationUs)},
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
    info.kind = mediaKindFromJson(json);
    info.hasVideo = json.value(QStringLiteral("hasVideo"))
                        .toBool(info.kind == MediaKind::Video);
    info.audioSampleRate = json.value(QStringLiteral("audioSampleRate")).toInt();
    info.audioChannels = json.value(QStringLiteral("audioChannels")).toInt();
    info.audioDurationUs = static_cast<int64_t>(
        json.value(QStringLiteral("audioDurationUs")).toDouble());
    return info;
}

QJsonObject transformToJson(const Transform &value)
{
    return {{QStringLiteral("scaleMilli"), value.scaleMilli},
            {QStringLiteral("offsetXPermille"), value.offsetXPermille},
            {QStringLiteral("offsetYPermille"), value.offsetYPermille},
            {QStringLiteral("opacityMilli"), value.opacityMilli}};
}

Transform transformFromJson(const QJsonObject &json)
{
    Transform value;
    value.scaleMilli = json.value(QStringLiteral("scaleMilli")).toInt(1000);
    value.offsetXPermille = json.value(QStringLiteral("offsetXPermille")).toInt(0);
    value.offsetYPermille = json.value(QStringLiteral("offsetYPermille")).toInt(0);
    value.opacityMilli = json.value(QStringLiteral("opacityMilli")).toInt(1000);
    return value;
}

QJsonObject audioToJson(const AudioParams &value)
{
    QJsonArray keyframes;
    for (const VolumeKeyframe &keyframe : value.keyframes) {
        keyframes.append(QJsonObject{
            {QStringLiteral("timeUs"), static_cast<double>(keyframe.timeUs)},
            {QStringLiteral("volumeMilli"), keyframe.volumeMilli}});
    }
    return {{QStringLiteral("volumeMilli"), value.volumeMilli},
            {QStringLiteral("panMilli"), value.panMilli},
            {QStringLiteral("fadeInUs"), static_cast<double>(value.fadeInUs)},
            {QStringLiteral("fadeOutUs"), static_cast<double>(value.fadeOutUs)},
            {QStringLiteral("muted"), value.muted},
            {QStringLiteral("keyframes"), keyframes}};
}

AudioParams audioFromJson(const QJsonObject &json)
{
    AudioParams value;
    value.volumeMilli = json.value(QStringLiteral("volumeMilli")).toInt(1000);
    value.panMilli = json.value(QStringLiteral("panMilli")).toInt(0);
    value.fadeInUs = static_cast<int64_t>(
        json.value(QStringLiteral("fadeInUs")).toDouble(0));
    value.fadeOutUs = static_cast<int64_t>(
        json.value(QStringLiteral("fadeOutUs")).toDouble(0));
    value.muted = json.value(QStringLiteral("muted")).toBool(false);
    for (const QJsonValue &entry : json.value(QStringLiteral("keyframes")).toArray()) {
        const QJsonObject object = entry.toObject();
        value.keyframes.append(VolumeKeyframe{
            static_cast<int64_t>(object.value(QStringLiteral("timeUs")).toDouble(0)),
            object.value(QStringLiteral("volumeMilli")).toInt(1000)});
    }
    return value;
}

QJsonObject clipToJson(const Clip &clip)
{
    return {{QStringLiteral("id"), clip.id},
            {QStringLiteral("assetId"), clip.assetId},
            {QStringLiteral("linkId"), clip.linkId},
            {QStringLiteral("sourceInUs"), static_cast<double>(clip.sourceInUs)},
            {QStringLiteral("sourceOutUs"), static_cast<double>(clip.sourceOutUs)},
            {QStringLiteral("timelineStartUs"), static_cast<double>(clip.timelineStartUs)},
            {QStringLiteral("speedMilli"), clip.speedMilli},
            {QStringLiteral("preservePitch"), clip.preservePitch},
            {QStringLiteral("enabled"), clip.enabled},
            {QStringLiteral("transform"), transformToJson(clip.transform)},
            {QStringLiteral("audio"), audioToJson(clip.audio)}};
}

Clip clipFromJson(const QJsonObject &json)
{
    Clip clip;
    clip.id = json.value(QStringLiteral("id")).toString();
    clip.assetId = json.value(QStringLiteral("assetId")).toString();
    clip.linkId = json.value(QStringLiteral("linkId")).toString();
    clip.sourceInUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceInUs")).toDouble(0));
    clip.sourceOutUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceOutUs")).toDouble(0));
    clip.timelineStartUs = static_cast<int64_t>(
        json.value(QStringLiteral("timelineStartUs")).toDouble(0));
    clip.speedMilli = json.value(QStringLiteral("speedMilli")).toInt(kDefaultSpeedMilli);
    clip.preservePitch = json.value(QStringLiteral("preservePitch")).toBool(false);
    clip.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    clip.transform = transformFromJson(json.value(QStringLiteral("transform")).toObject());
    clip.audio = audioFromJson(json.value(QStringLiteral("audio")).toObject());
    return clip;
}

QJsonObject cueToJson(const SubtitleCue &cue)
{
    return {{QStringLiteral("id"), cue.id},
            {QStringLiteral("startUs"), static_cast<double>(cue.startUs)},
            {QStringLiteral("endUs"), static_cast<double>(cue.endUs)},
            {QStringLiteral("text"), cue.text},
            {QStringLiteral("styleId"), cue.styleId},
            {QStringLiteral("positionXPermille"), cue.positionXPermille},
            {QStringLiteral("positionYPermille"), cue.positionYPermille},
            {QStringLiteral("enabled"), cue.enabled}};
}

SubtitleCue cueFromJson(const QJsonObject &json)
{
    SubtitleCue cue;
    cue.id = json.value(QStringLiteral("id")).toString();
    cue.startUs = static_cast<int64_t>(json.value(QStringLiteral("startUs")).toDouble(0));
    cue.endUs = static_cast<int64_t>(json.value(QStringLiteral("endUs")).toDouble(0));
    cue.text = json.value(QStringLiteral("text")).toString();
    cue.styleId = json.value(QStringLiteral("styleId")).toString(QStringLiteral("default"));
    cue.positionXPermille = json.value(QStringLiteral("positionXPermille")).toInt(500);
    cue.positionYPermille = json.value(QStringLiteral("positionYPermille")).toInt(880);
    cue.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    return cue;
}

QJsonObject styleToJson(const SubtitleStyle &style)
{
    return {{QStringLiteral("id"), style.id},
            {QStringLiteral("name"), style.name},
            {QStringLiteral("fontFamily"), style.fontFamily},
            {QStringLiteral("fontPixelSize"), style.fontPixelSize},
            {QStringLiteral("colorArgb"), static_cast<double>(style.colorArgb)},
            {QStringLiteral("outlineColorArgb"), static_cast<double>(style.outlineColorArgb)},
            {QStringLiteral("outlineWidth"), style.outlineWidth},
            {QStringLiteral("backgroundArgb"), static_cast<double>(style.backgroundArgb)},
            {QStringLiteral("alignment"), style.alignment}};
}

SubtitleStyle styleFromJson(const QJsonObject &json)
{
    SubtitleStyle style;
    style.id = json.value(QStringLiteral("id")).toString(QStringLiteral("default"));
    style.name = json.value(QStringLiteral("name")).toString(QStringLiteral("默认样式"));
    style.fontFamily = json.value(QStringLiteral("fontFamily"))
                           .toString(QStringLiteral("Microsoft YaHei"));
    style.fontPixelSize = json.value(QStringLiteral("fontPixelSize")).toInt(48);
    style.colorArgb = static_cast<quint32>(
        json.value(QStringLiteral("colorArgb")).toDouble(0xFFFFFFFFu));
    style.outlineColorArgb = static_cast<quint32>(
        json.value(QStringLiteral("outlineColorArgb")).toDouble(0xFF000000u));
    style.outlineWidth = json.value(QStringLiteral("outlineWidth")).toInt(2);
    style.backgroundArgb = static_cast<quint32>(
        json.value(QStringLiteral("backgroundArgb")).toDouble(0));
    style.alignment = json.value(QStringLiteral("alignment")).toInt(2);
    return style;
}

QJsonObject timelineToJson(const TimelineSettings &settings)
{
    return {{QStringLiteral("width"), settings.width},
            {QStringLiteral("height"), settings.height},
            {QStringLiteral("frameRate"), rationalToJson(settings.fps)},
            {QStringLiteral("sampleRate"), settings.sampleRate},
            {QStringLiteral("colorSpace"), settings.colorSpace},
            {QStringLiteral("dropFrameTimecode"), settings.dropFrameTimecode}};
}

TimelineSettings timelineFromJson(const QJsonObject &json)
{
    TimelineSettings settings;
    settings.width = json.value(QStringLiteral("width")).toInt(1920);
    settings.height = json.value(QStringLiteral("height")).toInt(1080);
    settings.fps = rationalFromJson(json.value(QStringLiteral("frameRate")).toObject());
    if (!settings.fps.isValid())
        settings.fps = Rational{30, 1};
    settings.sampleRate = json.value(QStringLiteral("sampleRate")).toInt(48000);
    settings.colorSpace = json.value(QStringLiteral("colorSpace"))
                              .toString(QStringLiteral("BT.709"));
    settings.dropFrameTimecode = json.value(QStringLiteral("dropFrameTimecode"))
                                     .toBool(false);
    return settings;
}

QJsonObject trackToJson(const Track &track)
{
    QJsonArray clips;
    for (const Clip &clip : track.clips)
        clips.append(clipToJson(clip));
    QJsonArray cues;
    for (const SubtitleCue &cue : track.cues)
        cues.append(cueToJson(cue));
    return {{QStringLiteral("id"), track.id},
            {QStringLiteral("type"), trackTypeToString(track.type)},
            {QStringLiteral("name"), track.name},
            {QStringLiteral("enabled"), track.enabled},
            {QStringLiteral("muted"), track.muted},
            {QStringLiteral("solo"), track.solo},
            {QStringLiteral("locked"), track.locked},
            {QStringLiteral("clips"), clips},
            {QStringLiteral("cues"), cues}};
}

Track trackFromJson(const QJsonObject &json, bool *ok)
{
    Track track;
    track.id = json.value(QStringLiteral("id")).toString();
    bool typeOk = false;
    track.type = trackTypeFromString(json.value(QStringLiteral("type")).toString(), &typeOk);
    track.name = json.value(QStringLiteral("name")).toString();
    track.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    track.muted = json.value(QStringLiteral("muted")).toBool(false);
    track.solo = json.value(QStringLiteral("solo")).toBool(false);
    track.locked = json.value(QStringLiteral("locked")).toBool(false);
    for (const QJsonValue &value : json.value(QStringLiteral("clips")).toArray())
        track.clips.append(clipFromJson(value.toObject()));
    for (const QJsonValue &value : json.value(QStringLiteral("cues")).toArray())
        track.cues.append(cueFromJson(value.toObject()));
    if (ok)
        *ok = typeOk && !track.id.isEmpty();
    return track;
}

QString validateProject(const Project &project)
{
    QSet<QString> trackIds;
    QSet<QString> clipIds;
    QSet<QString> cueIds;
    for (const Track &track : project.tracks) {
        if (track.id.isEmpty() || trackIds.contains(track.id))
            return QStringLiteral("工程包含无效或重复的轨道 id。");
        trackIds.insert(track.id);
        if (track.type == TrackType::Subtitle && !track.clips.isEmpty())
            return QStringLiteral("字幕轨“%1”不能包含视频/音频片段。").arg(track.name);
        if (track.type != TrackType::Subtitle && !track.cues.isEmpty())
            return QStringLiteral("轨道“%1”不能包含字幕条目。").arg(track.name);
        for (const Clip &clip : track.clips) {
            if (clip.id.isEmpty() || clipIds.contains(clip.id))
                return QStringLiteral("轨道“%1”包含无效或重复的片段 id。").arg(track.name);
            clipIds.insert(clip.id);
            if (clip.assetId.isEmpty())
                return QStringLiteral("片段“%1”缺少素材引用。").arg(clip.id);
            if (clip.sourceInUs < 0 || clip.sourceOutUs <= clip.sourceInUs)
                return QStringLiteral("片段“%1”的源区间无效。").arg(clip.id);
            if (clip.timelineStartUs < 0)
                return QStringLiteral("片段“%1”的起始时间为负。").arg(clip.id);
            if (clip.speedMilli < kMinSpeedMilli || clip.speedMilli > kMaxSpeedMilli)
                return QStringLiteral("片段“%1”的变速超出 0.25–4 倍。").arg(clip.id);
        }
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id.isEmpty() || cueIds.contains(cue.id))
                return QStringLiteral("字幕轨“%1”包含无效或重复的字幕 id。").arg(track.name);
            cueIds.insert(cue.id);
            if (cue.endUs <= cue.startUs || cue.startUs < 0)
                return QStringLiteral("字幕“%1”的时间范围无效。").arg(cue.id);
        }
    }
    return {};
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

    QJsonArray tracks;
    for (const Track &track : project.tracks)
        tracks.append(trackToJson(track));

    QJsonArray subtitleStyles;
    for (const SubtitleStyle &style : project.subtitleStyles)
        subtitleStyles.append(styleToJson(style));

    return {
        {QStringLiteral("schemaVersion"), Project::kSchemaVersion},
        {QStringLiteral("name"), project.name},
        {QStringLiteral("timeline"), timelineToJson(project.timeline)},
        {QStringLiteral("assets"), assets},
        {QStringLiteral("tracks"), tracks},
        {QStringLiteral("subtitleStyles"), subtitleStyles},
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

    if (version == 1) {
        project.ensureDefaultTracks();
        return project;
    }

    project.timeline = timelineFromJson(json.value(QStringLiteral("timeline")).toObject());
    for (const QJsonValue &value : json.value(QStringLiteral("tracks")).toArray()) {
        bool ok = false;
        Track track = trackFromJson(value.toObject(), &ok);
        if (!ok) {
            setError(error, QStringLiteral("工程包含无效的轨道条目。"));
            return std::nullopt;
        }
        project.tracks.append(track);
    }
    for (const QJsonValue &value : json.value(QStringLiteral("subtitleStyles")).toArray())
        project.subtitleStyles.append(styleFromJson(value.toObject()));
    // Do not inject default tracks for a v2 file that explicitly has none: an
    // empty timeline is valid, and round-tripping must preserve it exactly.
    // New projects get defaults from ProjectDocument's constructor; v1 files
    // get them in the migration branch above.

    const QString validation = validateProject(project);
    if (!validation.isEmpty()) {
        setError(error, validation);
        return std::nullopt;
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
