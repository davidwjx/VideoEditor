#pragma once

#include <cstdint>

#include <QString>

#include "ExportSettings.h"
#include "MediaInfo.h"

namespace VideoEditor {

// Result of re-probing a finished export. `ok` is true only when the container,
// codecs, geometry, frame rate, packet counts and both track durations match
// the frozen export timing within one frame (audio within one AAC frame), and
// the first video packet actually decodes with a software decoder.
struct ExportValidation {
    bool ok = false;
    QString error;
    MediaInfo info;
    int64_t videoDurationUs = 0;
    int64_t audioDurationUs = 0;
    int64_t videoPacketCount = 0;
    int64_t audioSampleCount = 0;
    bool decodedFirstFrame = false;
};

ExportValidation validateExportFile(const QString &path,
                                    const ExportSettings &settings,
                                    const ExportTiming &timing);

} // namespace VideoEditor
