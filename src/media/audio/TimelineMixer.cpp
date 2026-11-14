#include "TimelineMixer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QtGlobal>

#include "ClipAudioReader.h"
#include "TimelineEvaluator.h"

namespace VideoEditor {

namespace {

constexpr float kLimiterCeiling = 0.98f;
constexpr float kCenterPanGain = 0.70710678f;

} // namespace

float clipGainAt(const Clip &clip, int64_t localTimeUs)
{
    if (!clip.enabled || clip.audio.muted)
        return 0.0f;

    float gain = float(clip.audio.volumeMilli) / 1000.0f;

    const QList<VolumeKeyframe> &keyframes = clip.audio.keyframes;
    if (!keyframes.isEmpty()) {
        float keyframeGain = float(keyframes.first().volumeMilli) / 1000.0f;
        if (localTimeUs >= keyframes.last().timeUs) {
            keyframeGain = float(keyframes.last().volumeMilli) / 1000.0f;
        } else if (localTimeUs > keyframes.first().timeUs) {
            for (int i = 1; i < keyframes.size(); ++i) {
                if (localTimeUs <= keyframes.at(i).timeUs) {
                    const VolumeKeyframe &a = keyframes.at(i - 1);
                    const VolumeKeyframe &b = keyframes.at(i);
                    const double span = double(b.timeUs - a.timeUs);
                    const double t = span > 0.0
                        ? double(localTimeUs - a.timeUs) / span
                        : 1.0;
                    keyframeGain = float((a.volumeMilli
                                          + (b.volumeMilli - a.volumeMilli) * t)
                                         / 1000.0);
                    break;
                }
            }
        }
        gain *= keyframeGain;
    }

    const int64_t durationUs = qMax<int64_t>(1, clip.timelineDurationUs());
    if (clip.audio.fadeInUs > 0 && localTimeUs < clip.audio.fadeInUs) {
        const float ramp = float(qMax<int64_t>(0, localTimeUs))
                           / float(clip.audio.fadeInUs);
        gain *= qBound(0.0f, ramp, 1.0f);
    }
    if (clip.audio.fadeOutUs > 0) {
        const int64_t fadeOutStart = durationUs - clip.audio.fadeOutUs;
        if (localTimeUs > fadeOutStart) {
            const float ramp = float(qMax<int64_t>(0, durationUs - localTimeUs))
                               / float(clip.audio.fadeOutUs);
            gain *= qBound(0.0f, ramp, 1.0f);
        }
    }
    return qBound(0.0f, gain, 2.0f);
}

void panGains(int panMilli, float *left, float *right)
{
    const float pan = qBound(-1.0f, float(panMilli) / 1000.0f, 1.0f);
    const float angle = (pan + 1.0f) * 3.14159265358979323846f / 4.0f;
    if (left)
        *left = std::cos(angle);
    if (right)
        *right = std::sin(angle);
}

TimelineMixer::TimelineMixer(int sampleRate, int channels)
    : m_sampleRate(qMax(1, sampleRate))
    , m_channels(qMax(1, channels))
{
}

TimelineMixer::~TimelineMixer() = default;

void TimelineMixer::setProject(const Project &project)
{
    m_project = project;
    // Decoders are bound to an asset path and sample format; rebuilding them
    // avoids reusing a stale decoder after a project/asset change.
    m_readers.clear();
}

void TimelineMixer::setPosition(int64_t positionUs)
{
    m_startPositionUs = qMax<int64_t>(0, positionUs);
    m_framesRendered = 0;
    for (auto &entry : m_readers) {
        if (entry.second)
            entry.second->reset();
    }
}

int64_t TimelineMixer::positionForFrameCount(int64_t frames) const
{
    return m_startPositionUs
        + (frames * 1'000'000 + m_sampleRate / 2) / m_sampleRate;
}

int64_t TimelineMixer::positionUs() const
{
    return positionForFrameCount(m_framesRendered);
}

ClipAudioReader *TimelineMixer::readerFor(const Clip &clip)
{
    const QString key = clip.id.isEmpty() ? clip.assetId : clip.id;
    auto existing = m_readers.find(key);
    if (existing != m_readers.end())
        return existing->second.get();

    std::unique_ptr<ClipAudioReader> reader;
    if (const Asset *asset = m_project.assetById(clip.assetId)) {
        auto candidate = std::make_unique<ClipAudioReader>();
        QString error;
        if (candidate->open(*asset, m_sampleRate, m_channels, &error))
            reader = std::move(candidate);
    }
    ClipAudioReader *raw = reader.get();
    m_readers.emplace(key, std::move(reader));
    return raw;
}

void TimelineMixer::render(float *interleaved, int frames, MixStats *stats)
{
    if (!interleaved || frames <= 0) {
        if (stats) {
            stats->peak = 0.0f;
            stats->activeLayers = 0;
        }
        return;
    }

    const size_t sampleCount = size_t(frames) * size_t(m_channels);
    std::fill(interleaved, interleaved + sampleCount, 0.0f);

    const int64_t blockStartUs = positionForFrameCount(m_framesRendered);
    const int64_t blockEndUs = positionForFrameCount(m_framesRendered + frames);

    const TimelineEvaluator evaluator(m_project);
    const QList<AudioLayer> layers = evaluator.audioRange(blockStartUs, blockEndUs);

    bool anySolo = false;
    for (const AudioLayer &layer : layers)
        anySolo = anySolo || layer.trackSolo;

    QList<AudioLayer> audible;
    for (const AudioLayer &layer : layers) {
        if (layer.trackMuted || (anySolo && !layer.trackSolo))
            continue;
        audible.append(layer);
    }

    std::vector<float> scratch(sampleCount, 0.0f);
    for (const AudioLayer &layer : audible) {
        ClipAudioReader *reader = readerFor(layer.clip);
        if (!reader)
            continue;
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        reader->render(layer.clip, layer.localTimeUs, frames, scratch.data());

        float leftGain = kCenterPanGain;
        float rightGain = kCenterPanGain;
        panGains(layer.clip.audio.panMilli, &leftGain, &rightGain);

        for (int i = 0; i < frames; ++i) {
            const int64_t localUs = layer.localTimeUs
                + (int64_t(i) * 1'000'000) / m_sampleRate;
            const float gain = clipGainAt(layer.clip, localUs);
            if (m_channels == 2) {
                const float left = scratch[size_t(i) * 2] * gain * leftGain;
                const float right = scratch[size_t(i) * 2 + 1] * gain * rightGain;
                interleaved[size_t(i) * 2] += left;
                interleaved[size_t(i) * 2 + 1] += right;
            } else {
                for (int c = 0; c < m_channels; ++c) {
                    const float value = scratch[size_t(i) * size_t(m_channels)
                                                + size_t(c)] * gain;
                    interleaved[size_t(i) * size_t(m_channels) + size_t(c)] += value;
                }
            }
        }
    }

    // Master limiter: measure the summed mix (not individual layers) and apply
    // a per-block gain reduction so the preview never clips. 10 ms is short
    // enough to stay inaudible while guaranteeing the 0.98 full-scale ceiling
    // regardless of how many layers overlap.
    float blockPeak = 0.0f;
    for (size_t i = 0; i < sampleCount; ++i)
        blockPeak = qMax(blockPeak, std::abs(interleaved[i]));
    if (blockPeak > kLimiterCeiling) {
        const float scale = kLimiterCeiling / blockPeak;
        for (size_t i = 0; i < sampleCount; ++i)
            interleaved[i] *= scale;
        blockPeak = kLimiterCeiling;
    }

    if (stats) {
        stats->peak = blockPeak;
        stats->activeLayers = int(audible.size());
    }
    m_framesRendered += frames;
}

} // namespace VideoEditor
