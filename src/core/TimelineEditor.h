#pragma once

#include <cstdint>
#include <functional>
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "Removal.h"
#include "Timeline.h"
#include "commands/TimelineEditCommand.h" // TrackEdit is part of this API

namespace VideoEditor {

class ProjectDocument;

struct ClipLocator {
    QString trackId;
    QString clipId;
    bool operator==(const ClipLocator &) const = default;
};

struct SnapResult {
    int64_t timeUs = 0;
    bool snapped = false;
};

class TimelineEditor {
public:
    explicit TimelineEditor(ProjectDocument &document);

    QStringList addClips(const QList<QPair<QString, Clip>> &trackAndClip);
    QString addClip(const QString &trackId, Clip clip);
    void moveClip(const QString &trackId, const QString &clipId, int64_t newStartUs);
    void trimStart(const QString &trackId, const QString &clipId, int64_t newStartUs);
    void trimEnd(const QString &trackId, const QString &clipId, int64_t newEndUs);
    void splitClip(const QString &trackId, const QString &clipId, int64_t timelineUs);
    void deleteClip(const QString &trackId, const QString &clipId, bool ripple);
    void setClipSpeed(const QString &trackId, const QString &clipId,
                      int speedMilli, bool preservePitch);
    void setClipAudio(const QString &trackId, const QString &clipId,
                      const AudioParams &params);
    void setClipTransform(const QString &trackId, const QString &clipId,
                          const Transform &transform);
    void setClipEnabled(const QString &trackId, const QString &clipId, bool enabled);
    void linkClips(const QList<ClipLocator> &clips);
    void unlinkClip(const QString &trackId, const QString &clipId);
    void copy(const QList<ClipLocator> &clips);
    bool hasClipboard() const { return !m_clipboard.isEmpty(); }
    // Loading another project invalidates clipboard source track ids.
    void clearClipboard() { m_clipboard.clear(); }
    void paste(int64_t timelineUs, const QString &targetTrackId);
    void addTrack(const Track &track);
    void removeTrack(const QString &trackId);
    void moveTrack(int fromIndex, int toIndex);

    // Generates an id when empty and rejects duplicate ids, invalid ranges and
    // empty text. Returns the cue id, or an empty string when rejected.
    QString addSubtitleCue(const QString &trackId, SubtitleCue cue);
    // Replaces the entire cue (identified by cueId) and keeps the track's cues
    // sorted by startUs. Returns without an undo step when the cue is missing.
    void updateSubtitleCue(const QString &trackId, const QString &cueId,
                           const SubtitleCue &cue);
    // Convenience update for the start/end fields: preserves every other cue
    // field and keeps the track sorted. Invalid ranges are ignored.
    void setCueTiming(const QString &trackId, const QString &cueId,
                      int64_t startUs, int64_t endUs);
    void removeSubtitleCue(const QString &trackId, const QString &cueId);
    void applySubtitleStyle(const QString &trackId, const QString &styleId,
                            const QString &cueId = {});
    int importSrt(const QString &trackId, const QByteArray &bytes,
                  QString *error = nullptr);
    QByteArray exportSrt(const QString &trackId) const;

    static SnapResult snapTime(int64_t timeUs, int64_t thresholdUs,
                               const QList<int64_t> &candidates);

private:
    struct ClipboardEntry {
        QString sourceTrackId;
        TrackType sourceTrackType = TrackType::Video;
        Clip clip;
    };

    QList<ClipLocator> linkGroup(const QString &trackId, const QString &clipId) const;
    const Clip *findClip(const QString &trackId, const QString &clipId) const;
    bool groupLocked(const QList<ClipLocator> &group) const;
    bool groupSpeed(const QList<ClipLocator> &group, int *speedMilli) const;
    QList<RemovalLayer> removalLayersForClips(
        const QList<ClipLocator> &locators) const;
    QList<RemovalLayer> removalLayersForTrack(const Track &track) const;
    // One undo macro: optional track snapshot edits followed by removal-layer
    // adds/removes. Keeps clip/layer references one-to-one across undo.
    void pushEdits(const QString &text, QList<TrackEdit> edits,
                   const QList<RemovalLayer> &addedLayers = {},
                   const QList<RemovalLayer> &removedLayers = {});
    QList<TrackEdit> editsForClips(const QList<ClipLocator> &locators,
                                   const std::function<void(Clip &)> &mutate) const;
    QList<TrackEdit> editsForCues(
        const QString &trackId,
        const std::function<void(QList<SubtitleCue> &)> &mutate) const;
    QString uniqueLinkId() const;

    ProjectDocument &m_document;
    QList<ClipboardEntry> m_clipboard;
};

} // namespace VideoEditor
