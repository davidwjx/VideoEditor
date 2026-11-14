#include "RemoveAssetCommand.h"

namespace VideoEditor {

RemoveAssetCommand::RemoveAssetCommand(ProjectDocument *document, QString assetId)
    : m_document(document)
    , m_assetId(std::move(assetId))
{
    setText(QStringLiteral("删除素材"));
    const auto &assets = m_document->project().assets;
    for (int i = 0; i < assets.size(); ++i) {
        if (assets.at(i).id == m_assetId) {
            m_asset = assets.at(i);
            m_index = i;
            break;
        }
    }
}

void RemoveAssetCommand::redo()
{
    m_document->removeAssetInternal(m_assetId);
}

void RemoveAssetCommand::undo()
{
    m_document->insertAssetInternalAt(m_index, m_asset);
}

} // namespace VideoEditor
