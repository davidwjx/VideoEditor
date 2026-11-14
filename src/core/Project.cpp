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
    bool hasDefaultStyle = false;
    for (const SubtitleStyle &style : subtitleStyles) {
        if (style.id == QStringLiteral("default"))
            hasDefaultStyle = true;
    }
    if (!hasDefaultStyle)
        subtitleStyles.append(SubtitleStyle{});
}

const Asset *Project::assetById(const QString &id) const
{
    for (const Asset &asset : assets) {
        if (asset.id == id)
            return &asset;
    }
    return nullptr;
}

} // namespace VideoEditor
