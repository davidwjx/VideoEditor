#pragma once

#include <cstdint>
#include <functional>
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

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
    void paste(int64_t timelineUs, const QString &targetTrackId);
    void addTrack(const Track &track);
    void removeTrack(const QString &trackId);
    void moveTrack(int fromIndex, int toIndex);

    QString addSubtitleCue(const QString &trackId, SubtitleCue cue);
    void updateSubtitleCue(const QString &trackId, const QString &cueId,
                           const SubtitleCue &cue);
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
        Clip clip;
    };

    QList<ClipLocator> linkGroup(const QString &trackId, const QString &clipId) const;
    const Clip *findClip(const QString &trackId, const QString &clipId) const;
    bool groupLocked(const QList<ClipLocator> &group) const;
    bool groupSpeed(const QList<ClipLocator> &group, int *speedMilli) const;
    void pushEdits(const QString &text, QList<TrackEdit> edits);
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
