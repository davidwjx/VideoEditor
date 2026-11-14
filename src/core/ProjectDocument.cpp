#include "ProjectDocument.h"

#include "ProjectSerializer.h"
#include "commands/AddAssetCommand.h"
#include "commands/RemoveAssetCommand.h"

namespace VideoEditor {

ProjectDocument::ProjectDocument(QObject *parent)
    : QObject(parent)
{
    m_undoStack.setUndoLimit(100);
    connect(&m_undoStack, &QUndoStack::indexChanged, this, [this] {
        emit changed();
        emit assetsChanged();
        emit undoStateChanged();
        emit dirtyChanged();
    });
    connect(&m_undoStack, &QUndoStack::cleanChanged, this, [this] {
        emit dirtyChanged();
    });
}

void ProjectDocument::setProject(Project project, const QString &filePath)
{
    m_project = std::move(project);
    m_filePath = filePath;
    m_undoStack.clear();
    m_undoStack.setClean();
    emit changed();
    emit assetsChanged();
    emit undoStateChanged();
    emit dirtyChanged();
}

void ProjectDocument::addAsset(const Asset &asset)
{
    m_undoStack.push(new AddAssetCommand(this, asset));
}

void ProjectDocument::removeAsset(const QString &assetId)
{
    for (const Asset &asset : m_project.assets) {
        if (asset.id == assetId) {
            m_undoStack.push(new RemoveAssetCommand(this, assetId));
            return;
        }
    }
}

void ProjectDocument::setAssetProxyPath(const QString &assetId,
                                        const QString &proxyPath)
{
    for (Asset &asset : m_project.assets) {
        if (asset.id == assetId) {
            if (asset.proxyPath == proxyPath)
                return;
            asset.proxyPath = proxyPath;
            emit changed();
            emit assetsChanged();
            emit dirtyChanged();
            return;
        }
    }
}

bool ProjectDocument::save(const QString &path, QString *error)
{
    if (!ProjectSerializer::save(m_project, path, error))
        return false;
    m_filePath = path;
    m_undoStack.setClean();
    emit dirtyChanged();
    return true;
}

bool ProjectDocument::load(const QString &path, QString *error)
{
    auto project = ProjectSerializer::load(path, error);
    if (!project)
        return false;
    setProject(std::move(*project), path);
    return true;
}

void ProjectDocument::insertAssetInternal(const Asset &asset)
{
    m_project.assets.append(asset);
}

void ProjectDocument::insertAssetInternalAt(int index, const Asset &asset)
{
    const int bounded = qBound(0, index, int(m_project.assets.size()));
    m_project.assets.insert(bounded, asset);
}

void ProjectDocument::removeAssetInternal(const QString &assetId)
{
    for (int i = 0; i < m_project.assets.size(); ++i) {
        if (m_project.assets.at(i).id == assetId) {
            m_project.assets.removeAt(i);
            return;
        }
    }
}

} // namespace VideoEditor
