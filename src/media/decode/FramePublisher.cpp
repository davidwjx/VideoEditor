#include "FramePublisher.h"

#ifdef _WIN32

#include <cmath>
#include <memory>

#include "D3D11Context.h"
#include "Nv12Converter.h"
#include "RgbaConverter.h"
#include "SharedTextureRing.h"

namespace VideoEditor {

FramePublisher::FramePublisher(D3D11Context &context, SharedTextureRing &ring)
    : m_context(context)
    , m_ring(ring)
{
}

FramePublisher::~FramePublisher() = default;

bool FramePublisher::publish(DecodedFrame &frame, int rotationDegrees,
                             QString *error)
{
    if (m_ring.count() <= 0) {
        if (error)
            *error = QStringLiteral("共享纹理环尚未创建。");
        return false;
    }

    if (!m_nv12) {
        m_nv12 = std::make_unique<Nv12Converter>();
        if (!m_nv12->initialize(m_context, error))
            return false;
    }
    if (!m_rgba) {
        m_rgba = std::make_unique<RgbaConverter>();
        if (!m_rgba->initialize(m_context, error))
            return false;
    }

    const int slot = m_ring.nextWriteIndex();
    IDXGIKeyedMutex *mutex = m_ring.mutex(slot);
    // The consumer presents one frame at a time and releases abandoned slots,
    // so a wait here should be near-instant. Keep it bounded: a long stall
    // would freeze playback with no feedback.
    if (!mutex || mutex->AcquireSync(0, 1000) != S_OK) {
        if (error)
            *error = QStringLiteral("等待共享纹理写入超时。");
        return false;
    }

    // MediaProbe reports the clockwise display rotation, but sample-space UV
    // rotation is counter-clockwise for a positive angle, so negate here.
    const float radians = float(-rotationDegrees) * 3.14159265358979323846f / 180.0f;
    bool ok = false;
    if (frame.kind == DecodedFrame::Kind::HardwareTexture) {
        ok = m_nv12->convert(frame.texture, frame.subresource, frame.width,
                             frame.height, m_ring.rtv(slot), radians, error);
    } else {
        ok = m_rgba->convertRgba(frame.rgba, frame.width, frame.height,
                                 m_ring.rtv(slot), radians, error);
    }

    mutex->ReleaseSync(ok ? 1 : 0);
    if (!ok)
        return false;

    m_lastSlot = slot;
    m_ring.advanceWriteIndex();
    return true;
}

} // namespace VideoEditor

#endif
