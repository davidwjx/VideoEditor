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
    for (const TrackEdit &edit : m_edits)
        m_document->applyTrackEdit(edit.trackId, edit.afterClips, edit.afterCues);
}

void TimelineEditCommand::undo()
{
    for (const TrackEdit &edit : m_edits)
        m_document->applyTrackEdit(edit.trackId, edit.beforeClips, edit.beforeCues);
}

} // namespace VideoEditor
