#pragma once

#include <cstdint>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

struct MediaInfo {
    QString path;
    QString container;
    QString videoCodec;
    QString audioCodec;
    int width = 0;
    int height = 0;
    Rational frameRate;
    int64_t durationUs = 0;
    int rotationDegrees = 0;
    bool hasAudio = false;
    bool supported = false;
    QString unsupportedReason;

    bool operator==(const MediaInfo &) const = default;
};

} // namespace VideoEditor
