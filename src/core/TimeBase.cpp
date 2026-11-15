#include "TimeBase.h"

#include <algorithm>
#include <cmath>

#include <QStringList>

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
    // Drop-frame numbering is only defined for non-integer NTSC rates
    // (29.97/59.94). Integer rates keep every frame label.
    if (!dropFrame || fps.num % fps.den == 0)
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

std::optional<int64_t> parseTimecode(const QString &text, Rational fps)
{
    QString value = text.trimmed();
    value.replace(QLatin1Char(';'), QLatin1Char(':'));
    if (value.isEmpty())
        return std::nullopt;
    if (!fps.isValid())
        fps = Rational{30, 1};

    const QStringList parts = value.split(QLatin1Char(':'));
    if (parts.size() > 4)
        return std::nullopt;

    bool frameField = false;
    double seconds = 0.0;
    int64_t totalSecondsPart = 0;
    int64_t frames = 0;

    if (parts.size() == 1) {
        bool ok = false;
        seconds = parts.first().toDouble(&ok);
        if (!ok || seconds < 0.0)
            return std::nullopt;
    } else {
        QStringList whole = parts;
        QString last = whole.takeLast();
        // A decimal point means the final component is a fractional second
        // (HH:MM:SS.mmm / MM:SS.mmm); otherwise it is a frame number when
        // there are four components, or whole seconds for HH:MM:SS.
        if (last.contains(QLatin1Char('.'))) {
            bool ok = false;
            seconds = last.toDouble(&ok);
            if (!ok || seconds < 0.0 || seconds >= 60.0)
                return std::nullopt;
        } else if (parts.size() == 4) {
            bool ok = false;
            frames = last.toLongLong(&ok);
            const int nominalFps =
                std::max(1, int(std::llround(fps.toDouble())));
            if (!ok || frames < 0 || frames >= nominalFps)
                return std::nullopt;
            frameField = true;
            totalSecondsPart = 0;
        } else if (parts.size() == 3) {
            bool ok = false;
            totalSecondsPart = last.toLongLong(&ok);
            if (!ok || totalSecondsPart < 0 || totalSecondsPart >= 60)
                return std::nullopt;
        } else {
            return std::nullopt;
        }

        // With a frame field the remaining components end in seconds, so the
        // first multiplier is 1; otherwise the last component was seconds and
        // the next one up is minutes (×60).
        int64_t multiplier = frameField ? 1 : 60;
        for (int i = whole.size() - 1; i >= 0; --i) {
            bool ok = false;
            const int64_t component = whole.at(i).toLongLong(&ok);
            if (!ok || component < 0)
                return std::nullopt;
            totalSecondsPart += component * multiplier;
            multiplier *= 60;
        }
    }

    const int64_t secondsUs = int64_t(std::llround(seconds * 1'000'000.0));
    const int64_t frameUs = frameField ? framesToMicros(frames, fps) : 0;
    const int64_t totalUs = totalSecondsPart * kMicrosPerSecond + secondsUs
                            + frameUs;
    if (totalUs < 0)
        return std::nullopt;
    return totalUs;
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
