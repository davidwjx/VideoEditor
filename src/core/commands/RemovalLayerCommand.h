#pragma once

#include <optional>
#include <QString>
#include <QUndoCommand>
#include "Removal.h"

namespace VideoEditor {
class ProjectDocument;

struct RemovalLayerEdit {
    QString clipId;
    std::optional<RemovalLayer> before;
    std::optional<RemovalLayer> after;
};

class RemovalLayerCommand : public QUndoCommand {
public:
    RemovalLayerCommand(ProjectDocument *document, const QString &text,
                        RemovalLayerEdit edit);
    void redo() override;
    void undo() override;

private:
    void apply(const std::optional<RemovalLayer> &layer);
    ProjectDocument *m_document;
    RemovalLayerEdit m_edit;
};
} // namespace VideoEditor
