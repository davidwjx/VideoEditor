#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>

namespace VideoEditor {

// Min/max envelope of a source decoded to 48 kHz stereo, in 10 ms buckets.
struct WaveformPeaks {
    static constexpr int kBucketsPerSecond = 100;
    QList<float> minValues;
    QList<float> maxValues;
    int64_t durationUs = 0;

    int bucketCount() const
    {
        return int(std::min(minValues.size(), maxValues.size()));
    }

    bool isEmpty() const { return bucketCount() == 0; }
};

// Decodes the whole source and reduces it to min/max peaks. Returns an empty
// waveform on failure and fills `error` with a user-facing Chinese message.
WaveformPeaks computeWaveform(const QString &path, QString *error = nullptr);

// Binary cache payload: magic "VEWF", version 1, duration and int16 min/max
// pairs (little endian).
QByteArray encodeWaveform(const WaveformPeaks &peaks);

// Returns std::nullopt when the payload has a wrong magic/version or its
// declared length does not match the data.
std::optional<WaveformPeaks> decodeWaveform(const QByteArray &bytes);

} // namespace VideoEditor
