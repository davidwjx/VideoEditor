#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>

#include <d3d11.h>

#include <QString>

#include "MediaInfo.h"

struct AVFrame;

namespace VideoEditor {

struct DecodedFrame {
    enum class Kind { HardwareTexture, CpuRgba };

    Kind kind = Kind::CpuRgba;
    ID3D11Texture2D *texture = nullptr;
    UINT subresource = 0;
    int width = 0;
    int height = 0;
    int64_t ptsUs = 0;
    std::vector<uint8_t> rgba;
    AVFrame *avFrame = nullptr;

    DecodedFrame() = default;
    DecodedFrame(const DecodedFrame &) = delete;
    DecodedFrame &operator=(const DecodedFrame &) = delete;
    DecodedFrame(DecodedFrame &&other) noexcept;
    DecodedFrame &operator=(DecodedFrame &&other) noexcept;
    ~DecodedFrame();
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool seek(int64_t ptsUs, QString *error = nullptr) = 0;
    virtual bool decodeNext(DecodedFrame &frame, QString *error = nullptr) = 0;
    virtual const MediaInfo &info() const = 0;
    virtual bool isHardware() const = 0;
};

} // namespace VideoEditor

#endif
