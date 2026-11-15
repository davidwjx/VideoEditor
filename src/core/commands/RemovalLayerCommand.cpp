#include "RemovalLayerCommand.h"

#include <utility>

#include "ProjectDocument.h"

namespace VideoEditor {

RemovalLayerCommand::RemovalLayerCommand(ProjectDocument *document,
                                         const QString &text,
                                         RemovalLayerEdit edit)
    : m_document(document)
    , m_edit(std::move(edit))
{
    setText(text);
}

void RemovalLayerCommand::redo()
{
    apply(m_edit.after);
}

void RemovalLayerCommand::undo()
{
    apply(m_edit.before);
}

void RemovalLayerCommand::apply(const std::optional<RemovalLayer> &layer)
{
    if (layer) {
        m_document->upsertRemovalLayerInternal(*layer);
        m_document->setClipRemovalLayerInternal(layer->clipId, layer->id);
    } else {
        // The target state has no layer. Remove the layer from the other side
        // of the transition: undoing an add uses after, redoing a delete uses
        // before. With neither side present only the clip link is cleared.
        const QString layerId = m_edit.before ? m_edit.before->id
                               : m_edit.after  ? m_edit.after->id
                                               : QString();
        if (!layerId.isEmpty())
            m_document->removeRemovalLayerInternal(layerId);
        else
            m_document->setClipRemovalLayerInternal(m_edit.clipId, QString());
    }
    emit m_document->changed();
    emit m_document->timelineChanged();
    emit m_document->removalLayersChanged();
    emit m_document->dirtyChanged();
}

} // namespace VideoEditor
