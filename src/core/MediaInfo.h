#pragma once

#include <cstdint>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

enum class MediaKind { Unknown, Video, Audio };

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
    MediaKind kind = MediaKind::Unknown;
    bool hasVideo = false;
    int audioSampleRate = 0;
    int audioChannels = 0;
    int64_t audioDurationUs = 0;

    bool operator==(const MediaInfo &) const = default;
};

} // namespace VideoEditor
