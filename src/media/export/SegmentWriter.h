#pragma once

#include <cstdint>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "ExportPackets.h"
#include "TimeBase.h"

namespace VideoEditor {

// Writes one independent, trailer-complete MP4 video part. Packets carry
// global frame numbers; the writer rebases them so the part starts at frame 0
// and records one CFR frame per packet. Parts contain no audio: the whole
// timeline shares a single continuous AAC track written by AudioTrackWriter.
class SegmentWriter {
public:
    SegmentWriter() = default;
    ~SegmentWriter();

    SegmentWriter(const SegmentWriter &) = delete;
    SegmentWriter &operator=(const SegmentWriter &) = delete;

    // Opens `path` for `fps` and copies the encoder parameters (SPS/PPS) from
    // `video`. Every packet written to this part must have pts >= rebaseFrame.
    bool open(const QString &path, const Rational &fps, AVCodecContext *video,
              int64_t rebaseFrame, QString *error = nullptr);

    bool writeVideo(const EncodedPacket &packet, QString *error = nullptr);

    // Writes the trailer, closes the file and records the final byte count.
    // Safe to call once; the destructor calls it when the writer is still open
    // so a cancel path never leaks a file handle.
    bool finish(QString *error = nullptr);

    int64_t bytesWritten() const { return m_bytesWritten; }
    int64_t videoPackets() const { return m_videoPackets; }

private:
    AVFormatContext *m_oc = nullptr;
    AVStream *m_video = nullptr;
    AVPacket *m_packet = nullptr;
    Rational m_fps{0, 1};
    int64_t m_rebaseFrame = 0;
    int64_t m_frameDuration = 0;
    int64_t m_bytesWritten = 0;
    int64_t m_videoPackets = 0;
    bool m_ok = false;
};

} // namespace VideoEditor
