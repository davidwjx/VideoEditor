#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ClipAudioReader.h"
#include "FfmpegAudioSource.h"
#include "SpeedProcessor.h"

using namespace VideoEditor;

namespace {

std::vector<float> sine(int frames, int channels, double frequency, int sampleRate)
{
    std::vector<float> result(size_t(frames) * channels);
    for (int i = 0; i < frames; ++i) {
        const float value = float(std::sin(2.0 * 3.14159265358979323846
                                           * frequency * i / sampleRate));
        for (int c = 0; c < channels; ++c)
            result[size_t(i) * channels + c] = value;
    }
    return result;
}

int zeroCrossings(const std::vector<float> &samples, int channels)
{
    int crossings = 0;
    float previous = samples.empty() ? 0.0f : samples[0];
    for (size_t i = 1; i < samples.size() / size_t(channels); ++i) {
        const float current = samples[i * size_t(channels)];
        if ((previous <= 0.0f && current > 0.0f)
            || (previous >= 0.0f && current < 0.0f)) {
            ++crossings;
        }
        previous = current;
    }
    return crossings;
}

QString toneFixturePath()
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
        .filePath(QStringLiteral("tone.wav"));
}

Asset toneAsset()
{
    Asset asset{QStringLiteral("a"), toneFixturePath(), MediaInfo{}, {}};
    asset.info.kind = MediaKind::Audio;
    asset.info.hasAudio = true;
    asset.info.durationUs = 2'000'000;
    return asset;
}

// Writes a 48 kHz s16le stereo sine WAV with the same per-channel amplitude
// as the mono tone.wav fixture (4095), for level-comparison tests only.
QString writeStereoToneWav(const QString &directory)
{
    constexpr int kSampleRate = 48000;
    constexpr int kFrames = 9600; // 0.2 s
    QByteArray samples(size_t(kFrames) * 4, '\0');
    for (int i = 0; i < kFrames; ++i) {
        const double value = 4095.0
            * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / kSampleRate);
        const auto sample = static_cast<quint16>(qint16(std::lround(value)));
        for (int c = 0; c < 2; ++c) {
            const size_t offset = size_t(i) * 4 + size_t(c) * 2;
            samples[int(offset)] = char(sample & 0xFF);
            samples[int(offset) + 1] = char((sample >> 8) & 0xFF);
        }
    }

    QByteArray bytes;
    auto appendTag = [&bytes](const char *tag) { bytes.append(tag, 4); };
    auto appendU16 = [&bytes](quint16 value) {
        bytes.append(char(value & 0xFF));
        bytes.append(char((value >> 8) & 0xFF));
    };
    auto appendU32 = [&bytes](quint32 value) {
        bytes.append(char(value & 0xFF));
        bytes.append(char((value >> 8) & 0xFF));
        bytes.append(char((value >> 16) & 0xFF));
        bytes.append(char((value >> 24) & 0xFF));
    };
    appendTag("RIFF");
    appendU32(quint32(36 + samples.size()));
    appendTag("WAVE");
    appendTag("fmt ");
    appendU32(16);
    appendU16(1);                                  // PCM
    appendU16(2);                                  // stereo
    appendU32(kSampleRate);
    appendU32(kSampleRate * 2 * 2);                // byte rate
    appendU16(2 * 2);                              // block align
    appendU16(16);                                 // bits per sample
    appendTag("data");
    appendU32(quint32(samples.size()));
    bytes.append(samples);

    const QString path = QDir(directory).filePath(QStringLiteral("stereo-tone.wav"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(bytes);
    file.close();
    return path;
}

float peakOf(const std::vector<float> &samples)
{
    float peak = 0.0f;
    for (float sample : samples)
        peak = std::max(peak, std::abs(sample));
    return peak;
}

} // namespace

TEST(AudioSpeed, NaturalSpeedChangesPitchAndDuration)
{
    const int sampleRate = 48000;
    const auto input = sine(sampleRate, 2, 440.0, sampleRate); // 1 s
    std::vector<float> output(size_t(sampleRate / 2) * 2);
    resampleLinear(input.data(), sampleRate, 2, 2.0, output.data(), sampleRate / 2);
    // 880 Hz over 0.5 s -> about 880 zero crossings.
    EXPECT_NEAR(zeroCrossings(output, 2), 880, 40);
}

TEST(AudioSpeed, OlaPreservesPitchWhileHalvingDuration)
{
    const int sampleRate = 48000;
    const auto input = sine(sampleRate, 2, 440.0, sampleRate);
    OlaTimeStretcher stretcher(2);
    stretcher.push(input.data(), sampleRate);
    std::vector<float> output(size_t(sampleRate / 2) * 2);
    const int produced = stretcher.pull(output.data(), sampleRate / 2, 2.0);
    output.resize(size_t(produced) * 2);
    ASSERT_GT(produced, sampleRate / 4);
    // 440 Hz over 0.5 s -> about 440 crossings.
    EXPECT_NEAR(zeroCrossings(output, 2), 440, 40);
}

TEST(AudioSpeed, ClipReaderConsumesSourceAtClipSpeed)
{
    const QString path = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                             .filePath(QStringLiteral("tone.wav"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing tone.wav";
    Asset asset{QStringLiteral("a"), path, MediaInfo{}, {}};
    asset.info.kind = MediaKind::Audio;
    asset.info.hasAudio = true;
    asset.info.durationUs = 2'000'000;

    ClipAudioReader reader;
    QString error;
    ASSERT_TRUE(reader.open(asset, 48000, 2, &error)) << error.toStdString();

    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 2'000'000;
    clip.speedMilli = 2000;
    clip.preservePitch = false;

    std::vector<float> output(size_t(24000) * 2);
    reader.render(clip, 0, 24000, output.data()); // 0.5 s timeline = 1 s source
    double energy = 0.0;
    for (float sample : output)
        energy += double(sample) * sample;
    EXPECT_GT(energy / output.size(), 0.01);
}

TEST(AudioSpeed, LinearResampleTruncatesAtSourceOut)
{
    if (!QFileInfo::exists(toneFixturePath()))
        GTEST_SKIP() << "missing tone.wav";

    ClipAudioReader reader;
    QString error;
    ASSERT_TRUE(reader.open(toneAsset(), 48000, 2, &error)) << error.toStdString();

    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceInUs = 1'990'000;
    clip.sourceOutUs = 2'000'000;
    clip.speedMilli = 2000;
    clip.preservePitch = false;

    // 10 ms source (480 frames) at 2x yields 5 ms (240) of valid output.
    std::vector<float> out(size_t(480) * 2);
    reader.render(clip, 0, 480, out.data());

    double headEnergy = 0.0;
    for (int i = 0; i < 240; ++i) {
        headEnergy += double(out[size_t(i) * 2]) * out[size_t(i) * 2];
        headEnergy += double(out[size_t(i) * 2 + 1]) * out[size_t(i) * 2 + 1];
    }
    EXPECT_GT(headEnergy, 0.01);

    // The remainder must be exact silence, not a held last sample (DC).
    for (int i = 240; i < 480; ++i) {
        EXPECT_EQ(out[size_t(i) * 2], 0.0f) << "frame " << i;
        EXPECT_EQ(out[size_t(i) * 2 + 1], 0.0f) << "frame " << i;
    }
}

TEST(AudioSpeed, RendersFromNonZeroSourceInWithoutOffset)
{
    if (!QFileInfo::exists(toneFixturePath()))
        GTEST_SKIP() << "missing tone.wav";

    QString error;
    auto source = FfmpegAudioSource::open(toneFixturePath(), 48000, 2, &error);
    ASSERT_TRUE(source) << error.toStdString();

    const int64_t sourceInUs = 1'234'567;
    std::vector<float> reference(size_t(4800) * 2);
    ASSERT_TRUE(source->readRange(sourceInUs, 4800, reference.data(), &error))
        << error.toStdString();

    ClipAudioReader reader;
    ASSERT_TRUE(reader.open(toneAsset(), 48000, 2, &error)) << error.toStdString();
    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceInUs = sourceInUs;
    clip.sourceOutUs = 2'000'000;
    clip.speedMilli = 1000;

    std::vector<float> out(size_t(4800) * 2);
    reader.render(clip, 0, 4800, out.data());

    // ClipAudioReader applies only the documented mono +3 dB preview trim on
    // top of the raw source (factor 2 on swr's 1/sqrt(2) mono upmix).
    for (size_t i = 0; i < out.size(); ++i)
        EXPECT_NEAR(out[i], reference[i] * 2.0f, 1e-5f);
}

TEST(AudioSpeed, MonoAndStereoFollowDocumentedDualMonoConvention)
{
    if (!QFileInfo::exists(toneFixturePath()))
        GTEST_SKIP() << "missing tone.wav";

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString stereoPath = writeStereoToneWav(directory.path());
    ASSERT_FALSE(stereoPath.isEmpty());

    QString error;
    ClipAudioReader monoReader;
    ASSERT_TRUE(monoReader.open(toneAsset(), 48000, 2, &error)) << error.toStdString();

    Asset stereoAsset{QStringLiteral("s"), stereoPath, MediaInfo{}, {}};
    stereoAsset.info.kind = MediaKind::Audio;
    stereoAsset.info.hasAudio = true;
    stereoAsset.info.audioChannels = 2;
    stereoAsset.info.durationUs = 200'000;
    ClipAudioReader stereoReader;
    ASSERT_TRUE(stereoReader.open(stereoAsset, 48000, 2, &error))
        << error.toStdString();

    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceOutUs = 200'000;

    std::vector<float> mono(size_t(4800) * 2);
    std::vector<float> stereo(size_t(4800) * 2);
    monoReader.render(clip, 0, 4800, mono.data());
    stereoReader.render(clip, 0, 4800, stereo.data());

    // This pins the documented preview convention: a mono source at the same
    // per-channel level as a stereo source is +3 dB (sqrt(2)) hotter in the
    // ClipAudioReader. FfmpegAudioSource itself stays at the standard level.
    const float expectedRatio = std::sqrt(2.0f);
    EXPECT_NEAR(peakOf(mono), peakOf(stereo) * expectedRatio,
                0.02f * peakOf(stereo) * expectedRatio + 1e-5f);
}

TEST(AudioSpeed, BoundarySpeedsRenderFiniteAudio)
{
    if (!QFileInfo::exists(toneFixturePath()))
        GTEST_SKIP() << "missing tone.wav";

    ClipAudioReader reader;
    QString error;
    ASSERT_TRUE(reader.open(toneAsset(), 48000, 2, &error)) << error.toStdString();

    for (int speedMilli : {250, 4000}) {
        for (bool preservePitch : {false, true}) {
            reader.reset();
            Clip clip;
            clip.assetId = QStringLiteral("a");
            clip.sourceOutUs = 2'000'000;
            clip.speedMilli = speedMilli;
            clip.preservePitch = preservePitch;

            std::vector<float> out(size_t(4800) * 2);
            reader.render(clip, 0, 4800, out.data());

            double energy = 0.0;
            bool finite = true;
            for (float sample : out) {
                finite = finite && std::isfinite(sample);
                EXPECT_LE(std::abs(sample), 1.0f);
                energy += double(sample) * sample;
            }
            EXPECT_TRUE(finite) << speedMilli << " preserve=" << preservePitch;
            EXPECT_GT(energy, 0.0) << speedMilli << " preserve=" << preservePitch;
        }
    }
}

TEST(AudioSpeed, ShortClipWithOlaIsAudible)
{
    if (!QFileInfo::exists(toneFixturePath()))
        GTEST_SKIP() << "missing tone.wav";

    ClipAudioReader reader;
    QString error;
    ASSERT_TRUE(reader.open(toneAsset(), 48000, 2, &error)) << error.toStdString();

    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceOutUs = 15'000; // 15 ms of source, shorter than the 20 ms window
    clip.speedMilli = 2000;
    clip.preservePitch = true;

    std::vector<float> out(size_t(480) * 2);
    reader.render(clip, 0, 480, out.data());
    EXPECT_GT(peakOf(out), 0.005f);
}

TEST(AudioSpeed, OlaSlowSpeedKeepsStartAndTail)
{
    if (!QFileInfo::exists(toneFixturePath()))
        GTEST_SKIP() << "missing tone.wav";

    ClipAudioReader reader;
    QString error;
    ASSERT_TRUE(reader.open(toneAsset(), 48000, 2, &error)) << error.toStdString();

    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceOutUs = 2'000'000;
    clip.speedMilli = 250; // 8 s timeline
    clip.preservePitch = true;

    std::vector<float> out(size_t(480) * 2);
    // The pre-roll must make the very first block audible (was ~80 ms silent).
    reader.render(clip, 0, 480, out.data());
    EXPECT_GT(peakOf(out), 0.01f);

    float lastBlockPeak = 0.0f;
    const int blocks = 8 * 100; // 8 s in 10 ms blocks
    for (int block = 1; block < blocks; ++block) {
        reader.render(clip, int64_t(block) * 10'000, 480, out.data());
        if (block == blocks - 1)
            lastBlockPeak = peakOf(out);
    }
    // The zero-padded final windows must emit the time-stretched tail instead
    // of stopping one window early.
    EXPECT_GT(lastBlockPeak, 0.005f);
}
