#pragma once

#include <cstdint>
#include <functional>
#include <QObject>
#include <QString>

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

    Q_INVOKABLE void seekUs(qint64 positionUs);
    Q_INVOKABLE void stepFrames(int delta);
    Q_INVOKABLE qint64 snapUs(qint64 timeUs) const;
    Q_INVOKABLE QString addAssetToTimeline(const QString &assetId,
                                          const QString &trackId,
                                          qint64 atUs);
    Q_INVOKABLE void moveClip(const QString &clipId, qint64 newStartUs);
    Q_INVOKABLE void trimStart(const QString &clipId, qint64 newStartUs);
    Q_INVOKABLE void trimEnd(const QString &clipId, qint64 newEndUs);
    Q_INVOKABLE void splitAtPlayhead();
    Q_INVOKABLE void deleteSelected(bool ripple);
    Q_INVOKABLE void copySelected();
    Q_INVOKABLE void pasteAtPlayhead();
    Q_INVOKABLE void setClipSpeed(const QString &clipId, int speedMilli,
                                  bool preservePitch);
    Q_INVOKABLE void setClipVolume(const QString &clipId, int volumeMilli);
    Q_INVOKABLE void setClipPan(const QString &clipId, int panMilli);
    Q_INVOKABLE void setClipFades(const QString &clipId, qint64 fadeInUs,
                                  qint64 fadeOutUs);
    Q_INVOKABLE void setClipTransform(const QString &clipId, int scaleMilli,
                                      int offsetXPermille, int offsetYPermille,
                                      int opacityMilli);
    Q_INVOKABLE void selectClip(const QString &clipId);
    Q_INVOKABLE void selectCue(const QString &cueId);
    Q_INVOKABLE QString addCueAtPlayhead();
    Q_INVOKABLE void setCueText(const QString &cueId, const QString &text);
    Q_INVOKABLE void deleteSelectedCue();
    Q_INVOKABLE void applySubtitleStyle(const QString &styleId,
                                        const QString &cueId = {});
    Q_INVOKABLE QString importSrtFile(const QString &path);
    Q_INVOKABLE bool exportSrtFile(const QString &path);
    Q_INVOKABLE void addTrack(int type);
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
    qint64 m_lastDurationUs = -1;
};

} // namespace VideoEditor
