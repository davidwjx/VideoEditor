#include "TimeBase.h"

#include <algorithm>
#include <cmath>

namespace VideoEditor {

namespace {

int64_t clampNonNegative(int64_t value)
{
    return value < 0 ? 0 : value;
}

} // namespace

int64_t framesToMicros(int64_t frames, Rational fps)
{
    if (!fps.isValid())
        return 0;
    const long double micros = static_cast<long double>(frames) * kMicrosPerSecond
                               * fps.den / fps.num;
    return static_cast<int64_t>(std::llround(micros));
}

int64_t microsToFrames(int64_t micros, Rational fps)
{
    if (!fps.isValid())
        return 0;
    const long double frames = static_cast<long double>(micros) * fps.num
                               / (kMicrosPerSecond * static_cast<long double>(fps.den));
    return static_cast<int64_t>(std::llround(frames));
}

QString formatTimecode(int64_t micros, Rational fps)
{
    micros = clampNonNegative(micros);
    if (!fps.isValid())
        fps = Rational{30, 1};

    const int nominalFps = std::max(1, static_cast<int>(std::llround(fps.toDouble())));
    const int64_t totalFrames = microsToFrames(micros, fps);
    const int64_t frames = totalFrames % nominalFps;
    const int64_t totalSeconds = totalFrames / nominalFps;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;

    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}

QString formatTimecodeAtRate(int64_t micros, Rational fps, bool dropFrame)
{
    micros = clampNonNegative(micros);
    if (!fps.isValid())
        fps = Rational{30, 1};
    if (!dropFrame)
        return formatTimecode(micros, fps);

    // Nominal rate is the rounded frame rate: 29.97 -> 30, 59.94 -> 60.
    const int64_t nominalFps =
        std::max<int64_t>(1, (static_cast<int64_t>(fps.num) + fps.den / 2) / fps.den);
    // 29.97 -> 2, 59.94 -> 4 dropped frame numbers per minute (round(nominal / 15)).
    const int64_t dropPerMinute = (nominalFps + 7) / 15;

    int64_t frames = microsToFrames(micros, fps);
    const int64_t framesPerMinute = nominalFps * 60 - dropPerMinute;
    const int64_t framesPer10Minutes = nominalFps * 600 - dropPerMinute * 9;
    const int64_t tens = frames / framesPer10Minutes;
    const int64_t remainder = frames % framesPer10Minutes;
    if (remainder >= dropPerMinute) {
        frames += dropPerMinute * 9 * tens
                  + dropPerMinute * ((remainder - dropPerMinute) / framesPerMinute);
    } else {
        frames += dropPerMinute * 9 * tens;
    }

    const int64_t frameNumber = frames % nominalFps;
    const int64_t totalSeconds = frames / nominalFps;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3;%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frameNumber, 2, 10, QLatin1Char('0'));
}

QString formatDuration(int64_t micros)
{
    micros = clampNonNegative(micros);
    const int64_t totalMs = micros / 1000;
    const int64_t ms = totalMs % 1000;
    const int64_t totalSeconds = totalMs / 1000;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;

    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

} // namespace VideoEditor
