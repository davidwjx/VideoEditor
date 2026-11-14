#pragma once

#include <vector>

namespace VideoEditor {

// Linear resampling used for natural (pitch-changing) speed changes.
// For every output frame i, reads the source at position i * step and
// interpolates between the two neighbouring source frames. Positions past the
// end of the source clamp to its last frame.
void resampleLinear(const float *src, int srcFrames, int channels, double step,
                    float *dst, int dstFrames);

// Preview-grade pitch-preserving time stretcher: 20 ms Hann window with 50 %
// overlap (OLA). The caller pushes source frames and pulls timeline frames;
// speed > 1 shortens the material without changing pitch.
class OlaTimeStretcher {
public:
    explicit OlaTimeStretcher(int channels);
    void reset();
    void push(const float *interleaved, int frames);
    int pull(float *out, int maxFrames, double speed);
    int bufferedFrames() const { return m_inputFrames - m_inputRead; }
    static constexpr int kWindowFrames = 960; // 20 ms at 48 kHz
    static constexpr int kHopFrames = 480;    // 10 ms

private:
    int m_channels;
    std::vector<float> m_input;
    std::vector<float> m_accumulator;
    int m_inputRead = 0;
    int m_inputFrames = 0;
};

} // namespace VideoEditor
