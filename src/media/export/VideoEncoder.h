#pragma once

#include <cstdint>
#include <vector>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

#include "ExportPackets.h"
#include "ExportSettings.h"

struct SwsContext;

namespace VideoEditor {

struct EncoderDescriptor;

// Encodes CFR video frames into compressed packets for the segment muxer.
// Frames arrive as tightly packed RGBA (the OfflineFrameRenderer output) and
// are converted to the encoder's pixel format with swscale in BT.709 limited
// range. B-frames stay disabled so pts == dts and packet order is display
// order. One instance is used per export; not thread-safe.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    // Opens `encoder` at the resolution/frame rate/bitrate in `settings`.
    // Rejects odd dimensions and invalid frame rates before touching FFmpeg.
    // On failure fills `error` with a Chinese explanation.
    bool open(const EncoderDescriptor &encoder, const ExportSettings &settings,
              QString *error = nullptr);

    // Converts `rgba` (width * height * 4, top-down tightly packed) and encodes
    // it as frame number `frameIndex`. `forceKeyframe` requests an IDR. New
    // packets are appended to `packets` (packets may be delayed by the
    // encoder, hence the vector).
    bool encodeRgba(const uint8_t *rgba, int64_t frameIndex,
                    bool forceKeyframe, std::vector<EncodedPacket> *packets,
                    QString *error = nullptr);

    // Encodes a black frame (Y=16, chroma=128) without swscale. Used by probe
    // and hardware tests.
    bool encodeBlack(int64_t frameIndex, bool forceKeyframe,
                     std::vector<EncodedPacket> *packets,
                     QString *error = nullptr);

    // Signals end of stream and drains all delayed packets. Safe to call once
    // after the last frame.
    bool flush(std::vector<EncodedPacket> *packets, QString *error = nullptr);

    void close();

    AVCodecContext *context() const { return m_ctx; }
    AVPixelFormat pixelFormat() const { return m_pixelFormat; }
    bool isHardware() const { return m_hardware; }

private:
    bool sendFrame(bool forceKeyframe, int64_t frameIndex,
                   std::vector<EncodedPacket> *packets, QString *error);
    bool receivePackets(std::vector<EncodedPacket> *packets, QString *error);
    void fillBlackFrame();

    AVCodecContext *m_ctx = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_sws = nullptr;
    AVPixelFormat m_pixelFormat = AV_PIX_FMT_NONE;
    int m_width = 0;
    int m_height = 0;
    bool m_hardware = false;
    // Last submitted frame index; only used if an encoder omits pts.
    int64_t m_lastFrameIndex = -1;
};

} // namespace VideoEditor
