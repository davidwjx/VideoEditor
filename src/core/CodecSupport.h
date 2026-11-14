#pragma once

#include <QString>

namespace VideoEditor {

enum class VideoCodecKind {
    Mpeg4Part2,
    H264,
    H265,
    WmvVc1,
    Unsupported
};

struct CodecVerdict {
    VideoCodecKind kind = VideoCodecKind::Unsupported;
    bool supported = false;
    QString reason;
};

CodecVerdict classifyVideoCodec(const QString &ffmpegName);
QString friendlyCodecName(VideoCodecKind kind);

} // namespace VideoEditor
