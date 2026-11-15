#pragma once

#include <cstdint>
#include <functional>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "ProjectDocument.h"
#include "TimelineEditor.h"
#include "TimelineModel.h"
#include "TrackListModel.h"

namespace VideoEditor {

class TimelineController : public QObject {
    Q_OBJECT
    Q_PROPERTY(VideoEditor::TimelineModel *timelineModel READ timelineModel CONSTANT)
    Q_PROPERTY(VideoEditor::TrackListModel *trackModel READ trackModel CONSTANT)
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY durationChanged)
    Q_PROPERTY(QString timecode READ timecode NOTIFY positionChanged)
    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond
                   NOTIFY viewChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY viewChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCueId READ selectedCueId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedClip READ selectedClip
                   NOTIFY selectedClipChanged)
    Q_PROPERTY(QVariantMap selectedCue READ selectedCue
                   NOTIFY selectedCueChanged)
    // Track that receives newly added media and track operations. Empty means
    // "first track of the matching type". Kept out of the document on purpose:
    // it is transient UI state and must not create undo steps.
    Q_PROPERTY(QString targetTrackId READ targetTrackId WRITE setTargetTrackId
                   NOTIFY targetTrackChanged)

public:
    explicit TimelineController(ProjectDocument &document, QObject *parent = nullptr);

    TimelineModel *timelineModel() const { return m_timelineModel; }
    TrackListModel *trackModel() const { return m_trackModel; }
    qint64 positionUs() const { return m_positionUs; }
    qint64 durationUs() const;
    QString timecode() const;
    double pixelsPerSecond() const { return m_pixelsPerSecond; }
    void setPixelsPerSecond(double value);
    bool snapEnabled() const { return m_snapEnabled; }
    void setSnapEnabled(bool enabled);
    QString selectedClipId() const { return m_selectedClipId; }
    QString selectedCueId() const { return m_selectedCueId; }
    QVariantMap selectedClip() const;
    QVariantMap selectedCue() const;
    QString targetTrackId() const { return m_targetTrackId; }
    void setTargetTrackId(const QString &trackId);

    Q_INVOKABLE void seekUs(qint64 positionUs);
    Q_INVOKABLE void stepFrames(int delta);
    Q_INVOKABLE qint64 snapUs(qint64 timeUs) const;
    // Snaps a time to the project frame grid (no-op for an invalid fps).
    Q_INVOKABLE qint64 frameQuantizeUs(qint64 timeUs) const;
    // Accepts "HH:MM:SS:FF", "HH:MM:SS;FF", "MM:SS.mmm" or plain seconds and
    // moves the playhead. Returns false when the text cannot be parsed.
    Q_INVOKABLE bool seekTimecode(const QString &text);
    Q_INVOKABLE QString addAssetToTimeline(const QString &assetId,
                                          const QString &trackId,
                                          qint64 atUs);
    Q_INVOKABLE void moveClip(const QString &clipId, qint64 newStartUs);
    Q_INVOKABLE void trimStart(const QString &clipId, qint64 newStartUs);
    Q_INVOKABLE void trimEnd(const QString &clipId, qint64 newEndUs);
    Q_INVOKABLE void splitAtPlayhead();
    Q_INVOKABLE void deleteSelected(bool ripple);
    // Deletes whatever is selected: a subtitle cue takes precedence, otherwise
    // the selected clip. One shortcut path for both selection types.
    Q_INVOKABLE void deleteSelection(bool ripple = false);
    Q_INVOKABLE void copySelected();
    Q_INVOKABLE void pasteAtPlayhead();
    Q_INVOKABLE void setClipSpeed(const QString &clipId, int speedMilli,
                                  bool preservePitch);
    Q_INVOKABLE void setClipVolume(const QString &clipId, int volumeMilli);
    Q_INVOKABLE void setClipPan(const QString &clipId, int panMilli);
    Q_INVOKABLE void setClipFades(const QString &clipId, qint64 fadeInUs,
                                  qint64 fadeOutUs);
    // Single-field fade setters preserve the other fade and all other audio
    // parameters, so dragging one inspector slider cannot zero its sibling.
    Q_INVOKABLE void setClipFadeIn(const QString &clipId, qint64 fadeInUs);
    Q_INVOKABLE void setClipFadeOut(const QString &clipId, qint64 fadeOutUs);
    // Volume keyframes are authored as a discrete, time-sorted list. timeUs is
    // relative to the clip start; the same time replaces the existing entry.
    Q_INVOKABLE bool addVolumeKeyframe(const QString &trackId,
                                       const QString &clipId, int64_t timeUs,
                                       int volumeMilli);
    Q_INVOKABLE bool removeVolumeKeyframe(const QString &trackId,
                                          const QString &clipId, int index);
    Q_INVOKABLE void setClipTransform(const QString &clipId, int scaleMilli,
                                      int offsetXPermille, int offsetYPermille,
                                      int opacityMilli);
    // Single-field setters: preserve every transform field the user did not
    // touch (the full setter above replaces the whole struct).
    Q_INVOKABLE void setClipScale(const QString &clipId, int scaleMilli);
    Q_INVOKABLE void setClipOpacity(const QString &clipId, int opacityMilli);
    Q_INVOKABLE void selectClip(const QString &clipId);
    Q_INVOKABLE void selectCue(const QString &cueId);
    Q_INVOKABLE QString addCueAtPlayhead();
    Q_INVOKABLE void setCueText(const QString &cueId, const QString &text);
    Q_INVOKABLE bool setCuePosition(const QString &trackId,
                                    const QString &cueId, int xPermille,
                                    int yPermille);
    // Whole-cue timing update: text, style, position and enabled state survive.
    Q_INVOKABLE bool setCueTiming(const QString &cueId, qint64 startUs,
                                  qint64 endUs);
    Q_INVOKABLE void deleteSelectedCue();
    // Re-links the selected clip with a same-time A/V counterpart, or breaks
    // the selected clip's link. Both are single undo steps.
    Q_INVOKABLE bool linkSelectedClip();
    Q_INVOKABLE void unlinkSelectedClip();
    Q_INVOKABLE void applySubtitleStyle(const QString &styleId,
                                        const QString &cueId = {});
    Q_INVOKABLE QString importSrtFile(const QString &path);
    Q_INVOKABLE bool exportSrtFile(const QString &path);
    // Returns the new track id (empty on failure) and makes it the target.
    Q_INVOKABLE QString addTrack(int type);
    Q_INVOKABLE void removeTrack(const QString &trackId);
    Q_INVOKABLE void moveTrackUp(const QString &trackId);
    Q_INVOKABLE void moveTrackDown(const QString &trackId);
    Q_INVOKABLE void setTrackMuted(const QString &trackId, bool muted);
    Q_INVOKABLE void setTrackSolo(const QString &trackId, bool solo);
    Q_INVOKABLE void setTrackLocked(const QString &trackId, bool locked);

    // Self-test/test hook: adds a fully specified clip without the asset-based
    // linked-pair logic. Public for AppController self-tests; not usable from
    // QML because it is deliberately not Q_INVOKABLE.
    QString addClipForTest(const QString &trackId, const Clip &clip);

signals:
    void positionChanged();
    void durationChanged();
    void viewChanged();
    void selectionChanged();
    void selectedClipChanged();
    void selectedCueChanged();
    void targetTrackChanged();
    void editFailed(QString message);

private:
    Track *trackForClip(const QString &clipId, QString *trackId = nullptr);
    Track *trackForCue(const QString &cueId, QString *trackId = nullptr);
    QList<int64_t> snapCandidates(const QString &excludeClipId = {}) const;
    void refreshFromDocument();
    QString firstTrackIdOfType(TrackType type) const;
    const Clip *findClip(const QString &trackId, const QString &clipId) const;
    const SubtitleCue *findCue(const QString &trackId,
                               const QString &cueId) const;
    void setTrackFlag(const QString &trackId, const QString &text,
                      const std::function<void(Track &)> &mutate);

    ProjectDocument &m_document;
    TimelineEditor m_editor;
    TimelineModel *m_timelineModel = nullptr;
    TrackListModel *m_trackModel = nullptr;
    qint64 m_positionUs = 0;
    double m_pixelsPerSecond = 80.0;
    bool m_snapEnabled = true;
    QString m_selectedClipId;
    QString m_selectedCueId;
    QString m_targetTrackId;
    qint64 m_lastDurationUs = -1;
};

} // namespace VideoEditor
