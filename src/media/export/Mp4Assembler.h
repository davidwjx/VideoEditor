#pragma once

#include <cstdint>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "ExportSettings.h"
#include "TimeBase.h"

namespace VideoEditor {

// Re-muxes the independent video parts and the single continuous audio part
// into one ordinary (non-fragmented, +faststart) MP4 at `tempOutput`.
//
// Video packets are rewritten at their exact global frame offsets. Audio
// packets keep their own timestamps: the first packet is still the AAC priming
// packet at -audioEncoderDelay, and assembly writes the skip_samples /
// discard_padding contract explicitly. A part whose priming position was lost
// (for example a muxer that folded it into sample 0) is rejected instead of
// producing a silently shifted audio track.
class Mp4Assembler {
public:
    static bool assemble(const QString &partsDir, const QString &audioPartPath,
                         const ExportTiming &timing, const Rational &fps,
                         AVCodecContext *video, AVCodecContext *audio,
                         int64_t audioEncoderDelay, int64_t paddedSamples,
                         const QString &tempOutput, QString *error = nullptr);
};

} // namespace VideoEditor
