#include "SegmentRouter.h"

#include <algorithm>

#include <QFile>

#include "ExportCheckpoint.h"

namespace VideoEditor {

namespace {
constexpr int kMaxOpenWriters = 4;
}

SegmentRouter::SegmentRouter(QString partsDir, ExportTiming timing, Rational fps,
                             AVCodecContext *videoCtx)
    : m_partsDir(std::move(partsDir))
    , m_timing(std::move(timing))
    , m_fps(std::move(fps))
    , m_videoCtx(videoCtx)
{
}

SegmentRouter::~SegmentRouter()
{
    for (auto &entry : m_writers)
        entry.second->finish(nullptr);
    m_writers.clear();
}

bool SegmentRouter::ensureWriter(int segmentIndex, QString *error)
{
    if (m_writers.find(segmentIndex) != m_writers.end())
        return true;
    if (segmentIndex <= m_lastCompleted) {
        if (error)
            *error = QStringLiteral("分段 %1 已经关闭，编码器输出顺序异常。")
                         .arg(segmentIndex);
        return false;
    }
    int64_t vBegin = 0;
    int64_t vEnd = 0;
    videoFrameRangeForSegment(m_timing, m_fps, segmentIndex, &vBegin, &vEnd);
    auto writer = std::make_unique<SegmentWriter>();
    if (!writer->open(segmentPath(m_partsDir, segmentIndex), m_fps, m_videoCtx,
                      vBegin, error)) {
        return false;
    }
    m_writers.emplace(segmentIndex, std::move(writer));
    return true;
}

bool SegmentRouter::validateSegmentRanges(QString *error) const
{
    if (!m_fps.isValid() || m_timing.segmentCount <= 0
        || m_timing.segmentSamples <= 0) {
        if (error)
            *error = QStringLiteral("导出时间线没有有效的分段信息。");
        return false;
    }
    for (int i = 0; i < m_timing.segmentCount; ++i) {
        int64_t vBegin = 0;
        int64_t vEnd = 0;
        videoFrameRangeForSegment(m_timing, m_fps, i, &vBegin, &vEnd);
        if (vEnd <= vBegin) {
            if (error)
                *error = QStringLiteral(
                             "分段 %1 的时间范围不包含任何视频帧，"
                             "无法生成有效的 MP4 分段。")
                             .arg(i);
            return false;
        }
    }
    return true;
}

int SegmentRouter::segmentForVideo(int64_t pts, QString *error)
{
    if (!m_fps.isValid() || m_timing.segmentCount <= 0
        || m_timing.segmentSamples <= 0) {
        if (error)
            *error = QStringLiteral("导出时间线没有有效的分段信息。");
        return -1;
    }
    if (m_videoCursor >= m_timing.segmentCount) {
        if (error)
            *error = QStringLiteral("编码器输出的帧号 %1 超出时间线。").arg(pts);
        return -1;
    }

    int64_t vBegin = 0;
    int64_t vEnd = 0;
    videoFrameRangeForSegment(m_timing, m_fps, m_videoCursor, &vBegin, &vEnd);
    if (pts < vBegin) {
        if (error)
            *error = QStringLiteral(
                         "编码器输出顺序异常：帧 %1 早于当前分段 %2 的起点 %3。")
                         .arg(pts).arg(m_videoCursor).arg(vBegin);
        return -1;
    }
    while (m_videoCursor + 1 < m_timing.segmentCount && pts >= vEnd) {
        ++m_videoCursor;
        videoFrameRangeForSegment(m_timing, m_fps, m_videoCursor, &vBegin, &vEnd);
        if (pts < vBegin) {
            if (error)
                *error = QStringLiteral(
                             "编码器输出跳过了分段 %1（帧 %2 落在下一段起点 %3 之前）。")
                             .arg(m_videoCursor - 1).arg(pts).arg(vBegin);
            return -1;
        }
    }
    if (pts >= vEnd) {
        if (error)
            *error = QStringLiteral("编码器输出的帧号 %1 超出时间线总帧数。").arg(pts);
        return -1;
    }
    return m_videoCursor;
}

bool SegmentRouter::routeVideo(const EncodedPacket &packet, QString *error)
{
    if (!m_videoCtx) {
        if (error)
            *error = QStringLiteral("分段路由缺少视频编码器上下文。");
        return false;
    }
    // Reject a timeline with an empty (zero-frame) segment before writing any
    // part; such a segment can never be routed and would leave a missing MP4.
    if (m_firstSegment < 0 && !validateSegmentRanges(error))
        return false;
    const int segment = segmentForVideo(packet.pts, error);
    if (segment < 0)
        return false;
    if (m_firstSegment < 0)
        m_firstSegment = segment;
    if (!ensureWriter(segment, error))
        return false;
    if (!m_writers.at(segment)->writeVideo(packet, error))
        return false;

    if (int(m_writers.size()) > kMaxOpenWriters) {
        if (!closeThrough(m_videoCursor - (kMaxOpenWriters - 1), error))
            return false;
    }
    return true;
}

bool SegmentRouter::validateWriter(int segmentIndex, SegmentWriter *writer,
                                   QString *error) const
{
    int64_t vBegin = 0;
    int64_t vEnd = 0;
    videoFrameRangeForSegment(m_timing, m_fps, segmentIndex, &vBegin, &vEnd);
    const int64_t expected = vEnd - vBegin;
    if (writer->videoPackets() != expected) {
        if (error)
            *error = QStringLiteral(
                         "分段 %1 的编码包数量与时间线不一致（视频 %2/%3）。")
                         .arg(segmentIndex)
                         .arg(writer->videoPackets())
                         .arg(expected);
        return false;
    }
    return true;
}

bool SegmentRouter::closeWriter(int segmentIndex, QString *error)
{
    const auto it = m_writers.find(segmentIndex);
    if (it == m_writers.end())
        return true;
    SegmentWriter *writer = it->second.get();
    if (!validateWriter(segmentIndex, writer, error)) {
        writer->finish(nullptr);
        m_writers.erase(it);
        QFile::remove(segmentPath(m_partsDir, segmentIndex));
        return false;
    }
    if (!writer->finish(error)) {
        m_writers.erase(it);
        QFile::remove(segmentPath(m_partsDir, segmentIndex));
        return false;
    }
    m_bytes += writer->bytesWritten();
    m_lastCompleted = std::max(m_lastCompleted, segmentIndex);
    ++m_completedCount;
    m_writers.erase(it);
    return true;
}

bool SegmentRouter::closeThrough(int segmentIndex, QString *error)
{
    while (!m_writers.empty()) {
        const int index = m_writers.begin()->first;
        if (index > segmentIndex)
            break;
        if (!closeWriter(index, error))
            return false;
    }
    return true;
}

bool SegmentRouter::finishAll(bool allowPartialLastSegment, QString *error)
{
    bool ok = true;
    while (!m_writers.empty()) {
        const int index = m_writers.begin()->first;
        SegmentWriter *writer = m_writers.begin()->second.get();
        if (allowPartialLastSegment) {
            // Success: every still-open part must match its exact frame range.
            // Only the trailing part can be "partial" in the sense that it is
            // the one still being written when the encoder drained; it still
            // has to contain every frame of its timeline range.
            if (!validateWriter(index, writer, error)) {
                ok = false;
                writer->finish(nullptr);
                QFile::remove(segmentPath(m_partsDir, index));
            } else if (!writer->finish(error)) {
                ok = false;
                QFile::remove(segmentPath(m_partsDir, index));
            } else {
                m_bytes += writer->bytesWritten();
                m_lastCompleted = std::max(m_lastCompleted, index);
                ++m_completedCount;
            }
        } else {
            // Cancel/failure: close the handle and drop the unfinished parts.
            writer->finish(nullptr);
            QFile::remove(segmentPath(m_partsDir, index));
        }
        m_writers.erase(m_writers.begin());
    }
    if (ok && allowPartialLastSegment) {
        // A resume whose checkpoint already covered every segment routes no
        // packets at all: all parts are already complete on disk and the
        // assembler will rebuild the final file from them. Treat this as a
        // successful no-op. (A genuinely empty fresh export still fails in
        // Mp4Assembler when it opens the first missing part.)
        if (m_firstSegment < 0)
            return true;
        const int totalInThisRun = m_timing.segmentCount - m_firstSegment;
        if (m_lastCompleted != m_timing.segmentCount - 1
            || m_completedCount != totalInThisRun) {
            if (error)
                *error = QStringLiteral("导出分段不完整：已完成 %1/%2 段。")
                             .arg(completedSegments())
                             .arg(totalInThisRun);
            return false;
        }
    }
    return ok;
}

} // namespace VideoEditor
