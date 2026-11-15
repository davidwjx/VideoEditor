#pragma once

#include <QList>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

#include "ExportSettings.h"

namespace VideoEditor {

// One row of the process-wide encoder capability table. `hardware` describes
// the intended path (hardware MFT/GPU vs. software); `available` is the result
// of an actual open + tiny encode probe at the requested resolution/fps.
struct EncoderDescriptor {
    QString id;       // FFmpeg encoder name, e.g. "h264_mf".
    ExportVideoCodec codec = ExportVideoCodec::Mpeg4Part2;
    bool hardware = false;
    QString displayName;
    bool available = false;
    QString unavailableReason;

    // `h264_mf` appears once as a hardware and once as a software candidate,
    // so the id alone is not unique. Checkpoints persist this key.
    QString key() const;
};

class EncoderCatalog {
public:
    QList<EncoderDescriptor> encoders;

    // Picks an encoder for `codec`. H.265 only ever returns a hardware row and
    // otherwise returns an empty id plus a Chinese reason. For H.264/H.265 the
    // preference is honoured when possible and falls back to the other path.
    QString selectEncoder(ExportVideoCodec codec, bool preferHardware,
                          bool *hardwareOut, QString *reason) const;
    bool hasHardware(ExportVideoCodec codec) const;
    bool hasAny(ExportVideoCodec codec) const;
    const EncoderDescriptor *findByKey(const QString &key) const;

    // Deterministic catalog for tests: MPEG-4, Media Foundation software H.264
    // and OpenH264 are available; every hardware row (including H.265) is not.
    // Never touches FFmpeg.
    static EncoderCatalog withoutHardwareForTest();
};

// Probes every candidate at `width`x`height`/`fps`, caching the result for the
// lifetime of the process (per encoder path and target format). `forceReprobe`
// clears the cache first.
EncoderCatalog detectEncoders(int width, int height, const Rational &fps,
                              bool forceReprobe = false);

// Opens `encoderId`, encodes up to three black frames and reports whether at
// least one non-empty packet was produced. Never throws; fills `error` with a
// Chinese explanation on failure.
bool probeEncoder(const QString &encoderId, ExportVideoCodec codec,
                  bool hardware, int width, int height, const Rational &fps,
                  QString *error = nullptr);

// MPEG-4 and OpenH264 take YUV420P; every hardware encoder and Media
// Foundation software H.264/HEVC take system-memory NV12.
AVPixelFormat encoderPixelFormat(const EncoderDescriptor &encoder);

// Applies per-encoder options that cannot be expressed through
// AVCodecContext fields alone (Media Foundation `hw_encoding`, zero B-frames).
// Failed option sets are ignored; the probe/encode test is the source of truth.
void applyEncoderOptions(AVCodecContext *ctx, const EncoderDescriptor &encoder);

void resetEncoderProbeCacheForTest();

} // namespace VideoEditor
