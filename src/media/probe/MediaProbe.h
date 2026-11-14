#pragma once

#include <optional>
#include <QString>

#include "MediaInfo.h"

namespace VideoEditor {

class MediaProbe {
public:
    static std::optional<MediaInfo> probe(const QString &path,
                                          QString *error = nullptr);
};

} // namespace VideoEditor
