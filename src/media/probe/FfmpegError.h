#pragma once

#include <QString>

namespace VideoEditor {

QString ffmpegErrorString(int error);
QString cannotOpenMessage(const QString &path, int error);

} // namespace VideoEditor
