#pragma once

#include <QList>
#include <QString>
#include <QUndoCommand>

#include "Timeline.h"

namespace VideoEditor {

class ProjectDocument;

class TrackListCommand : public QUndoCommand {
public:
    TrackListCommand(ProjectDocument *document, const QString &text,
                     QList<Track> before, QList<Track> after);
    void redo() override;
    void undo() override;

private:
    ProjectDocument *m_document;
    QList<Track> m_before;
    QList<Track> m_after;
};

} // namespace VideoEditor
