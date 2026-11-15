#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "AudioEncoder.h"
#include "TimelineMixer.h"

using namespace VideoEditor;

namespace {
std::vector<float> sine(int frames, int channels, double hz, int rate)
{
    std::vector<float> out(size_t(frames) * size_t(channels));
    for (int i = 0; i < frames; ++i) {
        const float v = float(std::sin(2.0 * 3.14159265358979323846 * hz * i / rate));
        for (int c = 0; c < channels; ++c)
            out[size_t(i) * size_t(channels) + size_t(c)] = v;
    }
    return out;
}
} // namespace

TEST(AudioEncoder, EncodesAacLcAtFortyEightKilohertz)
{
    AudioEncoder encoder;
    QString error;
    ASSERT_TRUE(encoder.open(48000, 2, 192, &error)) << error.toStdString();
    ASSERT_EQ(encoder.frameSize(), 1024);

    const auto input = sine(48000, 2, 440.0, 48000);
    std::vector<EncodedPacket> all;
    int64_t sampleIndex = 0;
    for (; sampleIndex + 1024 <= 48000; sampleIndex += 1024) {
        std::vector<EncodedPacket> packets;
        ASSERT_TRUE(encoder.encode(input.data() + sampleIndex * 2, 1024,
                                   sampleIndex, &packets, &error))
            << error.toStdString();
        for (const EncodedPacket &packet : packets)
            all.push_back(packet);
    }
    std::vector<float> tail(1024 * 2, 0.0f);
    const int valid = int(48000 - sampleIndex);
    std::copy(input.begin() + sampleIndex * 2, input.end(), tail.begin());
    std::vector<EncodedPacket> last;
    ASSERT_TRUE(encoder.encode(tail.data(), 1024, sampleIndex, &last, &error))
        << error.toStdString();
    for (const EncodedPacket &packet : last)
        all.push_back(packet);
    std::vector<EncodedPacket> flushed;
    ASSERT_TRUE(encoder.flush(&flushed, &error)) << error.toStdString();
    for (const EncodedPacket &packet : flushed)
        all.push_back(packet);

    int64_t durationSum = 0;
    for (const EncodedPacket &packet : all)
        durationSum += packet.duration;
    EXPECT_EQ(durationSum, 48'128); // 47 AAC frames, 128 padded samples
    EXPECT_GT(valid, 0);
    EXPECT_GT(all.size(), size_t(40));
}

TEST(TimelineMixer, SamplePositionIsExactAcrossBlocks)
{
    TimelineMixer mixer(48000, 2);
    Project project;
    project.ensureDefaultTracks();
    mixer.setProject(project);
    mixer.setPositionSamples(1'234'567);
    std::vector<float> block(480 * 2, 0.0f);
    for (int i = 0; i < 10; ++i)
        mixer.render(block.data(), 480);
    EXPECT_EQ(mixer.positionSamples(), 1'234'567 + 4800);
    EXPECT_EQ(mixer.positionUs(),
              ((1'234'567 + 4800) * 1'000'000LL + 24000) / 48000);
}
