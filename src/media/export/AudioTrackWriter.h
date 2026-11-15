#pragma once

#include <cstdint>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "ExportPackets.h"

namespace VideoEditor {

// Writes the whole timeline as one continuous AAC MP4 track
// (<parts>/audio.mp4). Video is segmented; audio is not, so the encoder's
// single AAC priming packet appears exactly once for the whole export and a
// resumed export simply re-encodes audio from sample 0.
//
// The part is written with edit lists enabled and negative timestamps left
// untouched: the first packet keeps pts == -encoderDelay, which is how the
// AAC pre-roll position survives a round trip through MP4. Mp4Assembler is
// then responsible for the explicit skip_samples/discard_padding contract in
// the final file. Using avoid_negative_ts=make_zero (or use_editlist=0) here
// would silently fold the priming into sample 0 and shift all audio later by
// encoderDelay/sampleRate (~21.3 ms for AAC at 48 kHz).
class AudioTrackWriter {
public:
    AudioTrackWriter() = default;
    ~AudioTrackWriter();

    AudioTrackWriter(const AudioTrackWriter &) = delete;
    AudioTrackWriter &operator=(const AudioTrackWriter &) = delete;

    bool open(const QString &path, int sampleRate, AVCodecContext *audio,
              int64_t totalSamples, QString *error = nullptr);
    bool write(const EncodedPacket &packet, QString *error = nullptr);
    bool finish(QString *error = nullptr);

    int64_t bytesWritten() const { return m_bytesWritten; }

private:
    AVFormatContext *m_oc = nullptr;
    AVStream *m_stream = nullptr;
    AVPacket *m_packet = nullptr;
    int m_sampleRate = 0;
    int64_t m_bytesWritten = 0;
    bool m_ok = false;
};

} // namespace VideoEditor
