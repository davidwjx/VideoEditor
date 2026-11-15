#include "Project.h"

namespace VideoEditor {

void Project::ensureDefaultTracks()
{
    auto hasType = [this](TrackType type) {
        for (const Track &track : tracks) {
            if (track.type == type)
                return true;
        }
        return false;
    };

    if (!hasType(TrackType::Video)) {
        Track track;
        track.id = QStringLiteral("track-v1");
        track.type = TrackType::Video;
        track.name = QStringLiteral("V1");
        tracks.append(track);
    }
    if (!hasType(TrackType::Audio)) {
        Track track;
        track.id = QStringLiteral("track-a1");
        track.type = TrackType::Audio;
        track.name = QStringLiteral("A1");
        tracks.append(track);
    }
    if (!hasType(TrackType::Subtitle)) {
        Track track;
        track.id = QStringLiteral("track-s1");
        track.type = TrackType::Subtitle;
        track.name = QStringLiteral("S1");
        tracks.append(track);
    }
    ensureSubtitleStyles();
}

void Project::ensureSubtitleStyles()
{
    // Merge by id: an existing project style (possibly customized) is kept,
    // while builtin presets the project never had are appended. This makes
    // "boxed" and "bold-outline" selectable for new projects, v1 migrations
    // and v2 files written before the presets existed.
    for (const SubtitleStyle &builtin : builtinSubtitleStyles()) {
        bool found = false;
        for (const SubtitleStyle &existing : subtitleStyles) {
            if (existing.id == builtin.id) {
                found = true;
                break;
            }
        }
        if (!found)
            subtitleStyles.append(builtin);
    }
}

const Asset *Project::assetById(const QString &id) const
{
    for (const Asset &asset : assets) {
        if (asset.id == id)
            return &asset;
    }
    return nullptr;
}

const RemovalLayer *Project::removalLayerById(const QString &id) const
{
    for (const RemovalLayer &layer : removalLayers) {
        if (layer.id == id)
            return &layer;
    }
    return nullptr;
}

RemovalLayer *Project::removalLayerById(const QString &id)
{
    for (RemovalLayer &layer : removalLayers) {
        if (layer.id == id)
            return &layer;
    }
    return nullptr;
}

} // namespace VideoEditor
