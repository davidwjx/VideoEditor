#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;
class SharedTextureRing;
class Nv12Converter;
class RgbaConverter;

class FramePublisher {
public:
    FramePublisher(D3D11Context &context, SharedTextureRing &ring);
    ~FramePublisher();

    bool publish(DecodedFrame &frame, int rotationDegrees,
                 QString *error = nullptr);
    int lastWrittenSlot() const { return m_lastSlot; }

private:
    D3D11Context &m_context;
    SharedTextureRing &m_ring;
    std::unique_ptr<Nv12Converter> m_nv12;
    std::unique_ptr<RgbaConverter> m_rgba;
    int m_lastSlot = -1;
};

} // namespace VideoEditor

#endif
