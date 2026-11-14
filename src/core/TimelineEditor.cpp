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
            TrackEdit edit;
            edit.trackId = locator.trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterClips = track->clips;
            edit.afterCues = track->cues;
            edits.append(edit);
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
    return edits;
}

QStringList TimelineEditor::addClips(
    const QList<QPair<QString, Clip>> &trackAndClip)
{
    QList<TrackEdit> edits;
    QStringList addedIds;
    QHash<QString, int> editIndexByTrack;

    for (const auto &entry : trackAndClip) {
        const QString &trackId = entry.first;
        Clip clip = entry.second;
        const Track *track = m_document.trackById(trackId);
        if (!track || track->type == TrackType::Subtitle || track->locked)
            continue;
        if (clip.id.isEmpty())
            clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (clip.assetId.isEmpty() || clip.sourceOutUs <= clip.sourceInUs)
            continue;
        clip.speedMilli = qBound(kMinSpeedMilli, clip.speedMilli, kMaxSpeedMilli);
        clip.timelineStartUs = qMax<int64_t>(0, clip.timelineStartUs);

        if (!editIndexByTrack.contains(trackId)) {
            TrackEdit edit;
            edit.trackId = trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterClips = track->clips;
            edit.afterCues = track->cues;
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
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty())
        return;

    int64_t delta = newStartUs - findClip(trackId, clipId)->timelineStartUs;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            continue;
        delta = qMax(delta, -clip->timelineStartUs);
        if (m_document.trackById(locator.trackId)->locked)
            return;
    }
    pushEdits(QStringLiteral("移动片段"),
              editsForClips(group, [delta](Clip &clip) {
                  clip.timelineStartUs += delta;
              }));
}

void TimelineEditor::trimStart(const QString &trackId, const QString &clipId,
                               int64_t newStartUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked || newStartUs < 0)
        return;
    const int64_t minUs = oneFrameUs(m_document);
    const int64_t maxStart = clicked->timelineEndUs() - minUs;
    if (maxStart <= clicked->timelineStartUs)
        return; // Already at (or below) the one-frame minimum.
    newStartUs = qBound<int64_t>(0, newStartUs, maxStart);
    const int64_t timelineDelta = newStartUs - clicked->timelineStartUs;
    if (timelineDelta == 0)
        return;

    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    pushEdits(QStringLiteral("裁切片段入点"),
              editsForClips(group, [timelineDelta, minUs](Clip &clip) {
                  const int64_t sourceDelta = scaleBySpeed(timelineDelta, clip.speedMilli);
                  const int64_t minimumRemaining =
                      minSourceForTimeline(minUs, clip.speedMilli);
                  const int64_t newIn = qBound<int64_t>(
                      0, clip.sourceInUs + sourceDelta,
                      clip.sourceOutUs - minimumRemaining);
                  const int64_t actualTimelineDelta =
                      unscaleBySpeed(newIn - clip.sourceInUs, clip.speedMilli);
                  clip.sourceInUs = newIn;
                  clip.timelineStartUs = qMax<int64_t>(0,
                      clip.timelineStartUs + actualTimelineDelta);
              }));
}

void TimelineEditor::trimEnd(const QString &trackId, const QString &clipId,
                             int64_t newEndUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    const int64_t minUs = oneFrameUs(m_document);
    newEndUs = qMax(clicked->timelineStartUs + minUs, newEndUs);
    const int64_t wantedDuration = newEndUs - clicked->timelineStartUs;
    if (wantedDuration == clicked->timelineDurationUs())
        return;

    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    pushEdits(QStringLiteral("裁切片段出点"),
              editsForClips(group, [this, wantedDuration, minUs](Clip &clip) {
                  const int64_t minimumOut =
                      clip.sourceInUs + minSourceForTimeline(minUs, clip.speedMilli);
                  const int64_t wantedOut = clip.sourceInUs
                      + scaleBySpeed(wantedDuration, clip.speedMilli);
                  int64_t limit = qMax(minimumOut, wantedOut);
                  if (const Asset *asset = m_document.project().assetById(clip.assetId))
                      limit = qMin(limit, asset->info.durationUs);
                  clip.sourceOutUs = limit;
              }));
}

void TimelineEditor::splitClip(const QString &trackId, const QString &clipId,
                               int64_t timelineUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    if (timelineUs <= clicked->timelineStartUs || timelineUs >= clicked->timelineEndUs())
        return;
    const int64_t splitSource = sourceTimeAt(*clicked, timelineUs);
    if (splitSource <= clicked->sourceInUs || splitSource >= clicked->sourceOutUs)
        return;

    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    const QString rightLinkId =
        clicked->linkId.isEmpty() ? QString() : uniqueLinkId();

    QList<TrackEdit> edits;
    QHash<QString, int> editIndexByTrack;
    for (const ClipLocator &locator : group) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track || track->locked)
            return;
        if (!editIndexByTrack.contains(locator.trackId)) {
            TrackEdit edit;
            edit.trackId = locator.trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterClips = track->clips;
            edit.afterCues = track->cues;
            editIndexByTrack.insert(locator.trackId, edits.size());
            edits.append(edit);
        }
        TrackEdit &target = edits[editIndexByTrack.value(locator.trackId)];
        for (int i = 0; i < target.afterClips.size(); ++i) {
            Clip &left = target.afterClips[i];
            if (left.id != locator.clipId)
                continue;
            const int64_t sourceAtCut = sourceTimeAt(left, timelineUs);
            if (sourceAtCut <= left.sourceInUs || sourceAtCut >= left.sourceOutUs)
                break;
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
    if (group.isEmpty())
        return;

    QList<TrackEdit> edits;
    QHash<QString, int> editIndexByTrack;
    for (const ClipLocator &locator : group) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track || track->locked)
            return;
        if (!editIndexByTrack.contains(locator.trackId)) {
            TrackEdit edit;
            edit.trackId = locator.trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterCues = track->cues;
            editIndexByTrack.insert(locator.trackId, edits.size());
            edits.append(edit);
        }
        TrackEdit &edit = edits[editIndexByTrack.value(locator.trackId)];

        int64_t removedStart = 0;
        int64_t removedDuration = 0;
        for (const Clip &clip : track->clips) {
            for (const ClipLocator &onTrack : group) {
                if (onTrack.trackId == locator.trackId && onTrack.clipId == clip.id) {
                    removedStart = clip.timelineStartUs;
                    removedDuration = qMax(removedDuration, clip.timelineDurationUs());
                }
            }
        }

        edit.afterClips.clear();
        for (const Clip &clip : track->clips) {
            bool removed = false;
            for (const ClipLocator &onTrack : group) {
                if (onTrack.trackId == locator.trackId && onTrack.clipId == clip.id)
                    removed = true;
            }
            if (removed)
                continue;
            Clip adjusted = clip;
            if (ripple && removedDuration > 0
                && clip.timelineStartUs >= removedStart + removedDuration) {
                adjusted.timelineStartUs -= removedDuration;
            }
            edit.afterClips.append(adjusted);
        }
    }
    pushEdits(ripple ? QStringLiteral("波纹删除") : QStringLiteral("删除片段"), edits);
}

void TimelineEditor::setClipSpeed(const QString &trackId, const QString &clipId,
                                  int speedMilli, bool preservePitch)
{
    speedMilli = qBound(kMinSpeedMilli, speedMilli, kMaxSpeedMilli);
    pushEdits(QStringLiteral("设置片段变速"),
              editsForClips(linkGroup(trackId, clipId),
                            [speedMilli, preservePitch](Clip &clip) {
                                clip.speedMilli = speedMilli;
                                clip.preservePitch = preservePitch;
                            }));
}

void TimelineEditor::setClipAudio(const QString &trackId, const QString &clipId,
                                  const AudioParams &params)
{
    pushEdits(QStringLiteral("调整音频"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [params](Clip &clip) { clip.audio = params; }));
}

void TimelineEditor::setClipTransform(const QString &trackId,
                                      const QString &clipId,
                                      const Transform &transform)
{
    pushEdits(QStringLiteral("调整画面"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [transform](Clip &clip) { clip.transform = transform; }));
}

void TimelineEditor::setClipEnabled(const QString &trackId,
                                    const QString &clipId, bool enabled)
{
    pushEdits(enabled ? QStringLiteral("启用片段") : QStringLiteral("禁用片段"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [enabled](Clip &clip) { clip.enabled = enabled; }));
}

void TimelineEditor::linkClips(const QList<ClipLocator> &clips)
{
    if (clips.size() < 2)
        return;
    const Clip *first = findClip(clips.first().trackId, clips.first().clipId);
    if (!first)
        return;
    const QString linkId = uniqueLinkId();
    int64_t startUs = first->timelineStartUs;
    int speedMilli = first->speedMilli;
    for (const ClipLocator &locator : clips) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        startUs = qMin(startUs, clip->timelineStartUs);
    }
    pushEdits(QStringLiteral("链接片段"),
              editsForClips(clips, [linkId, startUs, speedMilli](Clip &clip) {
                  clip.linkId = linkId;
                  clip.timelineStartUs = startUs;
                  clip.speedMilli = speedMilli;
              }));
}

void TimelineEditor::unlinkClip(const QString &trackId, const QString &clipId)
{
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
        if (!destination || destination->locked || destination->type != target->type)
            continue;

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
        TrackEdit edit;
        edit.trackId = it.key();
        edit.beforeClips = track->clips;
        edit.afterClips = track->clips;
        edit.afterClips.append(it.value());
        edit.beforeCues = track->cues;
        edit.afterCues = track->cues;
        edits.append(edit);
    }
    pushEdits(QStringLiteral("粘贴片段"), edits);
}

void TimelineEditor::addTrack(const Track &track)
{
    QList<Track> before = m_document.project().tracks;
    QList<Track> after = before;
    Track added = track;
    if (added.id.isEmpty())
        added.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    after.append(added);
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("添加轨道"), before, after));
}

void TimelineEditor::removeTrack(const QString &trackId)
{
    QList<Track> before = m_document.project().tracks;
    QList<Track> after;
    for (const Track &track : before) {
        if (track.id != trackId)
            after.append(track);
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
