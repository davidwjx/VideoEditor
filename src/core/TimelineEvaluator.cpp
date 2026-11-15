#include "TimelineEvaluator.h"

#include <algorithm>

#include <QtGlobal>

namespace VideoEditor {

namespace {

bool covers(const Clip &clip, int64_t timeUs)
{
    return timeUs >= clip.timelineStartUs && timeUs < clip.timelineEndUs();
}

} // namespace

const SubtitleStyle &TimelineEvaluator::styleFor(const QString &styleId) const
{
    for (const SubtitleStyle &style : m_project.subtitleStyles) {
        if (style.id == styleId)
            return style;
    }
    // A project may have been created before a preset was added, or a cue may
    // reference a builtin id that was stripped while loading. Falling back to
    // the builtin implementation makes presets render correct immediately
    // instead of silently becoming the default style.
    static const QList<SubtitleStyle> builtins = builtinSubtitleStyles();
    for (const SubtitleStyle &style : builtins) {
        if (style.id == styleId)
            return style;
    }
    for (const SubtitleStyle &style : m_project.subtitleStyles) {
        if (style.id == QStringLiteral("default"))
            return style;
    }
    for (const SubtitleStyle &style : builtins) {
        if (style.id == QStringLiteral("default"))
            return style;
    }
    static const SubtitleStyle fallback;
    return fallback;
}

FrameComposition TimelineEvaluator::frameAt(int64_t timelineUs) const
{
    FrameComposition frame;
    frame.timelineUs = timelineUs;

    for (int trackIndex = 0; trackIndex < m_project.tracks.size(); ++trackIndex) {
        const Track &track = m_project.tracks.at(trackIndex);
        if (!track.enabled)
            continue;

        if (track.type == TrackType::Video) {
            // Overlaps are legal in the model; the last clip in track order wins.
            const Clip *active = nullptr;
            for (const Clip &clip : track.clips) {
                if (clip.enabled && covers(clip, timelineUs))
                    active = &clip;
            }
            if (!active)
                continue;
            VideoLayer layer;
            layer.trackId = track.id;
            layer.clipId = active->id;
            layer.assetId = active->assetId;
            layer.trackIndex = trackIndex;
            layer.localTimeUs = timelineUs - active->timelineStartUs;
            layer.sourceTimeUs = sourceTimeAt(*active, timelineUs);
            layer.transform = active->transform;
            frame.video.append(layer);
        } else if (track.type == TrackType::Subtitle) {
            for (const SubtitleCue &cue : track.cues) {
                if (!cue.enabled || timelineUs < cue.startUs || timelineUs >= cue.endUs)
                    continue;
                SubtitleLayer layer;
                layer.trackId = track.id;
                layer.cueId = cue.id;
                layer.text = cue.text;
                layer.style = styleFor(cue.styleId);
                layer.positionXPermille = cue.positionXPermille;
                layer.positionYPermille = cue.positionYPermille;
                layer.localTimeUs = timelineUs - cue.startUs;
                frame.subtitles.append(layer);
            }
        }
    }
    return frame;
}

QList<AudioLayer> TimelineEvaluator::audioRange(int64_t startUs, int64_t endUs) const
{
    QList<AudioLayer> layers;
    if (endUs <= startUs)
        return layers;

    for (int trackIndex = 0; trackIndex < m_project.tracks.size(); ++trackIndex) {
        const Track &track = m_project.tracks.at(trackIndex);
        if (!track.enabled || track.type != TrackType::Audio)
            continue;
        for (const Clip &clip : track.clips) {
            if (!clip.enabled)
                continue;
            if (clip.timelineEndUs() <= startUs || clip.timelineStartUs >= endUs)
                continue;
            const int64_t localUs = qMax<int64_t>(0, startUs - clip.timelineStartUs);
            AudioLayer layer;
            layer.trackId = track.id;
            layer.clipId = clip.id;
            layer.trackIndex = trackIndex;
            layer.localTimeUs = localUs;
            layer.sourceTimeUs = sourceTimeAt(clip, startUs);
            layer.trackMuted = track.muted;
            layer.trackSolo = track.solo;
            layer.clip = clip;
            layers.append(layer);
        }
    }
    return layers;
}

bool TimelineEvaluator::anySolo() const
{
    for (const Track &track : m_project.tracks) {
        if (track.enabled && track.type == TrackType::Audio && track.solo)
            return true;
    }
    return false;
}

int64_t TimelineEvaluator::durationUs() const
{
    int64_t duration = 0;
    for (const Track &track : m_project.tracks) {
        for (const Clip &clip : track.clips)
            duration = qMax(duration, clip.timelineEndUs());
        for (const SubtitleCue &cue : track.cues)
            duration = qMax(duration, cue.endUs);
    }
    return duration;
}

} // namespace VideoEditor
