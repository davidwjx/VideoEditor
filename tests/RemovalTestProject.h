#pragma once
#include "Project.h"
#include "Removal.h"

namespace VideoEditor {
inline Project projectWithClip()
{
    MediaInfo info;
    info.path = QStringLiteral("C:/视频/示例.mp4");
    info.durationUs = 10'000'000;
    info.width = 1920;
    info.height = 1080;
    info.supported = true;
    info.hasVideo = true;
    info.kind = MediaKind::Video;
    Project p;
    p.assets.append(Asset{QStringLiteral("asset-1"), info.path, info, {}});
    Track track;
    track.id = QStringLiteral("track-v");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = QStringLiteral("asset-1");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 10'000'000;
    track.clips.append(clip);
    p.tracks.append(track);
    p.ensureSubtitleStyles();
    return p;
}

inline RemovalLayer sampleLayer()
{
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-1");
    layer.clipId = QStringLiteral("clip-1");
    layer.sourceInUs = 0;
    layer.sourceOutUs = 10'000'000;
    layer.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('a')), 4, 1});
    layer.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-2"), 5'000'000, QString(64, QLatin1Char('b')), 8, 1});
    layer.trackingState = RemovalTrackingState::Complete;
    layer.trackingConfidenceHash = QString(64, QLatin1Char('c'));
    layer.parameters.mode = RepairMode::Fast;
    layer.parameters.temporalStabilization = true;
    layer.parameters.stabilizationStrength = 50;
    layer.parameters.processingWidth = 1920;
    layer.parameters.processingHeight = 1080;
    layer.cacheKey = QString(64, QLatin1Char('d'));
    layer.cachePath = QStringLiteral("removal/") + layer.cacheKey;
    return layer;
}
} // namespace VideoEditor
