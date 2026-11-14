#include "SpeedProcessor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace VideoEditor {

void resampleLinear(const float *src, int srcFrames, int channels, double step,
                    float *dst, int dstFrames)
{
    if (!src || !dst || channels <= 0 || srcFrames <= 0 || dstFrames <= 0 || step <= 0.0)
        return;

    const int lastFrame = srcFrames - 1;
    for (int i = 0; i < dstFrames; ++i) {
        const double position = double(i) * step;
        const int lowerUnclamped = int(std::floor(position));
        const float fraction = float(position - double(lowerUnclamped));
        const int lower = std::clamp(lowerUnclamped, 0, lastFrame);
        const int upper = std::clamp(lowerUnclamped + 1, 0, lastFrame);
        for (int c = 0; c < channels; ++c) {
            const float a = src[size_t(lower) * size_t(channels) + size_t(c)];
            const float b = src[size_t(upper) * size_t(channels) + size_t(c)];
            dst[size_t(i) * size_t(channels) + size_t(c)] = a + (b - a) * fraction;
        }
    }
}

OlaTimeStretcher::OlaTimeStretcher(int channels)
    : m_channels(std::max(1, channels))
    , m_accumulator(size_t(kWindowFrames) * size_t(m_channels), 0.0f)
{
}

void OlaTimeStretcher::reset()
{
    m_input.clear();
    std::fill(m_accumulator.begin(), m_accumulator.end(), 0.0f);
    m_inputRead = 0;
    m_inputFrames = 0;
}

void OlaTimeStretcher::push(const float *interleaved, int frames)
{
    if (!interleaved || frames <= 0)
        return;
    m_input.insert(m_input.end(), interleaved,
                   interleaved + size_t(frames) * size_t(m_channels));
    m_inputFrames += frames;
}

int OlaTimeStretcher::pull(float *out, int maxFrames, double speed)
{
    static const std::vector<float> window = [] {
        std::vector<float> values(kWindowFrames);
        for (int i = 0; i < kWindowFrames; ++i) {
            values[size_t(i)] = 0.5f
                - 0.5f * std::cos(2.0f * 3.14159265358979323846f
                                  * float(i) / float(kWindowFrames - 1));
        }
        return values;
    }();

    if (!out || maxFrames <= 0)
        return 0;

    speed = std::clamp(speed, 0.25, 4.0);
    const int analysisHop = int(std::lround(double(kHopFrames) * speed));
    int produced = 0;

    while (produced + kHopFrames <= maxFrames) {
        if (m_inputRead + kWindowFrames > m_inputFrames)
            break;
        for (int i = 0; i < kWindowFrames; ++i) {
            const float gain = window[size_t(i)];
            for (int c = 0; c < m_channels; ++c) {
                m_accumulator[size_t(i) * size_t(m_channels) + size_t(c)] +=
                    m_input[size_t(m_inputRead + i) * size_t(m_channels) + size_t(c)]
                    * gain;
            }
        }
        for (int i = 0; i < kHopFrames; ++i) {
            for (int c = 0; c < m_channels; ++c) {
                out[size_t(produced + i) * size_t(m_channels) + size_t(c)] =
                    m_accumulator[size_t(i) * size_t(m_channels) + size_t(c)];
            }
        }
        produced += kHopFrames;
        m_accumulator.erase(m_accumulator.begin(),
                            m_accumulator.begin() + kHopFrames * m_channels);
        m_accumulator.insert(m_accumulator.end(),
                             size_t(kHopFrames) * size_t(m_channels), 0.0f);
        m_inputRead += analysisHop;
    }

    // Keep the tail that may still contribute to an overlapping window, but
    // release the history once it is safely behind the read cursor.
    if (m_inputRead > kWindowFrames * 50) {
        m_input.erase(m_input.begin(),
                      m_input.begin() + m_inputRead * m_channels);
        m_inputFrames -= m_inputRead;
        m_inputRead = 0;
    }
    return produced;
}

} // namespace VideoEditor
