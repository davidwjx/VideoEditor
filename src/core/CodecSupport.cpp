#include "CodecSupport.h"

#include <QSet>

namespace VideoEditor {

CodecVerdict classifyVideoCodec(const QString &ffmpegName)
{
    const QString name = ffmpegName.trimmed().toLower();

    if (name == QStringLiteral("mpeg4"))
        return {VideoCodecKind::Mpeg4Part2, true, {}};
    if (name == QStringLiteral("h264"))
        return {VideoCodecKind::H264, true, {}};
    if (name == QStringLiteral("hevc") || name == QStringLiteral("h265"))
        return {VideoCodecKind::H265, true, {}};
    if (name == QStringLiteral("wmv1") || name == QStringLiteral("wmv2")
        || name == QStringLiteral("wmv3") || name == QStringLiteral("vc1")) {
        return {VideoCodecKind::WmvVc1, true, {}};
    }

    return {VideoCodecKind::Unsupported,
            false,
            QStringLiteral("不支持的视频编码（%1）。请先转码为 H.264 或 H.265 的 MP4。")
                .arg(ffmpegName)};
}

bool isSupportedAudioCodec(const QString &ffmpegName)
{
    static const QSet<QString> kSupported{
        QStringLiteral("aac"), QStringLiteral("mp3"), QStringLiteral("flac"),
        QStringLiteral("pcm_s16le"), QStringLiteral("pcm_s24le"),
        QStringLiteral("pcm_s32le"), QStringLiteral("pcm_f32le"),
        QStringLiteral("alac")};
    return kSupported.contains(ffmpegName.trimmed().toLower());
}

QString friendlyCodecName(VideoCodecKind kind)
{
    switch (kind) {
    case VideoCodecKind::Mpeg4Part2:
        return QStringLiteral("MPEG-4 Part 2");
    case VideoCodecKind::H264:
        return QStringLiteral("H.264");
    case VideoCodecKind::H265:
        return QStringLiteral("H.265");
    case VideoCodecKind::WmvVc1:
        return QStringLiteral("WMV/VC-1");
    case VideoCodecKind::Unsupported:
        return QStringLiteral("未知编码");
    }
    return QStringLiteral("未知编码");
}

} // namespace VideoEditor
