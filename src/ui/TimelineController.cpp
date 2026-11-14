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

    connect(&m_document, &ProjectDocument::timelineChanged, this,
            &TimelineController::refreshFromDocument);
    connect(&m_document, &ProjectDocument::tracksChanged, this,
            &TimelineController::refreshFromDocument);
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
    emit durationChanged();
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

void TimelineController::seekUs(qint64 positionUs)
{
    const qint64 clamped = positionUs < 0 ? 0 : positionUs;
    if (clamped == m_positionUs)
        return;
    m_positionUs = clamped;
    emit positionChanged();
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
    QList<int64_t> candidates;
    candidates.append(0);
    candidates.append(m_positionUs);
    candidates.append(durationUs());
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &clip : track.clips) {
            if (!excludeClipId.isEmpty() && clip.id == excludeClipId)
                continue;
            candidates.append(clip.timelineStartUs);
            candidates.append(clip.timelineEndUs());
        }
    }
    return candidates;
}

qint64 TimelineController::snapUs(qint64 timeUs) const
{
    if (!m_snapEnabled || m_pixelsPerSecond <= 0.0)
        return timeUs;
    const int64_t threshold = qMax<int64_t>(
        1, std::llround(8.0 / m_pixelsPerSecond * 1'000'000.0));
    return TimelineEditor::snapTime(timeUs, threshold, snapCandidates()).timeUs;
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
        if (requested && requested->type == TrackType::Audio)
            audioTrackId = requested->id;
        if (audioTrackId.isEmpty())
            audioTrackId = firstTrackIdOfType(TrackType::Audio);
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
    if (!videoClipId.isEmpty() && added.contains(videoClipId))
        return videoClipId;
    if (!audioClipId.isEmpty() && added.contains(audioClipId))
        return audioClipId;
    if (!added.isEmpty())
        return added.first();
    emit editFailed(QStringLiteral("添加片段失败。"));
    return {};
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
    if (!m_selectedClipId.isEmpty()) {
        QString trackId;
        const Clip *clip = nullptr;
        if (trackForClip(m_selectedClipId, &trackId))
            clip = findClip(trackId, m_selectedClipId);
        if (clip && m_positionUs > clip->timelineStartUs
            && m_positionUs < clip->timelineEndUs()) {
            locators.append(ClipLocator{trackId, m_selectedClipId});
        }
    } else {
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

void TimelineController::copySelected()
{
    if (m_selectedClipId.isEmpty())
        return;
    QString trackId;
    if (!trackForClip(m_selectedClipId, &trackId))
        return;
    m_editor.copy({ClipLocator{trackId, m_selectedClipId}});
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

void TimelineController::selectClip(const QString &clipId)
{
    if (m_selectedClipId == clipId && m_selectedCueId.isEmpty())
        return;
    m_selectedClipId = clipId;
    m_selectedCueId.clear();
    m_timelineModel->selectItem(clipId);
    emit selectionChanged();
}

void TimelineController::selectCue(const QString &cueId)
{
    if (m_selectedCueId == cueId && m_selectedClipId.isEmpty())
        return;
    m_selectedCueId = cueId;
    m_selectedClipId.clear();
    m_timelineModel->selectItem(cueId);
    emit selectionChanged();
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

void TimelineController::addTrack(int type)
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
        return;
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

void TimelineController::moveTrackUp(const QString &trackId)
{
    const QList<Track> &tracks = m_document.project().tracks;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).id != trackId)
            continue;
        if (i > 0)
            m_editor.moveTrack(i, i - 1);
        return;
    }
}

void TimelineController::moveTrackDown(const QString &trackId)
{
    const QList<Track> &tracks = m_document.project().tracks;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).id != trackId)
            continue;
        if (i + 1 < tracks.size())
            m_editor.moveTrack(i, i + 1);
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
