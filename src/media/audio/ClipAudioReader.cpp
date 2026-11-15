#include "ClipAudioReader.h"

#include <algorithm>
#include <cmath>

#include <QtGlobal>

#include "FfmpegAudioSource.h"
#include "SpeedProcessor.h"

namespace VideoEditor {

ClipAudioReader::ClipAudioReader() = default;
ClipAudioReader::~ClipAudioReader() = default;

int64_t ClipAudioReader::sampleIndexForUs(int64_t us) const
{
    if (us <= 0 || m_sampleRate <= 0)
        return 0;
    return (us * m_sampleRate) / 1'000'000;
}

int64_t ClipAudioReader::usForSample(int64_t sample) const
{
    if (sample <= 0)
        return 0;
    // Round up so re-reading from a sample cursor resolves back to the same
    // sample index (readRange floors microseconds to a sample).
    return (sample * 1'000'000 + m_sampleRate - 1) / m_sampleRate;
}

void ClipAudioReader::applyMonoTrim(float *out, int frames) const
{
    if (!m_monoToStereo || !out || frames <= 0)
        return;
    const size_t count = size_t(frames) * size_t(m_channels);
    for (size_t i = 0; i < count; ++i)
        out[i] *= kMonoDualMonoGain;
}

bool ClipAudioReader::open(const Asset &asset, int sampleRate, int channels,
                           QString *error)
{
    m_source.reset();
    m_stretcher.reset();
    m_open = false;
    m_monoToStereo = false;
    m_sourceChannels = 0;
    m_sampleRate = qMax(1, sampleRate);
    m_channels = qMax(1, channels);

    if (asset.path.isEmpty()) {
        if (error)
            *error = QStringLiteral("素材没有可读取的音频路径。");
        return false;
    }

    m_source = FfmpegAudioSource::open(asset.path, m_sampleRate, m_channels, error);
    if (!m_source)
        return false;

    m_sourceChannels = m_source->sourceChannels();
    m_monoToStereo = m_sourceChannels == 1 && m_channels == 2;
    m_stretcher = std::make_unique<OlaTimeStretcher>(m_channels);
    m_olaPrimed = false;
    m_olaNextSample = 0;
    m_olaPadRemaining = 0;
    m_open = true;
    return true;
}

void ClipAudioReader::reset()
{
    if (m_stretcher)
        m_stretcher->reset();
    m_olaPrimed = false;
    m_olaNextSample = 0;
    m_olaPadRemaining = 0;
    m_sourceScratch.clear();
    m_olaScratch.clear();
    m_olaCarry.clear();
    m_olaCarryOffset = 0;
}

void ClipAudioReader::render(const Clip &clip, int64_t blockLocalUs, int frames,
                             float *out)
{
    if (!out || frames <= 0)
        return;
    std::fill(out, out + size_t(frames) * size_t(m_channels), 0.0f);
    if (!m_open || !m_source || !m_stretcher)
        return;

    const int speedMilli = qBound(kMinSpeedMilli, clip.speedMilli, kMaxSpeedMilli);
    const int64_t localUs = qMax<int64_t>(0, blockLocalUs);
    const int64_t sourceStartUs = sourceTimeAt(clip, clip.timelineStartUs + localUs);
    // The timeline block duration in integer microseconds. Absolute timeline
    // positions are used for every block, so no fractional time accumulates.
    const int64_t blockDurationUs = (int64_t(frames) * 1'000'000) / m_sampleRate;
    const int64_t sourceEndUs = sourceTimeAt(
        clip, clip.timelineStartUs + localUs + blockDurationUs);

    const int64_t startSample = sampleIndexForUs(sourceStartUs);
    const int64_t endSample = sampleIndexForUs(sourceEndUs);
    const int64_t sourceFrames = qMax<int64_t>(0, endSample - startSample);
    const int64_t sourceOutSample = sampleIndexForUs(clip.sourceOutUs);
    const int64_t availableFrames = qMax<int64_t>(0, sourceOutSample - startSample);

    if (speedMilli == kDefaultSpeedMilli) {
        const int readFrames = int(qMin<int64_t>(frames, availableFrames));
        if (readFrames > 0)
            m_source->readRange(sourceStartUs, readFrames, out, nullptr);
        applyMonoTrim(out, frames);
        return;
    }

    const double speed = double(speedMilli) / 1000.0;

    if (!clip.preservePitch) {
        // Linear interpolation needs the two neighbours of the final sample.
        const int64_t desired = qMax<int64_t>(
            sourceFrames, int64_t(std::ceil(double(frames) * speed)) + 2);
        const int64_t needed = qMin(desired, availableFrames);
        if (needed > 0) {
            m_sourceScratch.resize(size_t(needed) * size_t(m_channels));
            if (m_source->readRange(sourceStartUs, int(needed),
                                    m_sourceScratch.data(), nullptr)) {
                // Only outputs whose interpolation neighbours exist in the
                // available source span are valid; the rest stays zero instead
                // of repeating the last source sample (which would emit DC).
                const int64_t validFrames =
                    int64_t(std::floor(double(needed - 1) / speed)) + 1;
                const int outFrames = int(qMin<int64_t>(
                    int64_t(frames), qMax<int64_t>(0, validFrames)));
                if (outFrames > 0) {
                    resampleLinear(m_sourceScratch.data(), int(needed),
                                   m_channels, speed, out, outFrames);
                }
            }
        }
        applyMonoTrim(out, frames);
        return;
    }

    // Pitch-preserving OLA path. First drain output carried from the previous
    // block (the stretcher emits whole 10 ms hops, while a device block can be
    // any length).
    const int framesPerSample = m_channels;
    int written = 0;
    if (m_olaCarryOffset < int(m_olaCarry.size())) {
        const int carriedFrames =
            int(m_olaCarry.size()) / framesPerSample - m_olaCarryOffset;
        const int take = std::min(frames, carriedFrames);
        std::copy_n(m_olaCarry.data()
                        + size_t(m_olaCarryOffset) * size_t(framesPerSample),
                    size_t(take) * size_t(framesPerSample), out);
        written = take;
        m_olaCarryOffset += take;
        if (m_olaCarryOffset >= int(m_olaCarry.size()) / framesPerSample) {
            m_olaCarry.clear();
            m_olaCarryOffset = 0;
        }
    }
    if (written >= frames) {
        applyMonoTrim(out, frames);
        return;
    }

    const int remaining = frames - written;
    const int outputHops = (remaining + OlaTimeStretcher::kHopFrames - 1)
                           / OlaTimeStretcher::kHopFrames;
    const int analysisHop = std::max(
        1, int(std::lround(double(OlaTimeStretcher::kHopFrames) * speed)));

    int preRollHops = 0;
    const bool priming = !m_olaPrimed;
    if (priming) {
        // Prime with whole analysis windows before the requested start so the
        // first emitted hop is already overlap-added at sourceStartUs. Reading
        // before sourceInUs is allowed (source media is read-only).
        preRollHops = std::max(
            0, (OlaTimeStretcher::kWindowFrames + analysisHop - 1) / analysisHop - 1);
        m_olaNextSample = startSample - int64_t(preRollHops) * analysisHop;
        m_olaPadRemaining = OlaTimeStretcher::kWindowFrames;
        m_olaPrimed = true;
    }

    const int hopsToPull = outputHops + (priming ? preRollHops : 0);
    const int64_t required = int64_t(OlaTimeStretcher::kWindowFrames)
                             + int64_t(analysisHop) * hopsToPull;

    std::vector<float> zeros;
    auto pushZeros = [&](int count) {
        if (count <= 0)
            return;
        zeros.assign(size_t(count) * size_t(m_channels), 0.0f);
        m_stretcher->push(zeros.data(), count);
    };

    while (m_stretcher->bufferedFrames() < required) {
        const int64_t deficit = required - m_stretcher->bufferedFrames();
        if (m_olaNextSample < 0) {
            // Silence before the start of the media file.
            const int count = int(qMin<int64_t>(deficit, -m_olaNextSample));
            pushZeros(count);
            m_olaNextSample += count;
            continue;
        }
        if (m_olaNextSample < sourceOutSample) {
            const int count = int(qMin<int64_t>(
                deficit, sourceOutSample - m_olaNextSample));
            m_sourceScratch.resize(size_t(count) * size_t(m_channels));
            if (!m_source->readRange(usForSample(m_olaNextSample), count,
                                     m_sourceScratch.data(), nullptr)) {
                break;
            }
            m_stretcher->push(m_sourceScratch.data(), count);
            m_olaNextSample += count;
            continue;
        }
        // Source exhausted: pad enough zeros (bounded by one window) to flush
        // the final overlapping windows, so the time-stretched tail is emitted.
        if (m_olaPadRemaining <= 0)
            break;
        const int count = int(qMin<int64_t>(deficit, m_olaPadRemaining));
        pushZeros(count);
        m_olaPadRemaining -= count;
    }

    const int pullFrames = hopsToPull * OlaTimeStretcher::kHopFrames;
    m_olaScratch.resize(size_t(pullFrames) * size_t(m_channels));
    const int produced = m_stretcher->pull(m_olaScratch.data(), pullFrames, speed);
    const int discard = priming
        ? preRollHops * OlaTimeStretcher::kHopFrames
        : 0;
    const int available = std::max(0, produced - discard);
    const int copyFrames = std::min(available, remaining);
    if (copyFrames > 0) {
        std::copy_n(m_olaScratch.data() + size_t(discard) * size_t(m_channels),
                    size_t(copyFrames) * size_t(m_channels),
                    out + size_t(written) * size_t(m_channels));
    }
    m_olaCarry.clear();
    m_olaCarryOffset = 0;
    if (available > copyFrames) {
        const int leftover = available - copyFrames;
        m_olaCarry.resize(size_t(leftover) * size_t(m_channels));
        std::copy_n(
            m_olaScratch.data()
                + size_t(discard + copyFrames) * size_t(m_channels),
            size_t(leftover) * size_t(m_channels), m_olaCarry.data());
    }
    applyMonoTrim(out, frames);
}

} // namespace VideoEditor
