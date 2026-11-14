#include "TimelineEditCommand.h"

#include <utility>

#include "ProjectDocument.h"

namespace VideoEditor {

TimelineEditCommand::TimelineEditCommand(ProjectDocument *document,
                                         const QString &text,
                                         QList<TrackEdit> edits)
    : m_document(document)
    , m_edits(std::move(edits))
{
    setText(text);
}

void TimelineEditCommand::redo()
{
    m_document->applyTrackEdits(m_edits);
}

void TimelineEditCommand::undo()
{
    QList<TrackEdit> before = m_edits;
    for (TrackEdit &edit : before) {
        edit.afterClips = edit.beforeClips;
        edit.afterCues = edit.beforeCues;
    }
    m_document->applyTrackEdits(before);
}

} // namespace VideoEditor
