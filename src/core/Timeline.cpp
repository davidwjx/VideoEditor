#include "Timeline.h"

#include <QtGlobal>

namespace VideoEditor {

namespace {

int clampSpeed(int speedMilli)
{
    return qBound(kMinSpeedMilli, speedMilli, kMaxSpeedMilli);
}

} // namespace

QString trackTypeToString(TrackType type)
{
    switch (type) {
    case TrackType::Video:
        return QStringLiteral("video");
    case TrackType::Audio:
        return QStringLiteral("audio");
    case TrackType::Subtitle:
        return QStringLiteral("subtitle");
    }
    return QStringLiteral("video");
}

TrackType trackTypeFromString(const QString &text, bool *ok)
{
    const QString value = text.trimmed().toLower();
    if (ok)
        *ok = true;
    if (value == QStringLiteral("video"))
        return TrackType::Video;
    if (value == QStringLiteral("audio"))
        return TrackType::Audio;
    if (value == QStringLiteral("subtitle"))
        return TrackType::Subtitle;
    if (ok)
        *ok = false;
    return TrackType::Video;
}

QList<SubtitleStyle> builtinSubtitleStyles()
{
    SubtitleStyle boxed;
    boxed.id = QStringLiteral("boxed");
    boxed.name = QStringLiteral("黑底白字");
    boxed.backgroundArgb = 0xB0000000;
    boxed.outlineWidth = 1;

    SubtitleStyle bold;
    bold.id = QStringLiteral("bold-outline");
    bold.name = QStringLiteral("粗描边");
    bold.fontPixelSize = 56;
    bold.outlineWidth = 4;

    return {SubtitleStyle{}, boxed, bold};
}

int64_t scaleBySpeed(int64_t timelineDeltaUs, int speedMilli)
{
    if (timelineDeltaUs <= 0)
        return 0;
    return (timelineDeltaUs * clampSpeed(speedMilli) + 500) / 1000;
}

int64_t unscaleBySpeed(int64_t sourceDeltaUs, int speedMilli)
{
    if (sourceDeltaUs <= 0)
        return 0;
    const int speed = clampSpeed(speedMilli);
    return (sourceDeltaUs * 1000 + speed / 2) / speed;
}

int64_t Clip::timelineDurationUs() const
{
    return unscaleBySpeed(sourceOutUs - sourceInUs, speedMilli);
}

int64_t sourceTimeAt(const Clip &clip, int64_t timelineUs)
{
    const int64_t deltaUs = qMax<int64_t>(0, timelineUs - clip.timelineStartUs);
    const int64_t sourceUs = clip.sourceInUs + scaleBySpeed(deltaUs, clip.speedMilli);
    return qBound(clip.sourceInUs, sourceUs, clip.sourceOutUs);
}

} // namespace VideoEditor
