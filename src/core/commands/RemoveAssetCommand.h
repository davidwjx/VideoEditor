#pragma once

#include <QUndoCommand>

#include "ProjectDocument.h"

namespace VideoEditor {

class RemoveAssetCommand : public QUndoCommand {
public:
    RemoveAssetCommand(ProjectDocument *document, QString assetId);
    void redo() override;
    void undo() override;

private:
    ProjectDocument *m_document;
    QString m_assetId;
    Asset m_asset;
    int m_index = 0;
};

} // namespace VideoEditor
