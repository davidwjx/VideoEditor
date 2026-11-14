#include "FfmpegError.h"

#include <QFileInfo>

extern "C" {
#include <libavutil/error.h>
}

namespace VideoEditor {

QString ffmpegErrorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

QString cannotOpenMessage(const QString &path, int error)
{
    return QStringLiteral("无法打开“%1”：%2\n请确认文件存在且未损坏，或先转码为 H.264/H.265 的 MP4。")
        .arg(QFileInfo(path).fileName(), ffmpegErrorString(error));
}

} // namespace VideoEditor
