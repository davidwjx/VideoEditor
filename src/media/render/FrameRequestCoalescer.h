#pragma once

#include <cstdint>
#include <optional>

namespace VideoEditor {

// Single-in-flight request gate for timeline renders. While a frame is being
// composed, further requests only move the desired target; when the in-flight
// frame completes, at most the latest target is dispatched. This keeps a slow
// renderer from queueing every tick and trailing the audio clock.
class FrameRequestCoalescer {
public:
    // Records timelineUs as the latest wanted time. Returns the time that must
    // be dispatched now, or nullopt when a request is already in flight.
    std::optional<int64_t> request(int64_t timelineUs)
    {
        m_desiredUs = timelineUs;
        if (m_pending)
            return std::nullopt;
        m_pending = true;
        return timelineUs;
    }

    // Marks the in-flight frame (renderedUs, or the skipped request's time) as
    // finished. Returns the latest target to dispatch, if it still differs.
    std::optional<int64_t> complete(int64_t renderedUs)
    {
        m_pending = false;
        if (m_desiredUs >= 0 && m_desiredUs != renderedUs) {
            m_pending = true;
            return m_desiredUs;
        }
        return std::nullopt;
    }

    void reset()
    {
        m_desiredUs = -1;
        m_pending = false;
    }

    bool pending() const { return m_pending; }
    int64_t desired() const { return m_desiredUs; }

private:
    int64_t m_desiredUs = -1;
    bool m_pending = false;
};

} // namespace VideoEditor
