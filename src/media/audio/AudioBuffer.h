#pragma once

#include <cstdint>
#include <vector>

namespace VideoEditor {

struct AudioBuffer {
    std::vector<float> samples;   // interleaved
    int frames = 0;
    int channels = 2;
    int sampleRate = 48000;

    void resize(int frameCount, int channelCount)
    {
        channels = channelCount;
        frames = frameCount;
        samples.assign(size_t(frameCount) * size_t(channelCount), 0.0f);
    }
};

} // namespace VideoEditor
