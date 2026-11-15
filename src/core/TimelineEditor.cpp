#include "TimelineEditor.h"

#include <algorithm>
#include <climits>
#include <utility>
#include <QHash>
#include <QSet>
#include <QUuid>

#include "ProjectDocument.h"
#include "Srt.h"
#include "commands/RemovalLayerCommand.h"
#include "commands/TimelineEditCommand.h"
#include "commands/TrackListCommand.h"

namespace VideoEditor {

namespace {

int64_t oneFrameUs(const ProjectDocument &document)
{
    const Rational fps = document.project().timeline.fps.isValid()
                             ? document.project().timeline.fps
                             : Rational{30, 1};
    return framesToMicros(1, fps);
}

// Smallest source span whose timeline duration is at least timelineUs after
// unscaleBySpeed's half-up rounding. Without this, trimming a slowed clip can
// leave a sub-frame (or zero-length) clip.
int64_t minSourceForTimeline(int64_t timelineUs, int speedMilli)
{
    const int64_t speed = qBound<int64_t>(int64_t(kMinSpeedMilli),
                                          int64_t(speedMilli),
                                          int64_t(kMaxSpeedMilli));
    const int64_t numerator = timelineUs * speed - speed / 2;
    if (numerator <= 0)
        return 1;
    return qMax<int64_t>(1, (numerator + 999) / 1000);
}

int64_t signedScale(int64_t timelineDelta, int speedMilli)
{
    if (timelineDelta == 0)
        return 0;
    return timelineDelta > 0 ? scaleBySpeed(timelineDelta, speedMilli)
                             : -scaleBySpeed(-timelineDelta, speedMilli);
}

int64_t signedUnscale(int64_t sourceDelta, int speedMilli)
{
    if (sourceDelta == 0)
        return 0;
    return sourceDelta > 0 ? unscaleBySpeed(sourceDelta, speedMilli)
                           : -unscaleBySpeed(-sourceDelta, speedMilli);
}

TrackEdit snapshotTrack(const Track &track)
{
    TrackEdit edit;
    edit.trackId = track.id;
    edit.beforeClips = track.clips;
    edit.afterClips = track.clips;
    edit.beforeCues = track.cues;
    edit.afterCues = track.cues;
    return edit;
}

bool isGroupClip(const QString &trackId, const QString &clipId,
                 const QList<ClipLocator> &group)
{
    for (const ClipLocator &locator : group) {
        if (locator.trackId == trackId && locator.clipId == clipId)
            return true;
    }
    return false;
}

} // namespace

TimelineEditor::TimelineEditor(ProjectDocument &document)
    : m_document(document)
{
}

QList<ClipLocator> TimelineEditor::linkGroup(const QString &trackId,
                                             const QString &clipId) const
{
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return {};
    if (clip->linkId.isEmpty())
        return {ClipLocator{trackId, clipId}};

    QList<ClipLocator> group;
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &candidate : track.clips) {
            if (candidate.linkId == clip->linkId)
                group.append(ClipLocator{track.id, candidate.id});
        }
    }
    return group;
}

const Clip *TimelineEditor::findClip(const QString &trackId,
                                     const QString &clipId) const
{
    const Track *track = m_document.trackById(trackId);
    if (!track)
        return nullptr;
    for (const Clip &clip : track->clips) {
        if (clip.id == clipId)
            return &clip;
    }
    return nullptr;
}

bool TimelineEditor::groupLocked(const QList<ClipLocator> &group) const
{
    for (const ClipLocator &locator : group) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track || track->locked)
            return true;
    }
    return false;
}

bool TimelineEditor::groupSpeed(const QList<ClipLocator> &group,
                                int *speedMilli) const
{
    bool found = false;
    int speed = 0;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return false;
        if (!found) {
            speed = clip->speedMilli;
            found = true;
        } else if (clip->speedMilli != speed) {
            return false;
        }
    }
    if (!found)
        return false;
    *speedMilli = speed;
    return true;
}

void TimelineEditor::pushEdits(const QString &text, QList<TrackEdit> edits,
                               const QList<RemovalLayer> &addedLayers,
                               const QList<RemovalLayer> &removedLayers)
{
    if (edits.isEmpty() && addedLayers.isEmpty() && removedLayers.isEmpty())
        return;

    QUndoStack *stack = m_document.undoStack();
    if (addedLayers.isEmpty() && removedLayers.isEmpty()) {
        stack->push(new TimelineEditCommand(&m_document, text, std::move(edits)));
        return;
    }

    stack->beginMacro(text);
    if (!edits.isEmpty()) {
        stack->push(new TimelineEditCommand(&m_document, text,
                                            std::move(edits)));
    }
    for (const RemovalLayer &layer : removedLayers) {
        RemovalLayerEdit edit;
        edit.clipId = layer.clipId;
        edit.before = layer;
        edit.after = std::nullopt;
        stack->push(new RemovalLayerCommand(&m_document, text, edit));
    }
    for (const RemovalLayer &layer : addedLayers) {
        RemovalLayerEdit edit;
        edit.clipId = layer.clipId;
        edit.after = layer;
        stack->push(new RemovalLayerCommand(&m_document, text, edit));
    }
    stack->endMacro();
}

QList<RemovalLayer> TimelineEditor::removalLayersForClips(
    const QList<ClipLocator> &locators) const
{
    QList<RemovalLayer> layers;
    QSet<QString> seen;
    for (const ClipLocator &locator : locators) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip || clip->removalLayerId.isEmpty()
            || seen.contains(clip->removalLayerId)) {
            continue;
        }
        if (const RemovalLayer *layer =
                m_document.removalLayerById(clip->removalLayerId)) {
            seen.insert(layer->id);
            layers.append(*layer);
        }
    }
    return layers;
}

QList<RemovalLayer> TimelineEditor::removalLayersForTrack(
    const Track &track) const
{
    QList<RemovalLayer> layers;
    QSet<QString> seen;
    for (const Clip &clip : track.clips) {
        if (clip.removalLayerId.isEmpty()
            || seen.contains(clip.removalLayerId)) {
            continue;
        }
        if (const RemovalLayer *layer =
                m_document.removalLayerById(clip.removalLayerId)) {
            seen.insert(layer->id);
            layers.append(*layer);
        }
    }
    return layers;
}

QList<TrackEdit> TimelineEditor::editsForClips(
    const QList<ClipLocator> &locators,
    const std::function<void(Clip &)> &mutate) const
{
    QList<TrackEdit> edits;
    QSet<QString> seenTracks;
    for (const ClipLocator &locator : locators) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track)
            continue;
        if (!seenTracks.contains(locator.trackId)) {
            edits.append(snapshotTrack(*track));
            seenTracks.insert(locator.trackId);
        }
    }
    for (TrackEdit &edit : edits) {
        for (Clip &clip : edit.afterClips) {
            for (const ClipLocator &locator : locators) {
                if (locator.trackId == edit.trackId && locator.clipId == clip.id) {
                    mutate(clip);
                    break;
                }
            }
        }
    }
    edits.removeIf([](const TrackEdit &edit) {
        return edit.beforeClips == edit.afterClips
            && edit.beforeCues == edit.afterCues;
    });
    return edits;
}

QStringList TimelineEditor::addClips(
    const QList<QPair<QString, Clip>> &trackAndClip)
{
    QList<TrackEdit> edits;
    QStringList addedIds;
    QHash<QString, int> editIndexByTrack;
    QSet<QString> usedIds;
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &clip : track.clips)
            usedIds.insert(clip.id);
    }

    for (const auto &entry : trackAndClip) {
        const QString &trackId = entry.first;
        Clip clip = entry.second;
        const Track *track = m_document.trackById(trackId);
        if (!track || track->type == TrackType::Subtitle || track->locked)
            continue;
        if (clip.assetId.isEmpty())
            continue;
        if (clip.sourceInUs < 0 || clip.sourceOutUs <= clip.sourceInUs)
            continue;
        if (const Asset *asset = m_document.project().assetById(clip.assetId)) {
            if (asset->info.durationUs > 0
                && clip.sourceOutUs > asset->info.durationUs) {
                continue;
            }
        }
        if (clip.id.isEmpty())
            clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        else if (usedIds.contains(clip.id))
            continue;
        usedIds.insert(clip.id);

        clip.speedMilli = qBound(kMinSpeedMilli, clip.speedMilli, kMaxSpeedMilli);
        clip.timelineStartUs = qMax<int64_t>(0, clip.timelineStartUs);

        if (!editIndexByTrack.contains(trackId)) {
            TrackEdit edit = snapshotTrack(*track);
            editIndexByTrack.insert(trackId, edits.size());
            edits.append(edit);
        }
        edits[editIndexByTrack.value(trackId)].afterClips.append(clip);
        addedIds.append(clip.id);
    }
    pushEdits(QStringLiteral("添加片段"), edits);
    return addedIds;
}

QString TimelineEditor::addClip(const QString &trackId, Clip clip)
{
    const QStringList ids = addClips({{trackId, clip}});
    return ids.isEmpty() ? QString() : ids.first();
}

void TimelineEditor::moveClip(const QString &trackId, const QString &clipId,
                              int64_t newStartUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;

    int64_t delta = newStartUs - clicked->timelineStartUs;
    if (delta == 0)
        return;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        delta = qMax(delta, -clip->timelineStartUs);
    }
    if (delta == 0)
        return;
    pushEdits(QStringLiteral("移动片段"),
              editsForClips(group, [delta](Clip &clip) {
                  clip.timelineStartUs += delta;
              }));
}

void TimelineEditor::trimStart(const QString &trackId, const QString &clipId,
                               int64_t newStartUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;
    int speed = kDefaultSpeedMilli;
    if (!groupSpeed(group, &speed))
        return;

    const int64_t minUs = oneFrameUs(m_document);
    const int64_t minSource = minSourceForTimeline(minUs, speed);
    newStartUs = qMax<int64_t>(0, newStartUs);
    const int64_t requestedTimelineDelta =
        newStartUs - clicked->timelineStartUs;
    if (requestedTimelineDelta == 0)
        return;
    const int64_t requestedSourceDelta =
        signedScale(requestedTimelineDelta, speed);

    // All group members share one source delta and one timeline delta so they
    // cannot diverge. Bounds come from every member.
    int64_t sourceLower = INT64_MIN;   // most extension allowed (>= -sourceIn)
    int64_t sourceUpper = INT64_MAX;   // most trim-in allowed (one frame left)
    int64_t timelineLower = INT64_MIN; // cannot move a start below zero
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        // Both bounds are deltas: extension cannot move sourceIn below 0,
        // trim-in cannot consume the last frame of the remaining source span.
        sourceLower = qMax(sourceLower, -clip->sourceInUs);
        sourceUpper = qMin(sourceUpper,
                           clip->sourceOutUs - clip->sourceInUs - minSource);
        timelineLower = qMax(timelineLower, -clip->timelineStartUs);
    }
    if (sourceLower > sourceUpper)
        return;
    if (requestedSourceDelta > 0 && sourceUpper < 0)
        return; // Already sub-frame and no trim-in is possible.
    const int64_t upperForRequest =
        requestedSourceDelta > 0 ? sourceUpper
                                 : qMin<int64_t>(sourceUpper, 0);
    if (sourceLower > upperForRequest)
        return;

    int64_t sourceDelta =
        qBound<int64_t>(sourceLower, requestedSourceDelta, upperForRequest);
    if (sourceDelta == 0)
        return;
    int64_t timelineDelta = signedUnscale(sourceDelta, speed);
    if (timelineDelta < timelineLower) {
        timelineDelta = timelineLower;
        const int64_t adjusted = signedScale(timelineDelta, speed);
        if (adjusted < sourceLower || adjusted > upperForRequest || adjusted == 0)
            return;
        sourceDelta = adjusted;
    }
    if (timelineDelta == 0 && sourceDelta == 0)
        return;

    pushEdits(QStringLiteral("裁切片段入点"),
              editsForClips(group, [sourceDelta, timelineDelta](Clip &clip) {
                  clip.sourceInUs += sourceDelta;
                  clip.timelineStartUs += timelineDelta;
              }));
}

void TimelineEditor::trimEnd(const QString &trackId, const QString &clipId,
                             int64_t newEndUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;
    int speed = kDefaultSpeedMilli;
    if (!groupSpeed(group, &speed))
        return;

    const int64_t minUs = oneFrameUs(m_document);
    const int64_t minSource = minSourceForTimeline(minUs, speed);
    newEndUs = qMax(clicked->timelineStartUs + minUs, newEndUs);
    const int64_t wantedDuration = newEndUs - clicked->timelineStartUs;

    int64_t maxSpan = INT64_MAX;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        const Asset *asset = m_document.project().assetById(clip->assetId);
        if (!asset || asset->info.durationUs <= 0)
            return; // Unknown duration: cannot enforce a valid range.
        maxSpan = qMin(maxSpan, asset->info.durationUs - clip->sourceInUs);
    }
    if (maxSpan < minSource)
        return; // No range can satisfy the one-frame minimum.

    const int64_t span =
        qBound<int64_t>(minSource, scaleBySpeed(wantedDuration, speed), maxSpan);
    bool changed = false;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        if (clip->sourceOutUs != clip->sourceInUs + span)
            changed = true;
    }
    if (!changed)
        return;

    pushEdits(QStringLiteral("裁切片段出点"),
              editsForClips(group, [span](Clip &clip) {
                  clip.sourceOutUs = clip.sourceInUs + span;
              }));
}

void TimelineEditor::splitClip(const QString &trackId, const QString &clipId,
                               int64_t timelineUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;
    int speed = kDefaultSpeedMilli;
    if (!groupSpeed(group, &speed))
        return;
    if (timelineUs <= clicked->timelineStartUs
        || timelineUs >= clicked->timelineEndUs()) {
        return;
    }

    const int64_t minSource =
        minSourceForTimeline(oneFrameUs(m_document), speed);
    // Validate the whole group first: one member that cannot be split rejects
    // the edit instead of leaving the group partially split.
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        const int64_t cut = sourceTimeAt(*clip, timelineUs);
        if (cut <= clip->sourceInUs || cut >= clip->sourceOutUs)
            return;
        if (cut - clip->sourceInUs < minSource
            || clip->sourceOutUs - cut < minSource) {
            return;
        }
    }

    const QString rightLinkId =
        clicked->linkId.isEmpty() ? QString() : uniqueLinkId();

    QList<TrackEdit> edits;
    QHash<QString, int> editIndexByTrack;
    QList<RemovalLayer> addedLayers;
    for (const ClipLocator &locator : group) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track)
            return;
        if (!editIndexByTrack.contains(locator.trackId)) {
            editIndexByTrack.insert(locator.trackId, edits.size());
            edits.append(snapshotTrack(*track));
        }
        TrackEdit &target = edits[editIndexByTrack.value(locator.trackId)];
        for (int i = 0; i < target.afterClips.size(); ++i) {
            Clip &left = target.afterClips[i];
            if (left.id != locator.clipId)
                continue;
            const int64_t sourceAtCut = sourceTimeAt(left, timelineUs);
            Clip right = left;
            right.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            right.sourceInUs = sourceAtCut;
            right.timelineStartUs = timelineUs;
            if (!right.linkId.isEmpty())
                right.linkId = rightLinkId;
            // Two clips must never share one removal layer. Give the right
            // half its own copy; the left half keeps the original.
            if (!left.removalLayerId.isEmpty()) {
                if (const RemovalLayer *source =
                        m_document.removalLayerById(left.removalLayerId)) {
                    RemovalLayer clone = *source;
                    clone.id = QUuid::createUuid().toString(
                        QUuid::WithoutBraces);
                    clone.id.prepend(QStringLiteral("rl-"));
                    clone.clipId = right.id;
                    clone.sourceInUs = right.sourceInUs;
                    clone.sourceOutUs = right.sourceOutUs;
                    right.removalLayerId = clone.id;
                    addedLayers.append(clone);
                } else {
                    right.removalLayerId.clear();
                }
            }
            left.sourceOutUs = sourceAtCut;
            target.afterClips.insert(i + 1, right);
            break;
        }
    }
    pushEdits(QStringLiteral("分割片段"), edits, addedLayers, {});
}

void TimelineEditor::deleteClip(const QString &trackId, const QString &clipId,
                                bool ripple)
{
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;

    // A deleted clip's removal layer must go in the same undo macro, or a
    // save would contain a layer that references a missing video clip and the
    // next load/recovery would reject the file.
    const QList<RemovalLayer> removedLayers = removalLayersForClips(group);

    if (!ripple) {
        QList<TrackEdit> edits;
        QHash<QString, int> editIndexByTrack;
        for (const ClipLocator &locator : group) {
            const Track *track = m_document.trackById(locator.trackId);
            if (!track)
                return;
            int index = -1;
            if (!editIndexByTrack.contains(locator.trackId)) {
                index = edits.size();
                editIndexByTrack.insert(locator.trackId, index);
                edits.append(snapshotTrack(*track));
            } else {
                index = editIndexByTrack.value(locator.trackId);
            }
            TrackEdit &edit = edits[index];
            for (int i = edit.afterClips.size() - 1; i >= 0; --i) {
                if (isGroupClip(locator.trackId, edit.afterClips.at(i).id, group))
                    edit.afterClips.removeAt(i);
            }
        }
        edits.removeIf([](const TrackEdit &edit) {
            return edit.beforeClips == edit.afterClips;
        });
        pushEdits(QStringLiteral("删除片段"), edits, {}, removedLayers);
        return;
    }

    // Ripple: the removed interval is the union of the whole linked group.
    int64_t removedStart = INT64_MAX;
    int64_t removedEnd = INT64_MIN;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        removedStart = qMin(removedStart, clip->timelineStartUs);
        removedEnd = qMax(removedEnd, clip->timelineEndUs());
    }
    if (removedStart >= removedEnd)
        return;
    const int64_t removedDuration = removedEnd - removedStart;

    // Shift later clips on every track by the same amount so linked pairs that
    // are not part of the deleted group stay in sync.
    QList<TrackEdit> edits;
    for (const Track &track : m_document.project().tracks) {
        bool touches = false;
        for (const Clip &clip : track.clips) {
            if (isGroupClip(track.id, clip.id, group)
                || clip.timelineStartUs >= removedEnd) {
                touches = true;
                break;
            }
        }
        if (!touches)
            continue;
        if (track.locked)
            return;
        TrackEdit edit = snapshotTrack(track);
        edit.afterClips.clear();
        for (const Clip &clip : track.clips) {
            if (isGroupClip(track.id, clip.id, group))
                continue;
            Clip adjusted = clip;
            if (clip.timelineStartUs >= removedEnd)
                adjusted.timelineStartUs -= removedDuration;
            edit.afterClips.append(adjusted);
        }
        if (edit.afterClips != edit.beforeClips)
            edits.append(edit);
    }
    pushEdits(QStringLiteral("波纹删除"), edits, {}, removedLayers);
}

void TimelineEditor::setClipSpeed(const QString &trackId, const QString &clipId,
                                  int speedMilli, bool preservePitch)
{
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;
    speedMilli = qBound(kMinSpeedMilli, speedMilli, kMaxSpeedMilli);
    pushEdits(QStringLiteral("设置片段变速"),
              editsForClips(group, [speedMilli, preservePitch](Clip &clip) {
                  clip.speedMilli = speedMilli;
                  clip.preservePitch = preservePitch;
              }));
}

void TimelineEditor::setClipAudio(const QString &trackId, const QString &clipId,
                                  const AudioParams &params)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked || !findClip(trackId, clipId))
        return;
    pushEdits(QStringLiteral("调整音频"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [params](Clip &clip) { clip.audio = params; }));
}

void TimelineEditor::setClipTransform(const QString &trackId,
                                      const QString &clipId,
                                      const Transform &transform)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked || !findClip(trackId, clipId))
        return;
    pushEdits(QStringLiteral("调整画面"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [transform](Clip &clip) { clip.transform = transform; }));
}

void TimelineEditor::setClipEnabled(const QString &trackId,
                                    const QString &clipId, bool enabled)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked || !findClip(trackId, clipId))
        return;
    pushEdits(enabled ? QStringLiteral("启用片段") : QStringLiteral("禁用片段"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [enabled](Clip &clip) { clip.enabled = enabled; }));
}

void TimelineEditor::linkClips(const QList<ClipLocator> &clips)
{
    if (clips.size() < 2)
        return;
    bool found = false;
    int64_t startUs = 0;
    int speedMilli = kDefaultSpeedMilli;
    for (const ClipLocator &locator : clips) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        const Track *track = m_document.trackById(locator.trackId);
        if (!clip || !track || track->locked)
            return;
        if (clip->sourceOutUs <= clip->sourceInUs
            || clip->timelineStartUs < 0) {
            return;
        }
        if (!found) {
            startUs = clip->timelineStartUs;
            speedMilli = qBound(kMinSpeedMilli, clip->speedMilli, kMaxSpeedMilli);
            found = true;
        } else {
            startUs = qMin(startUs, clip->timelineStartUs);
        }
    }
    if (!found)
        return;

    const QString linkId = uniqueLinkId();
    pushEdits(QStringLiteral("链接片段"),
              editsForClips(clips, [linkId, startUs, speedMilli](Clip &clip) {
                  clip.linkId = linkId;
                  clip.timelineStartUs = startUs;
                  clip.speedMilli = speedMilli;
              }));
}

void TimelineEditor::unlinkClip(const QString &trackId, const QString &clipId)
{
    const Clip *clip = findClip(trackId, clipId);
    const Track *track = m_document.trackById(trackId);
    if (!clip || !track || track->locked || clip->linkId.isEmpty())
        return;
    pushEdits(QStringLiteral("解链片段"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [](Clip &clip) { clip.linkId.clear(); }));
}

void TimelineEditor::copy(const QList<ClipLocator> &clips)
{
    m_clipboard.clear();
    for (const ClipLocator &locator : clips) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        const Track *track = m_document.trackById(locator.trackId);
        if (!clip || !track)
            continue;
        m_clipboard.append(
            ClipboardEntry{locator.trackId, track->type, *clip});
    }
}

void TimelineEditor::paste(int64_t timelineUs, const QString &targetTrackId)
{
    if (m_clipboard.isEmpty())
        return;
    timelineUs = qMax<int64_t>(0, timelineUs);
    const Track *target = m_document.trackById(targetTrackId);

    // Resolve every destination before touching the document: a pasted linked
    // group either lands completely (both halves on their original tracks) or
    // not at all. Missing source tracks fall back to the requested lane, but
    // only when its type matches.
    QList<QString> destinationIds;
    destinationIds.reserve(m_clipboard.size());
    for (const ClipboardEntry &entry : m_clipboard) {
        const Track *sourceTrack = m_document.trackById(entry.sourceTrackId);
        QString destinationId;
        if (sourceTrack && sourceTrack->type != TrackType::Subtitle) {
            destinationId = sourceTrack->id;
        } else if (target && target->type == entry.sourceTrackType) {
            // The original track is gone: the requested lane may stand in,
            // but only when it has the same media type.
            destinationId = target->id;
        }
        const Track *destination = m_document.trackById(destinationId);
        if (!destination || destination->type == TrackType::Subtitle)
            return;
        if (destination->type != entry.sourceTrackType)
            return;
        if (destination->locked)
            return; // One lock policy: reject the whole paste atomically.
        destinationIds.append(destinationId);
    }

    int64_t minStart = INT64_MAX;
    for (const ClipboardEntry &entry : m_clipboard)
        minStart = qMin(minStart, entry.clip.timelineStartUs);

    QHash<QString, QList<Clip>> addedByTrack;
    QHash<QString, QString> linkMap;
    QList<RemovalLayer> addedLayers;
    for (int i = 0; i < m_clipboard.size(); ++i) {
        const ClipboardEntry &entry = m_clipboard.at(i);
        const QString &destinationId = destinationIds.at(i);

        Clip copy = entry.clip;
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copy.timelineStartUs = timelineUs + (entry.clip.timelineStartUs - minStart);
        if (!copy.linkId.isEmpty()) {
            if (!linkMap.contains(copy.linkId))
                linkMap.insert(copy.linkId,
                               QUuid::createUuid().toString(QUuid::WithoutBraces));
            copy.linkId = linkMap.value(copy.linkId);
        }
        // Each pasted clip gets its own layer copy. A stale clipboard entry
        // (source clip/layer deleted after copy) is repaired by clearing the
        // dangling link instead of pasting an invalid reference.
        if (!copy.removalLayerId.isEmpty()) {
            if (const RemovalLayer *source =
                    m_document.removalLayerById(copy.removalLayerId)) {
                RemovalLayer clone = *source;
                clone.id = QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
                clone.id.prepend(QStringLiteral("rl-"));
                clone.clipId = copy.id;
                clone.sourceInUs = copy.sourceInUs;
                clone.sourceOutUs = copy.sourceOutUs;
                copy.removalLayerId = clone.id;
                addedLayers.append(clone);
            } else {
                copy.removalLayerId.clear();
            }
        }
        addedByTrack[destinationId].append(copy);
    }

    QList<TrackEdit> edits;
    for (auto it = addedByTrack.cbegin(); it != addedByTrack.cend(); ++it) {
        const Track *track = m_document.trackById(it.key());
        if (!track)
            continue;
        TrackEdit edit = snapshotTrack(*track);
        edit.afterClips.append(it.value());
        edits.append(edit);
    }
    pushEdits(QStringLiteral("粘贴片段"), edits, addedLayers, {});
}

void TimelineEditor::addTrack(const Track &track)
{
    QList<Track> before = m_document.project().tracks;
    Track added = track;
    if (added.id.isEmpty())
        added.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (const Track &existing : before) {
        if (existing.id == added.id)
            return;
    }
    QList<Track> after = before;
    after.append(added);
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("添加轨道"), before, after));
}

void TimelineEditor::removeTrack(const QString &trackId)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked)
        return;
    const QList<RemovalLayer> removedLayers = removalLayersForTrack(*track);
    QList<Track> before = m_document.project().tracks;
    QList<Track> after;
    for (const Track &existing : before) {
        if (existing.id != trackId)
            after.append(existing);
    }
    if (after.size() == before.size())
        return;

    QUndoStack *stack = m_document.undoStack();
    const QString text = QStringLiteral("删除轨道");
    if (removedLayers.isEmpty()) {
        stack->push(new TrackListCommand(&m_document, text, before, after));
        return;
    }
    stack->beginMacro(text);
    stack->push(new TrackListCommand(&m_document, text, before, after));
    for (const RemovalLayer &layer : removedLayers) {
        RemovalLayerEdit edit;
        edit.clipId = layer.clipId;
        edit.before = layer;
        edit.after = std::nullopt;
        stack->push(new RemovalLayerCommand(&m_document, text, edit));
    }
    stack->endMacro();
}

void TimelineEditor::moveTrack(int fromIndex, int toIndex)
{
    QList<Track> before = m_document.project().tracks;
    if (fromIndex < 0 || fromIndex >= before.size()
        || toIndex < 0 || toIndex >= before.size()) {
        return;
    }
    if (fromIndex == toIndex)
        return;
    if (before.at(fromIndex).locked || before.at(toIndex).locked)
        return;
    QList<Track> after = before;
    after.move(fromIndex, toIndex);
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("调整轨道顺序"), before, after));
}

QList<TrackEdit> TimelineEditor::editsForCues(
    const QString &trackId,
    const std::function<void(QList<SubtitleCue> &)> &mutate) const
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle || track->locked)
        return {};
    TrackEdit edit = snapshotTrack(*track);
    mutate(edit.afterCues);
    if (edit.afterCues == edit.beforeCues)
        return {};
    return {edit};
}

QString TimelineEditor::addSubtitleCue(const QString &trackId, SubtitleCue cue)
{
    if (cue.id.isEmpty())
        cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (cue.endUs <= cue.startUs || cue.startUs < 0)
        return {};
    if (cue.text.trimmed().isEmpty())
        return {};
    cue.positionXPermille = qBound(0, cue.positionXPermille, 1000);
    cue.positionYPermille = qBound(0, cue.positionYPermille, 1000);
    if (cue.styleId.isEmpty())
        cue.styleId = QStringLiteral("default");
    const QString id = cue.id;
    const QList<TrackEdit> edits =
        editsForCues(trackId, [cue](QList<SubtitleCue> &cues) {
            // Reject a caller-supplied id that already exists: update/remove
            // address cues by id and duplicates would be ambiguous.
            for (const SubtitleCue &existing : cues) {
                if (existing.id == cue.id)
                    return;
            }
            int index = cues.size();
            for (int i = 0; i < cues.size(); ++i) {
                if (cues.at(i).startUs > cue.startUs) {
                    index = i;
                    break;
                }
            }
            cues.insert(index, cue);
        });
    if (edits.isEmpty())
        return {};
    pushEdits(QStringLiteral("添加字幕"), edits);
    return id;
}

void TimelineEditor::updateSubtitleCue(const QString &trackId,
                                       const QString &cueId,
                                       const SubtitleCue &cue)
{
    if (cue.endUs <= cue.startUs || cue.startUs < 0)
        return;
    if (cue.text.trimmed().isEmpty())
        return;
    SubtitleCue updated = cue;
    updated.id = cueId;
    updated.positionXPermille = qBound(0, updated.positionXPermille, 1000);
    updated.positionYPermille = qBound(0, updated.positionYPermille, 1000);
    if (updated.styleId.isEmpty())
        updated.styleId = QStringLiteral("default");
    pushEdits(QStringLiteral("编辑字幕"),
              editsForCues(trackId, [cueId, updated](QList<SubtitleCue> &cues) {
                  bool found = false;
                  for (SubtitleCue &existing : cues) {
                      if (existing.id == cueId) {
                          existing = updated;
                          found = true;
                          break;
                      }
                  }
                  if (!found)
                      return;
                  // Keep the sorted-by-startUs invariant that addSubtitleCue
                  // and importSrt rely on when placing new cues.
                  std::stable_sort(cues.begin(), cues.end(),
                                   [](const SubtitleCue &a, const SubtitleCue &b) {
                                       return a.startUs < b.startUs;
                                   });
              }));
}

void TimelineEditor::setCueTiming(const QString &trackId,
                                  const QString &cueId, int64_t startUs,
                                  int64_t endUs)
{
    if (startUs < 0 || endUs <= startUs)
        return;
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle)
        return;
    const SubtitleCue *existing = nullptr;
    for (const SubtitleCue &cue : track->cues) {
        if (cue.id == cueId) {
            existing = &cue;
            break;
        }
    }
    if (!existing)
        return;
    SubtitleCue updated = *existing;
    updated.startUs = startUs;
    updated.endUs = endUs;
    updateSubtitleCue(trackId, cueId, updated);
}

void TimelineEditor::removeSubtitleCue(const QString &trackId,
                                       const QString &cueId)
{
    pushEdits(QStringLiteral("删除字幕"),
              editsForCues(trackId, [cueId](QList<SubtitleCue> &cues) {
                  for (int i = 0; i < cues.size(); ++i) {
                      if (cues.at(i).id == cueId) {
                          cues.removeAt(i);
                          return;
                      }
                  }
              }));
}

void TimelineEditor::applySubtitleStyle(const QString &trackId,
                                        const QString &styleId,
                                        const QString &cueId)
{
    if (styleId.isEmpty())
        return;
    pushEdits(QStringLiteral("应用字幕样式"),
              editsForCues(trackId, [styleId, cueId](QList<SubtitleCue> &cues) {
                  for (SubtitleCue &cue : cues) {
                      if (cueId.isEmpty() || cue.id == cueId)
                          cue.styleId = styleId;
                  }
              }));
}

int TimelineEditor::importSrt(const QString &trackId, const QByteArray &bytes,
                              QString *error)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle) {
        if (error)
            *error = QStringLiteral("未找到可用的字幕轨。");
        return 0;
    }
    if (track->locked) {
        if (error)
            *error = QStringLiteral("字幕轨已锁定。");
        return 0;
    }
    const SrtParseResult parsed = parseSrt(bytes);
    if (!parsed.ok()) {
        if (error)
            *error = parsed.error;
        return 0;
    }
    QList<SubtitleCue> imported;
    QSet<QString> usedIds;
    for (SubtitleCue cue : parsed.cues) {
        cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        while (usedIds.contains(cue.id))
            cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        usedIds.insert(cue.id);
        imported.append(cue);
    }
    const QList<TrackEdit> edits =
        editsForCues(trackId, [imported](QList<SubtitleCue> &cues) {
            cues.append(imported);
            std::stable_sort(cues.begin(), cues.end(),
                             [](const SubtitleCue &a, const SubtitleCue &b) {
                                 return a.startUs < b.startUs;
                             });
        });
    if (edits.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有可用的字幕轨。");
        return 0;
    }
    pushEdits(QStringLiteral("导入 SRT"), edits);
    return int(imported.size());
}

QByteArray TimelineEditor::exportSrt(const QString &trackId) const
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle)
        return {};
    return writeSrt(track->cues);
}

QString TimelineEditor::uniqueLinkId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

SnapResult TimelineEditor::snapTime(int64_t timeUs, int64_t thresholdUs,
                                    const QList<int64_t> &candidates)
{
    SnapResult result{timeUs, false};
    int64_t bestDistance = thresholdUs + 1;
    for (const int64_t candidate : candidates) {
        const int64_t distance = qAbs(candidate - timeUs);
        if (distance <= thresholdUs && distance < bestDistance) {
            result.timeUs = candidate;
            result.snapped = true;
            bestDistance = distance;
        }
    }
    return result;
}

} // namespace VideoEditor
