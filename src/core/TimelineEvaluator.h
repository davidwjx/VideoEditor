#pragma once

#include <cstdint>
#include <QList>
#include <QString>

#include "Project.h"
#include "Timeline.h"

namespace VideoEditor {

struct VideoLayer {
    QString trackId;
    QString clipId;
    QString assetId;
    int trackIndex = 0;
    int64_t localTimeUs = 0;
    int64_t sourceTimeUs = 0;
    Transform transform;
    // Non-empty only while the clip's enabled removal layer covers
    // sourceTimeUs. The compositor uses it to look up a cached repair patch.
    QString removalLayerId;

    bool operator==(const VideoLayer &) const = default;
};

struct AudioLayer {
    QString trackId;
    QString clipId;
    int trackIndex = 0;
    int64_t localTimeUs = 0;
    int64_t sourceTimeUs = 0;
    bool trackMuted = false;
    bool trackSolo = false;
    Clip clip;

    bool operator==(const AudioLayer &) const = default;
};

struct SubtitleLayer {
    QString trackId;
    QString cueId;
    QString text;
    SubtitleStyle style;
    int positionXPermille = 500;
    int positionYPermille = 880;
    int64_t localTimeUs = 0;

    bool operator==(const SubtitleLayer &) const = default;
};

struct FrameComposition {
    int64_t timelineUs = 0;
    QList<VideoLayer> video;        // bottom to top
    QList<SubtitleLayer> subtitles; // bottom to top, always drawn last
};

class TimelineEvaluator {
public:
    explicit TimelineEvaluator(const Project &project) : m_project(project) {}

    FrameComposition frameAt(int64_t timelineUs) const;
    QList<AudioLayer> audioRange(int64_t startUs, int64_t endUs) const;
    // True when any enabled audio track is soloed. Solo is a project-wide
    // state: a solo track that has a gap must still mute every other track in
    // that gap, so this is intentionally independent of the current block.
    bool anySolo() const;
    int64_t durationUs() const;

private:
    const SubtitleStyle &styleFor(const QString &styleId) const;
    const Project &m_project;
};

} // namespace VideoEditor
