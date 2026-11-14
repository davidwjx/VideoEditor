#include "ClipAudioReader.h"

#include <algorithm>
#include <cmath>

#include <QtGlobal>

#include "FfmpegAudioSource.h"
#include "SpeedProcessor.h"

namespace VideoEditor {

namespace {

int64_t sampleIndexForUs(int64_t us, int sampleRate)
{
    if (us <= 0 || sampleRate <= 0)
        return 0;
    return (us * sampleRate) / 1'000'000;
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

ClipAudioReader::ClipAudioReader() = default;
ClipAudioReader::~ClipAudioReader() = default;

bool ClipAudioReader::open(const Asset &asset, int sampleRate, int channels,
                           QString *error)
{
    m_source.reset();
    m_stretcher.reset();
    m_open = false;
    m_sampleRate = qMax(1, sampleRate);
    m_channels = qMax(1, channels);

    if (asset.path.isEmpty()) {
        setError(error, QStringLiteral("素材没有可读取的音频路径。"));
        return false;
    }

    m_source = FfmpegAudioSource::open(asset.path, m_sampleRate, m_channels, error);
    if (!m_source)
        return false;

    m_stretcher = std::make_unique<OlaTimeStretcher>(m_channels);
    m_open = true;
    return true;
}

void ClipAudioReader::reset()
{
    if (m_stretcher)
        m_stretcher->reset();
    m_sourceScratch.clear();
}

void ClipAudioReader::render(const Clip &clip, int64_t blockLocalUs, int frames,
                             float *out)
{
    if (!out || frames <= 0)
        return;
    std::fill(out, out + size_t(frames) * size_t(m_channels), 0.0f);
    if (!m_open || !m_source)
        return;

    const int64_t localUs = qMax<int64_t>(0, blockLocalUs);
    const int64_t sourceStartUs = sourceTimeAt(clip, clip.timelineStartUs + localUs);

    // The timeline block duration in integer microseconds. Absolute timeline
    // positions are used for every block, so no fractional time accumulates.
    const int64_t blockDurationUs = (int64_t(frames) * 1'000'000) / m_sampleRate;
    const int64_t sourceEndUs = sourceTimeAt(
        clip, clip.timelineStartUs + localUs + blockDurationUs);

    const int64_t startSample = sampleIndexForUs(sourceStartUs, m_sampleRate);
    const int64_t endSample = sampleIndexForUs(sourceEndUs, m_sampleRate);
    const int64_t sourceFrames = qMax<int64_t>(0, endSample - startSample);
    // Never decode past the clip's source-out point; the tail stays silent.
    const int64_t availableFrames = qMax<int64_t>(
        0, sampleIndexForUs(clip.sourceOutUs, m_sampleRate) - startSample);

    if (clip.speedMilli == kDefaultSpeedMilli) {
        const int readFrames = int(qMin<int64_t>(frames, availableFrames));
        if (readFrames > 0)
            m_source->readRange(sourceStartUs, readFrames, out, nullptr);
        return;
    }

    const double speed = double(clip.speedMilli) / 1000.0;

    if (!clip.preservePitch) {
        // Linear interpolation needs the two neighbours of the final sample.
        const int64_t span = qMin<int64_t>(
            qMax<int64_t>(sourceFrames, int64_t(std::ceil(frames * speed)) + 2),
            availableFrames);
        const int needed = int(qMax<int64_t>(0, span));
        if (needed <= 0)
            return;
        m_sourceScratch.resize(size_t(needed) * size_t(m_channels));
        if (!m_source->readRange(sourceStartUs, needed, m_sourceScratch.data(), nullptr))
            return;
        resampleLinear(m_sourceScratch.data(), needed, m_channels, speed, out, frames);
        return;
    }

    // Pitch-preserving path: push exactly the contiguous source span belonging
    // to this block. Consecutive blocks derive their span from absolute
    // integer timeline positions, so the OLA input never duplicates samples.
    if (sourceFrames <= 0 || availableFrames <= 0)
        return;
    const int64_t pushFrames = qMin(sourceFrames, availableFrames);
    m_sourceScratch.resize(size_t(pushFrames) * size_t(m_channels));
    if (!m_source->readRange(sourceStartUs, int(pushFrames),
                             m_sourceScratch.data(), nullptr)) {
        return;
    }
    m_stretcher->push(m_sourceScratch.data(), int(pushFrames));
    // pull() can return fewer frames while the window warms up; the tail stays
    // silent, which is inaudible at 10 ms block boundaries.
    m_stretcher->pull(out, frames, speed);
}

} // namespace VideoEditor
