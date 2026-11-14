#include "Waveform.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <QDataStream>
#include <QIODevice>

#include "FfmpegAudioSource.h"
#include "MediaProbe.h"

namespace VideoEditor {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kSamplesPerBucket = 480;   // 10 ms at 48 kHz
constexpr int kBucketsPerRead = 10;      // 100 ms per decoded block
// Reject implausible durations before converting to int or allocating; the
// binary cache format stores the count as qint32 anyway.
constexpr qint64 kMaxWaveformBuckets = qint64(24) * 60 * 60 * 100;

} // namespace

WaveformPeaks computeWaveform(const QString &path, QString *error)
{
    WaveformPeaks peaks;
    const auto info = MediaProbe::probe(path, error);
    if (!info)
        return peaks;
    peaks.durationUs = info->durationUs;
    const qint64 bucketCount64 = (peaks.durationUs + 9'999) / 10'000;
    if (bucketCount64 <= 0 || bucketCount64 > kMaxWaveformBuckets) {
        if (error)
            *error = QStringLiteral("音频时长无效或过长，无法生成波形。");
        return WaveformPeaks{};
    }
    const int buckets = int(bucketCount64);
    peaks.minValues = QList<float>(buckets, 0.0f);
    peaks.maxValues = QList<float>(buckets, 0.0f);

    auto source = FfmpegAudioSource::open(path, kSampleRate, kChannels, error);
    if (!source)
        return WaveformPeaks{};

    // libswresample upmixes mono to stereo with a power-preserving 1/sqrt(2)
    // matrix, which would make a mono waveform look 3 dB quieter than it is.
    // Compensate so peaks describe the source level.
    const float channelGain = source->sourceChannels() == 1
                                  ? std::sqrt(2.0f)
                                  : 1.0f;

    std::vector<float> block(size_t(kSamplesPerBucket) * kBucketsPerRead
                             * kChannels);
    int bucket = 0;
    while (bucket < buckets) {
        const int64_t startUs = int64_t(bucket) * 10'000;
        // readRange returns true and pads with silence at EOF; a false return
        // is a real decode error. Do not publish or cache a partial envelope.
        if (!source->readRange(startUs, int(block.size() / kChannels),
                               block.data(), error))
            return WaveformPeaks{};
        const int availableBuckets = qMin(kBucketsPerRead, buckets - bucket);
        for (int b = 0; b < availableBuckets; ++b) {
            float minimum = 0.0f;
            float maximum = 0.0f;
            for (int i = 0; i < kSamplesPerBucket; ++i) {
                const size_t index = (size_t(b) * kSamplesPerBucket + i)
                                     * kChannels;
                const float value = channelGain * 0.5f
                                    * (block[index] + block[index + 1]);
                minimum = qMin(minimum, value);
                maximum = qMax(maximum, value);
            }
            peaks.minValues[bucket] = minimum;
            peaks.maxValues[bucket] = maximum;
            ++bucket;
        }
    }
    return peaks;
}

QByteArray encodeWaveform(const WaveformPeaks &peaks)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("VEWF", 4);
    stream << quint16(1) << qint64(peaks.durationUs)
           << qint32(peaks.bucketCount());
    for (int i = 0; i < peaks.bucketCount(); ++i) {
        stream << qint16(qRound(qBound(-1.0f, peaks.minValues.at(i), 1.0f)
                                * 32767.0f))
               << qint16(qRound(qBound(-1.0f, peaks.maxValues.at(i), 1.0f)
                                * 32767.0f));
    }
    return bytes;
}

std::optional<WaveformPeaks> decodeWaveform(const QByteArray &bytes)
{
    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::LittleEndian);
    char magic[4] = {};
    quint16 version = 0;
    qint64 durationUs = 0;
    qint32 count = 0;
    if (stream.readRawData(magic, 4) != 4 || std::memcmp(magic, "VEWF", 4) != 0)
        return std::nullopt;
    stream >> version >> durationUs >> count;
    if (stream.status() != QDataStream::Ok || version != 1 || durationUs < 0
        || count < 0)
        return std::nullopt;
    const qint64 expectedSize = 4 + 2 + 8 + 4 + qint64(count) * 4;
    if (qint64(bytes.size()) < expectedSize)
        return std::nullopt;

    WaveformPeaks peaks;
    peaks.durationUs = durationUs;
    peaks.minValues.reserve(count);
    peaks.maxValues.reserve(count);
    for (int i = 0; i < count; ++i) {
        qint16 minimum = 0;
        qint16 maximum = 0;
        stream >> minimum >> maximum;
        peaks.minValues.append(minimum / 32767.0f);
        peaks.maxValues.append(maximum / 32767.0f);
    }
    if (stream.status() != QDataStream::Ok)
        return std::nullopt;
    return peaks;
}

} // namespace VideoEditor
