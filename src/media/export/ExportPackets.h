#pragma once

#include <cstdint>
#include <vector>

namespace VideoEditor {

// One compressed video packet produced by VideoEncoder. Times are in the
// encoder time base (the export frame rate), so a CFR export yields
// duration == 1. `dts` equals `pts` because the export pipeline never uses
// B-frames.
struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    bool key = false;
};

} // namespace VideoEditor
