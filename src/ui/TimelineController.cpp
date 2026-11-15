#include "TimelineController.h"

#include <cmath>
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include "TimeBase.h"
#include "TimelineEvaluator.h"
#include "TimelineModel.h"
#include "TrackListModel.h"
#include "commands/TrackListCommand.h"

namespace VideoEditor {

namespace {

Rational projectFps(const Project &project)
{
    return project.timeline.fps.isValid() ? project.timeline.fps
                                          : Rational{30, 1};
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

TimelineController::TimelineController(ProjectDocument &document, QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_editor(document)
{
    m_timelineModel = new TimelineModel(this);
    m_trackModel = new TrackListModel(this);
    m_timelineModel->setProject(m_document.project());
    m_trackModel->setProject(m_document.project());
    m_lastDurationUs = durationUs();

    connect(&m_document, &ProjectDocument::timelineChanged, this,
            &TimelineController::refreshFromDocument);
    connect(&m_document, &ProjectDocument::tracksChanged, this,
            &TimelineController::refreshFromDocument);
    connect(&m_document, &ProjectDocument::projectReplaced, this, [this] {
        // Clipboard entries point at the old document's tracks; pasting them
        // into a freshly loaded project would either target stale ids or
        // duplicate content invisibly.
        m_editor.clearClipboard();
    });
}

QVariantMap TimelineController::selectedClip() const
{
    QVariantMap map;
    if (m_selectedClipId.isEmpty())
        return map;

    QString trackId;
    TrackType trackType = TrackType::Video;
    const Clip *clip = nullptr;
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &candidate : track.clips) {
            if (candidate.id == m_selectedClipId) {
                trackId = track.id;
                trackType = track.type;
                clip = &candidate;
                break;
            }
        }
        if (clip)
            break;
    }
    if (!clip)
        return map;

    map.insert(QStringLiteral("id"), clip->id);
    map.insert(QStringLiteral("trackId"), trackId);
    // The inspector disables clip-audio controls on video tracks: a picture
    // clip whose only audio lives on the linked A-track cannot be heard.
    map.insert(QStringLiteral("trackType"), trackTypeToString(trackType));
    map.insert(QStringLiteral("hasAudioControls"),
               trackType == TrackType::Audio);
    map.insert(QStringLiteral("assetId"), clip->assetId);
    map.insert(QStringLiteral("speedMilli"), clip->speedMilli);
    map.insert(QStringLiteral("preservePitch"), clip->preservePitch);
    map.insert(QStringLiteral("volumeMilli"), clip->audio.volumeMilli);
    map.insert(QStringLiteral("panMilli"), clip->audio.panMilli);
    map.insert(QStringLiteral("fadeInUs"), QVariant::fromValue<qint64>(
                                               clip->audio.fadeInUs));
    map.insert(QStringLiteral("fadeOutUs"), QVariant::fromValue<qint64>(
                                                clip->audio.fadeOutUs));
    map.insert(QStringLiteral("scaleMilli"), clip->transform.scaleMilli);
    map.insert(QStringLiteral("offsetXPermille"),
               clip->transform.offsetXPermille);
    map.insert(QStringLiteral("offsetYPermille"),
               clip->transform.offsetYPermille);
    map.insert(QStringLiteral("opacityMilli"),
               clip->transform.opacityMilli);
    map.insert(QStringLiteral("startUs"),
               QVariant::fromValue<qint64>(clip->timelineStartUs));
    map.insert(QStringLiteral("durationUs"),
               QVariant::fromValue<qint64>(clip->timelineDurationUs()));

    QVariantList keyframes;
    for (const VolumeKeyframe &keyframe : clip->audio.keyframes) {
        QVariantMap entry;
        entry.insert(QStringLiteral("timeUs"),
                     QVariant::fromValue<qint64>(keyframe.timeUs));
        entry.insert(QStringLiteral("volumeMilli"), keyframe.volumeMilli);
        keyframes.append(entry);
    }
    map.insert(QStringLiteral("keyframeCount"), keyframes.size());
    map.insert(QStringLiteral("keyframes"), keyframes);
    return map;
}

QVariantMap TimelineController::selectedCue() const
{
    if (m_selectedCueId.isEmpty())
        return {};

    QString trackId;
    const SubtitleCue *cue = nullptr;
    for (const Track &track : m_document.project().tracks) {
        for (const SubtitleCue &candidate : track.cues) {
            if (candidate.id == m_selectedCueId) {
                trackId = track.id;
                cue = &candidate;
                break;
            }
        }
        if (cue)
            break;
    }
    if (!cue)
        return {};

    QVariantMap map;
    map.insert(QStringLiteral("id"), cue->id);
    map.insert(QStringLiteral("trackId"), trackId);
    map.insert(QStringLiteral("startUs"),
               QVariant::fromValue<qint64>(cue->startUs));
    map.insert(QStringLiteral("endUs"),
               QVariant::fromValue<qint64>(cue->endUs));
    map.insert(QStringLiteral("text"), cue->text);
    map.insert(QStringLiteral("styleId"), cue->styleId);
    map.insert(QStringLiteral("positionXPermille"), cue->positionXPermille);
    map.insert(QStringLiteral("positionYPermille"), cue->positionYPermille);
    map.insert(QStringLiteral("enabled"), cue->enabled);
    return map;
}

void TimelineController::refreshFromDocument()
{
    m_timelineModel->setProject(m_document.project());
    m_trackModel->setProject(m_document.project());

    bool selectionLost = false;
    if (!m_selectedClipId.isEmpty() && !trackForClip(m_selectedClipId)) {
        m_selectedClipId.clear();
        selectionLost = true;
    }
    if (!m_selectedCueId.isEmpty() && !trackForCue(m_selectedCueId)) {
        m_selectedCueId.clear();
        selectionLost = true;
    }
    if (selectionLost) {
        m_timelineModel->selectItem(QString());
        emit selectionChanged();
    }
    // A removed track must not remain the target for later inserts.
    if (!m_targetTrackId.isEmpty() && !m_document.trackById(m_targetTrackId)) {
        m_targetTrackId.clear();
        emit targetTrackChanged();
    }
    // Clip properties may have changed while the same clip stayed selected;
    // the inspector binds to these maps, so refresh them on every edit.
    emit selectedClipChanged();
    emit selectedCueChanged();

    // Track commands emit tracksChanged and timelineChanged back to back; only
    // notify bindings when the duration actually changed.
    const qint64 duration = durationUs();
    if (duration != m_lastDurationUs) {
        m_lastDurationUs = duration;
        emit durationChanged();
    }
}

qint64 TimelineController::durationUs() const
{
    return TimelineEvaluator(m_document.project()).durationUs();
}

QString TimelineController::timecode() const
{
    const Project &project = m_document.project();
    return formatTimecodeAtRate(m_positionUs, projectFps(project),
                                project.timeline.dropFrameTimecode);
}

void TimelineController::setPixelsPerSecond(double value)
{
    if (value <= 0.0 || qFuzzyCompare(value, m_pixelsPerSecond))
        return;
    m_pixelsPerSecond = value;
    emit viewChanged();
}

void TimelineController::setSnapEnabled(bool enabled)
{
    if (m_snapEnabled == enabled)
        return;
    m_snapEnabled = enabled;
    emit viewChanged();
}

void TimelineController::setTargetTrackId(const QString &trackId)
{
    QString next = trackId;
    if (!next.isEmpty() && !m_document.trackById(next))
        next.clear();
    if (next == m_targetTrackId)
        return;
    m_targetTrackId = next;
    emit targetTrackChanged();
}

void TimelineController::seekUs(qint64 positionUs)
{
    const qint64 clamped = positionUs < 0 ? 0 : positionUs;
    if (clamped == m_positionUs)
        return;
    m_positionUs = clamped;
    emit positionChanged();
}

qint64 TimelineController::frameQuantizeUs(qint64 timeUs) const
{
    const Rational fps = projectFps(m_document.project());
    if (!fps.isValid())
        return qMax<qint64>(0, timeUs);
    const int64_t frame = microsToFrames(qMax<qint64>(0, timeUs), fps);
    return framesToMicros(frame < 0 ? 0 : frame, fps);
}

bool TimelineController::seekTimecode(const QString &text)
{
    const std::optional<int64_t> parsed =
        parseTimecode(text, projectFps(m_document.project()));
    if (!parsed)
        return false;
    seekUs(*parsed);
    return true;
}

void TimelineController::stepFrames(int delta)
{
    if (delta == 0)
        return;
    const Rational fps = projectFps(m_document.project());
    const int64_t frames = microsToFrames(m_positionUs, fps) + delta;
    seekUs(framesToMicros(frames < 0 ? 0 : frames, fps));
}

QList<int64_t> TimelineController::snapCandidates(
    const QString &excludeClipId) const
{
    // A linked drag moves the whole group together, so the edges excluded
    // below are those of the selected clip and every clip sharing its linkId.
    QString excludeLinkId;
    if (!excludeClipId.isEmpty()) {
        bool found = false;
        for (const Track &track : m_document.project().tracks) {
            for (const Clip &clip : track.clips) {
                if (clip.id == excludeClipId) {
                    excludeLinkId = clip.linkId;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    const QList<Track> &tracks = m_document.project().tracks;
    auto isOwnClip = [&](const Clip &clip) {
        return (!excludeClipId.isEmpty() && clip.id == excludeClipId)
            || (!excludeLinkId.isEmpty() && clip.linkId == excludeLinkId);
    };

    QList<int64_t> ownEdges;
    QList<int64_t> candidates;
    candidates.append(0);
    candidates.append(m_positionUs);
    candidates.append(durationUs());

    for (const Track &track : tracks) {
        for (const Clip &clip : track.clips) {
            if (isOwnClip(clip)) {
                ownEdges.append(clip.timelineStartUs);
                ownEdges.append(clip.timelineEndUs());
                continue;
            }
            candidates.append(clip.timelineStartUs);
            candidates.append(clip.timelineEndUs());
        }
    }

    // The origin, playhead and project duration may coincide with the moving
    // group's own edges (first clip at 0, last clip at duration, playhead on
    // the clip start). Those values must not self-snap either.
    candidates.removeIf([&ownEdges](int64_t candidate) {
        return ownEdges.contains(candidate);
    });
    return candidates;
}

qint64 TimelineController::snapUs(qint64 timeUs) const
{
    if (!m_snapEnabled || m_pixelsPerSecond <= 0.0)
        return timeUs;
    const int64_t threshold = qMax<int64_t>(
        1, std::llround(8.0 / m_pixelsPerSecond * 1'000'000.0));
    return TimelineEditor::snapTime(timeUs, threshold,
                                    snapCandidates(m_selectedClipId))
        .timeUs;
}

QString TimelineController::firstTrackIdOfType(TrackType type) const
{
    for (const Track &track : m_document.project().tracks) {
        if (track.type == type)
            return track.id;
    }
    return {};
}

const Clip *TimelineController::findClip(const QString &trackId,
                                         const QString &clipId) const
{
    for (const Track &track : m_document.project().tracks) {
        if (track.id != trackId)
            continue;
        for (const Clip &clip : track.clips) {
            if (clip.id == clipId)
                return &clip;
        }
    }
    return nullptr;
}

const SubtitleCue *TimelineController::findCue(const QString &trackId,
                                               const QString &cueId) const
{
    for (const Track &track : m_document.project().tracks) {
        if (track.id != trackId)
            continue;
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id == cueId)
                return &cue;
        }
    }
    return nullptr;
}

Track *TimelineController::trackForClip(const QString &clipId, QString *trackId)
{
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &clip : track.clips) {
            if (clip.id != clipId)
                continue;
            if (trackId)
                *trackId = track.id;
            return m_document.trackById(track.id);
        }
    }
    return nullptr;
}

Track *TimelineController::trackForCue(const QString &cueId, QString *trackId)
{
    for (const Track &track : m_document.project().tracks) {
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id != cueId)
                continue;
            if (trackId)
                *trackId = track.id;
            return m_document.trackById(track.id);
        }
    }
    return nullptr;
}

QString TimelineController::addAssetToTimeline(const QString &assetId,
                                               const QString &trackId,
                                               qint64 atUs)
{
    const Asset *asset = m_document.project().assetById(assetId);
    if (!asset) {
        emit editFailed(QStringLiteral("未找到素材。"));
        return {};
    }
    if (asset->info.durationUs <= 0) {
        emit editFailed(QStringLiteral("素材时长无效。"));
        return {};
    }

    const bool wantsVideo =
        asset->info.hasVideo || asset->info.kind == MediaKind::Video;
    const bool wantsAudio = asset->info.hasAudio;
    if (!wantsVideo && !wantsAudio) {
        emit editFailed(QStringLiteral("不支持的素材类型。"));
        return {};
    }

    const int64_t startUs = qMax<int64_t>(0, atUs);
    const Track *requested = m_document.trackById(trackId);

    // Nth track of a type, used to pair original audio with the target video
    // lane (V2 -> A2). Falls back to the first matching track when no such
    // ordinal exists.
    const auto trackIdAtOrdinal = [this](TrackType type, int ordinal) {
        int seen = 0;
        for (const Track &track : m_document.project().tracks) {
            if (track.type != type)
                continue;
            if (seen == ordinal)
                return track.id;
            ++seen;
        }
        return QString();
    };
    int requestedVideoOrdinal = 0;
    if (requested && requested->type == TrackType::Video) {
        for (const Track &track : m_document.project().tracks) {
            if (track.id == requested->id)
                break;
            if (track.type == TrackType::Video)
                ++requestedVideoOrdinal;
        }
    }

    QString videoTrackId;
    if (wantsVideo) {
        if (requested && requested->type == TrackType::Video)
            videoTrackId = requested->id;
        else
            videoTrackId = firstTrackIdOfType(TrackType::Video);
        if (videoTrackId.isEmpty()) {
            emit editFailed(QStringLiteral("没有可用的视频轨。"));
            return {};
        }
    }

    QString audioTrackId;
    if (wantsAudio) {
        if (requested && requested->type == TrackType::Audio) {
            audioTrackId = requested->id;
        } else if (requested && requested->type == TrackType::Video) {
            // Keep the original audio on the A-lane paired with the chosen
            // V-lane instead of piling every source onto A1.
            audioTrackId = trackIdAtOrdinal(TrackType::Audio,
                                            requestedVideoOrdinal);
            if (audioTrackId.isEmpty())
                audioTrackId = firstTrackIdOfType(TrackType::Audio);
        } else {
            audioTrackId = firstTrackIdOfType(TrackType::Audio);
        }
        if (audioTrackId.isEmpty() && !wantsVideo) {
            emit editFailed(QStringLiteral("没有可用的音频轨。"));
            return {};
        }
    }

    const bool linked = !videoTrackId.isEmpty() && !audioTrackId.isEmpty();
    const QString linkId = linked ? newId() : QString();

    QList<QPair<QString, Clip>> entries;
    QString videoClipId;
    QString audioClipId;

    if (!videoTrackId.isEmpty()) {
        Clip clip;
        clip.id = newId();
        clip.assetId = assetId;
        clip.linkId = linkId;
        clip.sourceInUs = 0;
        clip.sourceOutUs = asset->info.durationUs;
        clip.timelineStartUs = startUs;
        videoClipId = clip.id;
        entries.append({videoTrackId, clip});
    }
    if (!audioTrackId.isEmpty()) {
        Clip clip;
        clip.id = newId();
        clip.assetId = assetId;
        clip.linkId = linkId;
        clip.sourceInUs = 0;
        clip.sourceOutUs = asset->info.durationUs;
        clip.timelineStartUs = startUs;
        audioClipId = clip.id;
        entries.append({audioTrackId, clip});
    }

    // One addClips command keeps the linked pair in a single undo step.
    const QStringList added = m_editor.addClips(entries);
    if (added.size() != entries.size()) {
        // addClips skips locked tracks silently; a half-added link group would
        // leave the timeline inconsistent. Roll back the partial command
        // before reporting failure so the caller's model stays unchanged.
        if (!added.isEmpty())
            m_document.undo();
        emit editFailed(QStringLiteral("部分片段未能添加，轨道可能已锁定。"));
        return {};
    }
    if (!videoClipId.isEmpty() && added.contains(videoClipId))
        return videoClipId;
    if (!audioClipId.isEmpty() && added.contains(audioClipId))
        return audioClipId;
    if (!added.isEmpty())
        return added.first();
    emit editFailed(QStringLiteral("添加片段失败。"));
    return {};
}

QString TimelineController::addClipForTest(const QString &trackId,
                                           const Clip &clip)
{
    return m_editor.addClip(trackId, clip);
}

void TimelineController::moveClip(const QString &clipId, qint64 newStartUs)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    m_editor.moveClip(trackId, clipId, newStartUs);
}

void TimelineController::trimStart(const QString &clipId, qint64 newStartUs)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    m_editor.trimStart(trackId, clipId, newStartUs);
}

void TimelineController::trimEnd(const QString &clipId, qint64 newEndUs)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    m_editor.trimEnd(trackId, clipId, newEndUs);
}

void TimelineController::splitAtPlayhead()
{
    QList<ClipLocator> locators;
    for (const Track &track : m_document.project().tracks) {
        if (track.type == TrackType::Subtitle)
            continue;
        for (const Clip &clip : track.clips) {
            if (m_positionUs > clip.timelineStartUs
                && m_positionUs < clip.timelineEndUs()) {
                locators.append(ClipLocator{track.id, clip.id});
            }
        }
    }

    // Fallback: the playhead is elsewhere but the user explicitly selected a
    // clip that still contains a valid split point.
    if (locators.isEmpty() && !m_selectedClipId.isEmpty()) {
        QString trackId;
        if (trackForClip(m_selectedClipId, &trackId)) {
            const Clip *clip = findClip(trackId, m_selectedClipId);
            if (clip && m_positionUs > clip->timelineStartUs
                && m_positionUs < clip->timelineEndUs()) {
                locators.append(ClipLocator{trackId, m_selectedClipId});
            }
        }
    }

    if (locators.isEmpty()) {
        emit editFailed(QStringLiteral("播放头不在可分割的片段上。"));
        return;
    }

    // splitClip already acts on the whole linked group, so call it once per
    // group instead of once per clip.
    QSet<QString> handled;
    for (const ClipLocator &locator : locators) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            continue;
        const QString key = clip->linkId.isEmpty()
                                ? locator.trackId + QLatin1Char(':')
                                      + locator.clipId
                                : clip->linkId;
        if (handled.contains(key))
            continue;
        handled.insert(key);
        m_editor.splitClip(locator.trackId, locator.clipId, m_positionUs);
    }
}

void TimelineController::deleteSelected(bool ripple)
{
    if (m_selectedClipId.isEmpty())
        return;
    QString trackId;
    if (!trackForClip(m_selectedClipId, &trackId))
        return;
    m_editor.deleteClip(trackId, m_selectedClipId, ripple);
}

void TimelineController::deleteSelection(bool ripple)
{
    // Selecting a cue deliberately clears the clip selection, so dispatch on
    // the cue first or Delete would silently do nothing for subtitles.
    if (!m_selectedCueId.isEmpty()) {
        deleteSelectedCue();
        return;
    }
    if (!m_selectedClipId.isEmpty()) {
        deleteSelected(ripple);
        return;
    }
    emit editFailed(QStringLiteral("请先选择片段或字幕。"));
}

void TimelineController::copySelected()
{
    if (m_selectedClipId.isEmpty())
        return;
    QString trackId;
    if (!trackForClip(m_selectedClipId, &trackId))
        return;
    const Clip *selected = findClip(trackId, m_selectedClipId);
    if (!selected)
        return;

    QList<ClipLocator> locators;
    if (selected->linkId.isEmpty()) {
        locators.append(ClipLocator{trackId, m_selectedClipId});
    } else {
        // Copying one half of a linked pair must copy the whole group so a
        // later paste stays in sync.
        for (const Track &track : m_document.project().tracks) {
            for (const Clip &clip : track.clips) {
                if (clip.linkId == selected->linkId)
                    locators.append(ClipLocator{track.id, clip.id});
            }
        }
    }
    m_editor.copy(locators);
}

void TimelineController::pasteAtPlayhead()
{
    QString targetId;
    if (!m_selectedClipId.isEmpty())
        trackForClip(m_selectedClipId, &targetId);
    if (targetId.isEmpty()) {
        for (const Track &track : m_document.project().tracks) {
            if (track.type != TrackType::Subtitle) {
                targetId = track.id;
                break;
            }
        }
    }
    if (targetId.isEmpty())
        return;
    m_editor.paste(m_positionUs, targetId);
}

void TimelineController::setClipSpeed(const QString &clipId, int speedMilli,
                                      bool preservePitch)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    m_editor.setClipSpeed(trackId, clipId, speedMilli, preservePitch);
}

void TimelineController::setClipVolume(const QString &clipId, int volumeMilli)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    AudioParams params = clip->audio;
    params.volumeMilli = volumeMilli;
    m_editor.setClipAudio(trackId, clipId, params);
}

void TimelineController::setClipPan(const QString &clipId, int panMilli)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    AudioParams params = clip->audio;
    params.panMilli = panMilli;
    m_editor.setClipAudio(trackId, clipId, params);
}

void TimelineController::setClipFades(const QString &clipId, qint64 fadeInUs,
                                      qint64 fadeOutUs)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    AudioParams params = clip->audio;
    params.fadeInUs = fadeInUs;
    params.fadeOutUs = fadeOutUs;
    m_editor.setClipAudio(trackId, clipId, params);
}

void TimelineController::setClipFadeIn(const QString &clipId, qint64 fadeInUs)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    AudioParams params = clip->audio;
    params.fadeInUs = fadeInUs;
    m_editor.setClipAudio(trackId, clipId, params);
}

void TimelineController::setClipFadeOut(const QString &clipId, qint64 fadeOutUs)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    AudioParams params = clip->audio;
    params.fadeOutUs = fadeOutUs;
    m_editor.setClipAudio(trackId, clipId, params);
}

bool TimelineController::addVolumeKeyframe(const QString &trackId,
                                           const QString &clipId,
                                           int64_t timeUs, int volumeMilli)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked)
        return false;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return false;
    // Keyframe time is relative to the clip start and must stay inside it.
    if (timeUs < 0 || timeUs > clip->timelineDurationUs())
        return false;

    AudioParams params = clip->audio;
    const VolumeKeyframe keyframe{timeUs,
                                  qBound(0, volumeMilli, 2000)};
    int insertAt = params.keyframes.size();
    for (int i = 0; i < params.keyframes.size(); ++i) {
        if (params.keyframes.at(i).timeUs == timeUs) {
            insertAt = i;
            break;
        }
        if (params.keyframes.at(i).timeUs > timeUs) {
            insertAt = i;
            break;
        }
    }
    if (insertAt < params.keyframes.size()
        && params.keyframes.at(insertAt).timeUs == timeUs) {
        params.keyframes[insertAt] = keyframe;
    } else {
        params.keyframes.insert(insertAt, keyframe);
    }

    m_editor.setClipAudio(trackId, clipId, params);
    return true;
}

bool TimelineController::removeVolumeKeyframe(const QString &trackId,
                                              const QString &clipId, int index)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked)
        return false;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip || index < 0 || index >= clip->audio.keyframes.size())
        return false;

    AudioParams params = clip->audio;
    params.keyframes.removeAt(index);
    m_editor.setClipAudio(trackId, clipId, params);
    return true;
}

void TimelineController::setClipTransform(const QString &clipId, int scaleMilli,
                                          int offsetXPermille,
                                          int offsetYPermille, int opacityMilli)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    Transform transform;
    transform.scaleMilli = scaleMilli;
    transform.offsetXPermille = offsetXPermille;
    transform.offsetYPermille = offsetYPermille;
    transform.opacityMilli = opacityMilli;
    m_editor.setClipTransform(trackId, clipId, transform);
}

void TimelineController::setClipScale(const QString &clipId, int scaleMilli)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    Transform transform = clip->transform;
    transform.scaleMilli = scaleMilli;
    m_editor.setClipTransform(trackId, clipId, transform);
}

void TimelineController::setClipOpacity(const QString &clipId, int opacityMilli)
{
    QString trackId;
    if (!trackForClip(clipId, &trackId))
        return;
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return;
    Transform transform = clip->transform;
    transform.opacityMilli = opacityMilli;
    m_editor.setClipTransform(trackId, clipId, transform);
}

void TimelineController::selectClip(const QString &clipId)
{
    if (m_selectedClipId == clipId && m_selectedCueId.isEmpty())
        return;
    m_selectedClipId = clipId;
    m_selectedCueId.clear();
    m_timelineModel->selectItem(clipId);
    emit selectionChanged();
    emit selectedClipChanged();
    emit selectedCueChanged();
}

void TimelineController::selectCue(const QString &cueId)
{
    if (m_selectedCueId == cueId && m_selectedClipId.isEmpty())
        return;
    m_selectedCueId = cueId;
    m_selectedClipId.clear();
    m_timelineModel->selectItem(cueId);
    emit selectionChanged();
    emit selectedClipChanged();
    emit selectedCueChanged();
}

QString TimelineController::addCueAtPlayhead()
{
    const QString trackId = firstTrackIdOfType(TrackType::Subtitle);
    if (trackId.isEmpty()) {
        emit editFailed(QStringLiteral("没有可用的字幕轨。"));
        return {};
    }

    SubtitleCue cue;
    cue.id = newId();
    cue.startUs = m_positionUs;
    int64_t endUs = m_positionUs + 2'000'000;
    const int64_t cap = durationUs() + 2'000'000;
    if (endUs > cap)
        endUs = cap;
    if (endUs <= cue.startUs)
        endUs = cue.startUs + 2'000'000;
    cue.endUs = endUs;
    cue.text = QStringLiteral("字幕");
    cue.styleId = QStringLiteral("default");
    cue.positionXPermille = 500;
    cue.positionYPermille = 880;

    const QString id = m_editor.addSubtitleCue(trackId, cue);
    if (id.isEmpty()) {
        emit editFailed(QStringLiteral("添加字幕失败。"));
        return {};
    }
    // Selecting the new cue immediately makes Delete, the subtitle text field
    // and the inspector target the cue the user just created.
    selectCue(id);
    return id;
}

void TimelineController::setCueText(const QString &cueId, const QString &text)
{
    QString trackId;
    if (!trackForCue(cueId, &trackId)) {
        emit editFailed(QStringLiteral("未找到字幕。"));
        return;
    }
    const SubtitleCue *cue = findCue(trackId, cueId);
    if (!cue)
        return;
    SubtitleCue updated = *cue;
    updated.text = text;
    m_editor.updateSubtitleCue(trackId, cueId, updated);
}

bool TimelineController::setCuePosition(const QString &trackId,
                                        const QString &cueId, int xPermille,
                                        int yPermille)
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle || track->locked)
        return false;
    const SubtitleCue *cue = findCue(trackId, cueId);
    if (!cue)
        return false;

    // Copy the whole cue and change only the position so text, style, timing
    // and enabled state survive the single undo step.
    SubtitleCue updated = *cue;
    updated.positionXPermille = qBound(0, xPermille, 1000);
    updated.positionYPermille = qBound(0, yPermille, 1000);
    m_editor.updateSubtitleCue(trackId, cueId, updated);
    return true;
}

bool TimelineController::setCueTiming(const QString &cueId, qint64 startUs,
                                      qint64 endUs)
{
    QString trackId;
    if (!trackForCue(cueId, &trackId))
        return false;
    const Track *track = m_document.trackById(trackId);
    if (!track || track->locked)
        return false;
    const SubtitleCue *cue = findCue(trackId, cueId);
    if (!cue)
        return false;
    if (startUs < 0 || endUs <= startUs)
        return false;

    // Copy the cue so text, style, position and enabled state are preserved
    // across the single undo step.
    SubtitleCue updated = *cue;
    updated.startUs = startUs;
    updated.endUs = endUs;
    const int undoIndex = m_document.undoStack()->index();
    m_editor.updateSubtitleCue(trackId, cueId, updated);
    return m_document.undoStack()->index() != undoIndex;
}

bool TimelineController::linkSelectedClip()
{
    if (m_selectedClipId.isEmpty())
        return false;
    QString trackId;
    const Track *track = trackForClip(m_selectedClipId, &trackId);
    if (!track)
        return false;
    const Clip *selected = findClip(trackId, m_selectedClipId);
    if (!selected)
        return false;

    // Find a same-time counterpart of the opposite media type. Original A/V
    // pairs share the asset and start time, so this is the common relink flow.
    QString counterpartTrackId;
    const Clip *counterpart = nullptr;
    for (const Track &candidateTrack : m_document.project().tracks) {
        if (candidateTrack.type == track->type
            || candidateTrack.type == TrackType::Subtitle) {
            continue;
        }
        for (const Clip &candidate : candidateTrack.clips) {
            if (candidate.id == selected->id
                || candidate.timelineStartUs != selected->timelineStartUs
                || candidate.assetId != selected->assetId) {
                continue;
            }
            counterpartTrackId = candidateTrack.id;
            counterpart = &candidate;
            break;
        }
        if (counterpart)
            break;
    }
    if (!counterpart)
        return false;

    const int undoIndex = m_document.undoStack()->index();
    m_editor.linkClips({ClipLocator{trackId, m_selectedClipId},
                        ClipLocator{counterpartTrackId, counterpart->id}});
    return m_document.undoStack()->index() != undoIndex;
}

void TimelineController::unlinkSelectedClip()
{
    if (m_selectedClipId.isEmpty())
        return;
    QString trackId;
    if (!trackForClip(m_selectedClipId, &trackId))
        return;
    m_editor.unlinkClip(trackId, m_selectedClipId);
}

void TimelineController::deleteSelectedCue()
{
    if (m_selectedCueId.isEmpty())
        return;
    QString trackId;
    if (!trackForCue(m_selectedCueId, &trackId))
        return;
    m_editor.removeSubtitleCue(trackId, m_selectedCueId);
}

void TimelineController::applySubtitleStyle(const QString &styleId,
                                            const QString &cueId)
{
    const QString target = cueId.isEmpty() ? m_selectedCueId : cueId;
    QString trackId;
    if (!target.isEmpty())
        trackForCue(target, &trackId);
    if (trackId.isEmpty())
        trackId = firstTrackIdOfType(TrackType::Subtitle);
    if (trackId.isEmpty()) {
        emit editFailed(QStringLiteral("没有可用的字幕轨。"));
        return;
    }
    m_editor.applySubtitleStyle(trackId, styleId, target);
}

QString TimelineController::importSrtFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit editFailed(QStringLiteral("无法读取 SRT 文件。"));
        return {};
    }
    const QByteArray bytes = file.readAll();

    const QString trackId = firstTrackIdOfType(TrackType::Subtitle);
    if (trackId.isEmpty()) {
        emit editFailed(QStringLiteral("没有可用的字幕轨。"));
        return {};
    }

    QString error;
    const int count = m_editor.importSrt(trackId, bytes, &error);
    if (count <= 0) {
        emit editFailed(error.isEmpty() ? QStringLiteral("导入 SRT 失败。")
                                        : error);
        return {};
    }
    return QString::number(count);
}

bool TimelineController::exportSrtFile(const QString &path)
{
    const QString trackId = firstTrackIdOfType(TrackType::Subtitle);
    if (trackId.isEmpty()) {
        emit editFailed(QStringLiteral("没有可用的字幕轨。"));
        return false;
    }
    const QByteArray contents = m_editor.exportSrt(trackId);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit editFailed(QStringLiteral("无法写入 SRT 文件。"));
        return false;
    }
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        emit editFailed(QStringLiteral("写入 SRT 文件失败。"));
        return false;
    }
    if (!file.commit()) {
        emit editFailed(QStringLiteral("保存 SRT 文件失败。"));
        return false;
    }
    return true;
}

QString TimelineController::addTrack(int type)
{
    TrackType trackType;
    QString prefix;
    switch (type) {
    case 0:
        trackType = TrackType::Video;
        prefix = QStringLiteral("V");
        break;
    case 1:
        trackType = TrackType::Audio;
        prefix = QStringLiteral("A");
        break;
    case 2:
        trackType = TrackType::Subtitle;
        prefix = QStringLiteral("S");
        break;
    default:
        emit editFailed(QStringLiteral("未知的轨道类型。"));
        return {};
    }

    int count = 0;
    for (const Track &track : m_document.project().tracks) {
        if (track.type == trackType)
            ++count;
    }

    Track track;
    track.id = newId();
    track.type = trackType;
    track.name = prefix + QString::number(count + 1);
    m_editor.addTrack(track);
    if (!m_document.trackById(track.id))
        return {};
    // New media should land on the lane the user just created.
    setTargetTrackId(track.id);
    return track.id;
}

void TimelineController::removeTrack(const QString &trackId)
{
    const Track *track = m_document.trackById(trackId);
    if (!track)
        return;
    int count = 0;
    for (const Track &existing : m_document.project().tracks) {
        if (existing.type == track->type)
            ++count;
    }
    if (count <= 1) {
        emit editFailed(QStringLiteral("不能删除最后一条同类轨道。"));
        return;
    }
    m_editor.removeTrack(trackId);
}

namespace {

// Project indices of one media type in project order. Display order is project
// order for audio/subtitle and reversed for video (the last composited layer
// is the top lane), so up/down must be mapped per type instead of moving to an
// adjacent project entry, which may belong to another group.
QList<int> sameTypeProjectIndices(const QList<Track> &tracks, TrackType type)
{
    QList<int> indices;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).type == type)
            indices.append(i);
    }
    return indices;
}

} // namespace

void TimelineController::moveTrackUp(const QString &trackId)
{
    const QList<Track> &tracks = m_document.project().tracks;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).id != trackId)
            continue;
        const TrackType type = tracks.at(i).type;
        const QList<int> sameType = sameTypeProjectIndices(tracks, type);
        const int position = sameType.indexOf(i);
        if (position < 0)
            return;
        // Video lanes are displayed in reverse composition order, so moving
        // one row up means moving later in project order; other types move
        // earlier. At the group edge this is a no-op (no undo command).
        const int targetPosition =
            type == TrackType::Video ? position + 1 : position - 1;
        if (targetPosition < 0 || targetPosition >= sameType.size())
            return;
        m_editor.moveTrack(i, sameType.at(targetPosition));
        return;
    }
}

void TimelineController::moveTrackDown(const QString &trackId)
{
    const QList<Track> &tracks = m_document.project().tracks;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).id != trackId)
            continue;
        const TrackType type = tracks.at(i).type;
        const QList<int> sameType = sameTypeProjectIndices(tracks, type);
        const int position = sameType.indexOf(i);
        if (position < 0)
            return;
        const int targetPosition =
            type == TrackType::Video ? position - 1 : position + 1;
        if (targetPosition < 0 || targetPosition >= sameType.size())
            return;
        m_editor.moveTrack(i, sameType.at(targetPosition));
        return;
    }
}

void TimelineController::setTrackFlag(const QString &trackId,
                                      const QString &text,
                                      const std::function<void(Track &)> &mutate)
{
    const QList<Track> before = m_document.project().tracks;
    QList<Track> after = before;
    bool found = false;
    for (Track &track : after) {
        if (track.id == trackId) {
            mutate(track);
            found = true;
            break;
        }
    }
    if (!found || after == before)
        return;
    m_document.undoStack()->push(
        new TrackListCommand(&m_document, text, before, after));
}

void TimelineController::setTrackMuted(const QString &trackId, bool muted)
{
    setTrackFlag(trackId,
                 muted ? QStringLiteral("静音轨道") : QStringLiteral("取消静音"),
                 [muted](Track &track) { track.muted = muted; });
}

void TimelineController::setTrackSolo(const QString &trackId, bool solo)
{
    setTrackFlag(trackId,
                 solo ? QStringLiteral("独奏轨道") : QStringLiteral("取消独奏"),
                 [solo](Track &track) { track.solo = solo; });
}

void TimelineController::setTrackLocked(const QString &trackId, bool locked)
{
    setTrackFlag(trackId,
                 locked ? QStringLiteral("锁定轨道") : QStringLiteral("解锁轨道"),
                 [locked](Track &track) { track.locked = locked; });
}

} // namespace VideoEditor
