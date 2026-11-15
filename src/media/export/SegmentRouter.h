#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "ExportPackets.h"
#include "ExportSettings.h"
#include "SegmentWriter.h"
#include "TimeBase.h"

namespace VideoEditor {

// Routes encoder packets (global frame numbers) into independent MP4 parts so
// that each part starts on its segment's first frame. The cursor is monotonic:
// encoder output must be in display order (B-frames stay disabled). At most
// four writers stay open so a crash loses little work; completed parts are
// verified packet-for-packet against the timeline before they are counted.
class SegmentRouter {
public:
    SegmentRouter(QString partsDir, ExportTiming timing, Rational fps,
                  AVCodecContext *videoCtx);
    ~SegmentRouter();

    SegmentRouter(const SegmentRouter &) = delete;
    SegmentRouter &operator=(const SegmentRouter &) = delete;

    bool routeVideo(const EncodedPacket &packet, QString *error = nullptr);

    // Closes and validates every open part with index <= segmentIndex.
    bool closeThrough(int segmentIndex, QString *error = nullptr);

    // Closes every remaining part. With allowPartialLastSegment the final part
    // must contain exactly its timeline frame range and is counted; otherwise
    // (cancel/failure) the open parts are deleted and never counted.
    bool finishAll(bool allowPartialLastSegment, QString *error = nullptr);

    // Number of segments validated by this router instance. A resumed export
    // starts at its checkpoint segment, so callers add the segment they
    // resumed from (ExportPipeline does exactly that).
    int completedSegments() const { return m_completedCount; }
    int64_t bytesWritten() const { return m_bytes; }

private:
    bool ensureWriter(int segmentIndex, QString *error);
    int segmentForVideo(int64_t pts, QString *error);
    bool closeWriter(int segmentIndex, QString *error);
    bool validateWriter(int segmentIndex, SegmentWriter *writer,
                        QString *error) const;

    QString m_partsDir;
    ExportTiming m_timing;
    Rational m_fps;
    AVCodecContext *m_videoCtx = nullptr;
    std::map<int, std::unique_ptr<SegmentWriter>> m_writers;
    int m_videoCursor = 0;
    int m_firstSegment = -1;
    int m_lastCompleted = -1;
    int m_completedCount = 0;
    int64_t m_bytes = 0;
};

} // namespace VideoEditor
