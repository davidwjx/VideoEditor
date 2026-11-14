#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "ClipAudioReader.h"
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
