#pragma once

#include <cstdint>
#include <optional>
#include <QString>

namespace VideoEditor {

struct Rational {
    int num = 0;
    int den = 1;

    bool isValid() const { return num > 0 && den > 0; }
    double toDouble() const { return isValid() ? double(num) / double(den) : 0.0; }
    bool operator==(const Rational &) const = default;
};

constexpr int64_t kMicrosPerSecond = 1'000'000;

int64_t framesToMicros(int64_t frames, Rational fps);
int64_t microsToFrames(int64_t micros, Rational fps);
QString formatTimecode(int64_t micros, Rational fps);
QString formatTimecodeAtRate(int64_t micros, Rational fps, bool dropFrame);
QString formatDuration(int64_t micros);

// Parses a user-entered position. Accepted forms: "HH:MM:SS:FF",
// "HH:MM:SS;FF", "HH:MM:SS.mmm", "MM:SS.mmm" and plain seconds. Frame fields
// are converted with fps. Returns std::nullopt for malformed or negative
// input, or when a frame field is outside 0..fps-1.
std::optional<int64_t> parseTimecode(const QString &text, Rational fps);

} // namespace VideoEditor
