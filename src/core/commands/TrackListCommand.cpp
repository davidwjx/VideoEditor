#include "TrackListCommand.h"

#include <utility>

#include "ProjectDocument.h"

namespace VideoEditor {

TrackListCommand::TrackListCommand(ProjectDocument *document,
                                   const QString &text,
                                   QList<Track> before,
                                   QList<Track> after)
    : m_document(document)
    , m_before(std::move(before))
    , m_after(std::move(after))
{
    setText(text);
}

void TrackListCommand::redo()
{
    m_document->replaceTracks(m_after);
}

void TrackListCommand::undo()
{
    m_document->replaceTracks(m_before);
}

} // namespace VideoEditor
