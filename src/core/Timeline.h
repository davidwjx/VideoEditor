#pragma once

#include <cstdint>
#include <QList>
#include <QString>
#include <QtGlobal>

#include "TimeBase.h"

namespace VideoEditor {

enum class TrackType { Video, Audio, Subtitle };

QString trackTypeToString(TrackType type);
TrackType trackTypeFromString(const QString &text, bool *ok = nullptr);

constexpr int kDefaultSpeedMilli = 1000;
constexpr int kMinSpeedMilli = 250;
constexpr int kMaxSpeedMilli = 4000;

struct TimelineSettings {
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    int sampleRate = 48000;
    QString colorSpace = QStringLiteral("BT.709");
    bool dropFrameTimecode = false;

    bool operator==(const TimelineSettings &) const = default;
};

struct Transform {
    int scaleMilli = 1000;      // 1000 = fit inside the frame
    int offsetXPermille = 0;    // -1000..1000 of frame width from center
    int offsetYPermille = 0;
    int opacityMilli = 1000;

    bool operator==(const Transform &) const = default;
};

struct VolumeKeyframe {
    int64_t timeUs = 0;         // relative to the clip start on the timeline
    int volumeMilli = 1000;

    bool operator==(const VolumeKeyframe &) const = default;
};

struct AudioParams {
    int volumeMilli = 1000;
    int panMilli = 0;           // -1000 left .. 1000 right
    int64_t fadeInUs = 0;
    int64_t fadeOutUs = 0;
    bool muted = false;
    QList<VolumeKeyframe> keyframes;

    bool operator==(const AudioParams &) const = default;
};

struct Clip {
    QString id;
    QString assetId;
    QString linkId;             // empty = unlinked; linked clips share the id
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    int64_t timelineStartUs = 0;
    int speedMilli = kDefaultSpeedMilli;
    bool preservePitch = false;
    bool enabled = true;
    Transform transform;
    AudioParams audio;

    int64_t timelineDurationUs() const;
    int64_t timelineEndUs() const { return timelineStartUs + timelineDurationUs(); }

    bool operator==(const Clip &) const = default;
};

struct SubtitleCue {
    QString id;
    int64_t startUs = 0;
    int64_t endUs = 0;
    QString text;
    QString styleId = QStringLiteral("default");
    int positionXPermille = 500;
    int positionYPermille = 880;
    bool enabled = true;

    bool operator==(const SubtitleCue &) const = default;
};

struct SubtitleStyle {
    QString id = QStringLiteral("default");
    QString name = QStringLiteral("默认样式");
    QString fontFamily = QStringLiteral("Microsoft YaHei");
    int fontPixelSize = 48;
    quint32 colorArgb = 0xFFFFFFFF;
    quint32 outlineColorArgb = 0xFF000000;
    int outlineWidth = 2;
    quint32 backgroundArgb = 0x00000000;
    int alignment = 2;          // Qt::AlignHCenter

    bool operator==(const SubtitleStyle &) const = default;
};

struct Track {
    QString id;
    TrackType type = TrackType::Video;
    QString name;
    bool enabled = true;
    bool muted = false;
    bool solo = false;
    bool locked = false;
    QList<Clip> clips;
    QList<SubtitleCue> cues;

    bool operator==(const Track &) const = default;
};

QList<SubtitleStyle> builtinSubtitleStyles();

// Integer speed mapping. Both directions round half up so a clip that is
// trimmed and untrimmed by the same amount does not drift.
int64_t scaleBySpeed(int64_t timelineDeltaUs, int speedMilli);
int64_t unscaleBySpeed(int64_t sourceDeltaUs, int speedMilli);
int64_t sourceTimeAt(const Clip &clip, int64_t timelineUs);

} // namespace VideoEditor
