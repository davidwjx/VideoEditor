#pragma once

#include <QUndoCommand>

#include "ProjectDocument.h"

namespace VideoEditor {

class AddAssetCommand : public QUndoCommand {
public:
    AddAssetCommand(ProjectDocument *document, Asset asset)
        : m_document(document)
        , m_asset(std::move(asset))
    {
        setText(QStringLiteral("添加素材"));
    }

    void redo() override { m_document->insertAssetInternal(m_asset); }
    void undo() override { m_document->removeAssetInternal(m_asset.id); }

private:
    ProjectDocument *m_document;
    Asset m_asset;
};

} // namespace VideoEditor
