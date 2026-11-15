#include "ProjectDocument.h"

#include "ProjectSerializer.h"
#include "commands/AddAssetCommand.h"
#include "commands/RemoveAssetCommand.h"
#include "commands/RemovalLayerCommand.h"

namespace VideoEditor {

ProjectDocument::ProjectDocument(QObject *parent)
    : QObject(parent)
{
    m_project.ensureDefaultTracks();
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
    m_project.ensureDefaultTracks();
    m_project.ensureSubtitleStyles();
    m_filePath = filePath;
    m_undoStack.clear();
    m_undoStack.setClean();
    emit changed();
    emit assetsChanged();
    emit tracksChanged();
    emit timelineChanged();
    emit projectReplaced();
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

void ProjectDocument::applyTrackEdit(const QString &trackId,
                                     const QList<Clip> &clips,
                                     const QList<SubtitleCue> &cues)
{
    for (Track &track : m_project.tracks) {
        if (track.id != trackId)
            continue;
        track.clips = clips;
        track.cues = cues;
        emit changed();
        emit timelineChanged();
        emit dirtyChanged();
        return;
    }
}

void ProjectDocument::applyTrackEdits(const QList<TrackEdit> &edits)
{
    bool applied = false;
    for (const TrackEdit &edit : edits) {
        for (Track &track : m_project.tracks) {
            if (track.id != edit.trackId)
                continue;
            track.clips = edit.afterClips;
            track.cues = edit.afterCues;
            applied = true;
            break;
        }
    }
    if (!applied)
        return;
    emit changed();
    emit timelineChanged();
    emit dirtyChanged();
}

void ProjectDocument::replaceTracks(const QList<Track> &tracks)
{
    m_project.tracks = tracks;
    emit changed();
    emit tracksChanged();
    emit timelineChanged();
    emit dirtyChanged();
}

Track *ProjectDocument::trackById(const QString &id)
{
    for (Track &track : m_project.tracks) {
        if (track.id == id)
            return &track;
    }
    return nullptr;
}

const Track *ProjectDocument::trackById(const QString &id) const
{
    for (const Track &track : m_project.tracks) {
        if (track.id == id)
            return &track;
    }
    return nullptr;
}

void ProjectDocument::applyRemovalLayerEdit(const RemovalLayerEdit &edit,
                                            const QString &text)
{
    m_undoStack.push(new RemovalLayerCommand(this, text, edit));
}

const RemovalLayer *ProjectDocument::removalLayerById(const QString &id) const
{
    return m_project.removalLayerById(id);
}

RemovalLayer *ProjectDocument::removalLayerById(const QString &id)
{
    return m_project.removalLayerById(id);
}

void ProjectDocument::upsertRemovalLayerInternal(const RemovalLayer &layer)
{
    for (RemovalLayer &existing : m_project.removalLayers) {
        if (existing.id == layer.id) {
            existing = layer;
            return;
        }
    }
    m_project.removalLayers.append(layer);
}

void ProjectDocument::removeRemovalLayerInternal(const QString &layerId)
{
    for (int i = 0; i < m_project.removalLayers.size(); ++i) {
        if (m_project.removalLayers[i].id == layerId) {
            m_project.removalLayers.removeAt(i);
            break;
        }
    }
    for (Track &track : m_project.tracks) {
        for (Clip &clip : track.clips) {
            if (clip.removalLayerId == layerId)
                clip.removalLayerId.clear();
        }
    }
}

void ProjectDocument::setClipRemovalLayerInternal(const QString &clipId,
                                                  const QString &layerId)
{
    for (Track &track : m_project.tracks) {
        for (Clip &clip : track.clips) {
            if (clip.id == clipId) {
                clip.removalLayerId = layerId;
                return;
            }
        }
    }
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
