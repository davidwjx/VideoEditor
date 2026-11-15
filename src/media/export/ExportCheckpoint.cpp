#include "ExportCheckpoint.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "ExportSettings.h"
#include "FfmpegError.h"
#include "Project.h"
#include "ProjectSerializer.h"

namespace VideoEditor {

namespace {

std::optional<ExportCheckpoint> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

} // namespace

QJsonObject ExportCheckpoint::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("version"), version);
    json.insert(QStringLiteral("projectHash"), projectHash);
    json.insert(QStringLiteral("encoderKey"), encoderKey);
    json.insert(QStringLiteral("width"), width);
    json.insert(QStringLiteral("height"), height);
    json.insert(QStringLiteral("fpsNum"), fps.num);
    json.insert(QStringLiteral("fpsDen"), fps.den);
    json.insert(QStringLiteral("sampleRate"), sampleRate);
    json.insert(QStringLiteral("totalFrames"), QJsonValue(totalFrames));
    json.insert(QStringLiteral("totalSamples"), QJsonValue(totalSamples));
    json.insert(QStringLiteral("segmentSamples"), QJsonValue(segmentSamples));
    json.insert(QStringLiteral("segmentCount"), segmentCount);
    json.insert(QStringLiteral("completedSegments"), completedSegments);
    json.insert(QStringLiteral("bytesWritten"), QJsonValue(bytesWritten));
    json.insert(QStringLiteral("lastError"), lastError);
    return json;
}

std::optional<ExportCheckpoint> ExportCheckpoint::fromJson(const QJsonObject &json,
                                                           QString *error)
{
    if (error)
        error->clear();

    const QJsonValue versionValue = json.value(QStringLiteral("version"));
    if (!versionValue.isDouble())
        return fail(error, QStringLiteral("导出检查点缺少版本字段。"));
    const int version = versionValue.toInt();
    if (version != kVersion) {
        return fail(error, QStringLiteral(
                               "导出检查点版本 %1 与当前版本 %2 不兼容，将从头导出。")
                               .arg(version).arg(kVersion));
    }

    ExportCheckpoint checkpoint;
    checkpoint.version = version;
    checkpoint.projectHash =
        json.value(QStringLiteral("projectHash")).toString();
    checkpoint.encoderKey = json.value(QStringLiteral("encoderKey")).toString();
    checkpoint.width = json.value(QStringLiteral("width")).toInt();
    checkpoint.height = json.value(QStringLiteral("height")).toInt();
    checkpoint.fps.num = json.value(QStringLiteral("fpsNum")).toInt();
    checkpoint.fps.den = json.value(QStringLiteral("fpsDen")).toInt();
    checkpoint.sampleRate = json.value(QStringLiteral("sampleRate")).toInt();
    checkpoint.totalFrames =
        json.value(QStringLiteral("totalFrames")).toInteger();
    checkpoint.totalSamples =
        json.value(QStringLiteral("totalSamples")).toInteger();
    checkpoint.segmentSamples =
        json.value(QStringLiteral("segmentSamples")).toInteger();
    checkpoint.segmentCount =
        json.value(QStringLiteral("segmentCount")).toInt();
    checkpoint.completedSegments =
        json.value(QStringLiteral("completedSegments")).toInt();
    checkpoint.bytesWritten =
        json.value(QStringLiteral("bytesWritten")).toInteger();
    checkpoint.lastError = json.value(QStringLiteral("lastError")).toString();

    if (checkpoint.projectHash.isEmpty())
        return fail(error, QStringLiteral("导出检查点缺少工程指纹。"));
    if (!checkpoint.fps.isValid() || checkpoint.sampleRate <= 0
        || checkpoint.width <= 0 || checkpoint.height <= 0) {
        return fail(error, QStringLiteral("导出检查点包含无效的格式参数。"));
    }
    if (checkpoint.totalFrames <= 0 || checkpoint.totalSamples <= 0
        || checkpoint.segmentSamples <= 0 || checkpoint.segmentCount <= 0) {
        return fail(error, QStringLiteral("导出检查点包含无效的时间线长度。"));
    }
    if (checkpoint.completedSegments < 0
        || checkpoint.completedSegments > checkpoint.segmentCount) {
        return fail(error, QStringLiteral("导出检查点的已完成分段数超出范围。"));
    }
    return checkpoint;
}

bool ExportCheckpoint::save(const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入导出检查点：%1")
                         .arg(file.errorString());
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("无法提交导出检查点：%1")
                         .arg(file.errorString());
        return false;
    }
    return true;
}

std::optional<ExportCheckpoint> ExportCheckpoint::load(const QString &path,
                                                       QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("无法读取导出检查点：%1")
                               .arg(file.errorString()));
    }
    const QByteArray data = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QStringLiteral("导出检查点格式损坏：%1")
                               .arg(parseError.errorString()));
    }
    return fromJson(document.object(), error);
}

QString exportProjectHash(const Project &project, const ExportSettings &settings)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), ExportCheckpoint::kVersion);
    root.insert(QStringLiteral("project"), ProjectSerializer::toJson(project));
    root.insert(QStringLiteral("settings"), exportSettingsToJson(settings));
    const QByteArray canonical =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString partsDirectoryFor(const QString &outputPath)
{
    return outputPath + QStringLiteral(".parts");
}

QString segmentPath(const QString &partsDir, int segmentIndex)
{
    return QDir(partsDir).filePath(
        QStringLiteral("segment_%1.mp4")
            .arg(segmentIndex, 5, 10, QLatin1Char('0')));
}

QString assemblyTempPathFor(const QString &outputPath)
{
    return outputPath + QStringLiteral(".assembling.mp4");
}

void clearParts(const QString &partsDir)
{
    QDir dir(partsDir);
    if (dir.exists())
        dir.removeRecursively();
}

bool inspectSegment(const QString &path, SegmentCounts *counts, QString *error)
{
    if (counts)
        *counts = SegmentCounts{};
    AVFormatContext *in = nullptr;
    if (avformat_open_input(&in, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        if (error)
            *error = QStringLiteral("无法读取分段：%1").arg(path);
        return false;
    }
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        if (error)
            *error = QStringLiteral("分段缺少视频流：%1").arg(path);
        avformat_close_input(&in);
        return false;
    }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        if (error)
            *error = QStringLiteral("无法分配分段校验缓冲区。");
        avformat_close_input(&in);
        return false;
    }
    bool firstVideo = true;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            if (firstVideo && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                if (error)
                    *error = QStringLiteral("分段首帧不是关键帧：%1").arg(path);
                av_packet_free(&pkt);
                avformat_close_input(&in);
                return false;
            }
            firstVideo = false;
            if (counts)
                ++counts->videoPackets;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&in);
    if (firstVideo) {
        if (error)
            *error = QStringLiteral("分段不包含视频帧：%1").arg(path);
        return false;
    }
    return true;
}

} // namespace VideoEditor
