#pragma once

#include <QList>
#include <QString>
#include <QUndoCommand>

#include "Timeline.h"

namespace VideoEditor {

class ProjectDocument;

struct TrackEdit {
    QString trackId;
    QList<Clip> beforeClips;
    QList<Clip> afterClips;
    QList<SubtitleCue> beforeCues;
    QList<SubtitleCue> afterCues;
};

class TimelineEditCommand : public QUndoCommand {
public:
    TimelineEditCommand(ProjectDocument *document, const QString &text,
                        QList<TrackEdit> edits);
    void redo() override;
    void undo() override;

private:
    ProjectDocument *m_document;
    QList<TrackEdit> m_edits;
};

} // namespace VideoEditor
