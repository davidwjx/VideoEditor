#include "IVideoDecoder.h"

#ifdef _WIN32

extern "C" {
#include <libavutil/frame.h>
}

namespace VideoEditor {

DecodedFrame::DecodedFrame(DecodedFrame &&other) noexcept
    : kind(other.kind)
    , texture(other.texture)
    , subresource(other.subresource)
    , width(other.width)
    , height(other.height)
    , ptsUs(other.ptsUs)
    , rgba(std::move(other.rgba))
    , avFrame(other.avFrame)
{
    other.texture = nullptr;
    other.avFrame = nullptr;
}

DecodedFrame &DecodedFrame::operator=(DecodedFrame &&other) noexcept
{
    if (this == &other)
        return *this;
    if (avFrame)
        av_frame_free(&avFrame);
    kind = other.kind;
    texture = other.texture;
    subresource = other.subresource;
    width = other.width;
    height = other.height;
    ptsUs = other.ptsUs;
    rgba = std::move(other.rgba);
    avFrame = other.avFrame;
    other.texture = nullptr;
    other.avFrame = nullptr;
    return *this;
}

DecodedFrame::~DecodedFrame()
{
    if (avFrame)
        av_frame_free(&avFrame);
}

} // namespace VideoEditor

#endif
