#pragma once

#include <cstdint>
#include <optional>

#include <QJsonObject>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

class Project;
struct ExportSettings;

// Resume state for an interrupted export. A checkpoint is written atomically
// (QSaveFile) after every completed segment. `load` rejects unknown versions so
// an older format is discarded and the export restarts from the beginning
// instead of mixing incompatible parts.
struct ExportCheckpoint {
    static constexpr int kVersion = 1;

    int version = kVersion;
    QString projectHash;
    QString encoderKey;
    int width = 0;
    int height = 0;
    Rational fps{0, 1};
    int sampleRate = 0;
    int64_t totalFrames = 0;
    int64_t totalSamples = 0;
    int64_t segmentSamples = 0;
    int segmentCount = 0;
    int completedSegments = 0;
    int64_t bytesWritten = 0;
    QString lastError;

    QJsonObject toJson() const;
    static std::optional<ExportCheckpoint> fromJson(const QJsonObject &json,
                                                    QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;
    static std::optional<ExportCheckpoint> load(const QString &path,
                                                QString *error = nullptr);
};

// Hash of the frozen project snapshot plus every export setting. A different
// hash means the existing parts belong to another timeline/preset and must be
// discarded.
QString exportProjectHash(const Project &project, const ExportSettings &settings);

// <output>.parts -- one directory per running export.
QString partsDirectoryFor(const QString &outputPath);
// <partsDir>/segment_00000.mp4 -- independent, trailer-complete video part.
QString segmentPath(const QString &partsDir, int segmentIndex);
// <output>.assembling.mp4 -- muxed output, renamed into place on success.
QString assemblyTempPathFor(const QString &outputPath);
void clearParts(const QString &partsDir);

struct SegmentCounts {
    int64_t videoPackets = 0;
};

// Verifies a part is a readable MP4 whose first video packet is a keyframe and
// counts its video packets. Used to validate checkpoint claims and to compare
// encoder parameters before assembly. Audio is a separate continuous track and
// is never inspected here.
bool inspectSegment(const QString &path, SegmentCounts *counts,
                    QString *error = nullptr);

} // namespace VideoEditor
