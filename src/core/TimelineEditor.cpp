#include "TimelineEditor.h"

#include <climits>
#include <utility>
#include <QHash>
#include <QSet>
#include <QUuid>

#include "ProjectDocument.h"
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

void TimelineEditor::pushEdits(const QString &text, QList<TrackEdit> edits)
{
    if (edits.isEmpty())
        return;
    m_document.undoStack()->push(
        new TimelineEditCommand(&m_document, text, std::move(edits)));
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
        sourceLower = qMax(sourceLower, -clip->sourceInUs);
        sourceUpper = qMin(sourceUpper, clip->sourceOutUs - minSource);
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
            left.sourceOutUs = sourceAtCut;
            target.afterClips.insert(i + 1, right);
            break;
        }
    }
    pushEdits(QStringLiteral("分割片段"), edits);
}

void TimelineEditor::deleteClip(const QString &trackId, const QString &clipId,
                                bool ripple)
{
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty() || groupLocked(group))
        return;

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
        pushEdits(QStringLiteral("删除片段"), edits);
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
    pushEdits(QStringLiteral("波纹删除"), edits);
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
        if (!clip)
            continue;
        m_clipboard.append(ClipboardEntry{locator.trackId, *clip});
    }
}

void TimelineEditor::paste(int64_t timelineUs, const QString &targetTrackId)
{
    if (m_clipboard.isEmpty())
        return;
    timelineUs = qMax<int64_t>(0, timelineUs);
    const Track *target = m_document.trackById(targetTrackId);
    if (!target || target->locked)
        return;

    int64_t minStart = INT64_MAX;
    for (const ClipboardEntry &entry : m_clipboard)
        minStart = qMin(minStart, entry.clip.timelineStartUs);

    QHash<QString, QList<Clip>> addedByTrack;
    QHash<QString, QString> linkMap;
    for (const ClipboardEntry &entry : m_clipboard) {
        const Track *sourceTrack = m_document.trackById(entry.sourceTrackId);
        QString destinationId = entry.sourceTrackId;
        if (!sourceTrack || sourceTrack->type != target->type) {
            if (sourceTrack && sourceTrack->type != target->type)
                continue;
            destinationId = targetTrackId;
        }
        const Track *destination = m_document.trackById(destinationId);
        if (!destination || destination->type != target->type)
            continue;
        if (destination->locked)
            return; // One lock policy: reject the whole paste.

        Clip copy = entry.clip;
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copy.timelineStartUs = timelineUs + (entry.clip.timelineStartUs - minStart);
        if (!copy.linkId.isEmpty()) {
            if (!linkMap.contains(copy.linkId))
                linkMap.insert(copy.linkId,
                               QUuid::createUuid().toString(QUuid::WithoutBraces));
            copy.linkId = linkMap.value(copy.linkId);
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
    pushEdits(QStringLiteral("粘贴片段"), edits);
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
    QList<Track> before = m_document.project().tracks;
    QList<Track> after;
    for (const Track &existing : before) {
        if (existing.id != trackId)
            after.append(existing);
    }
    if (after.size() == before.size())
        return;
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("删除轨道"), before, after));
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
