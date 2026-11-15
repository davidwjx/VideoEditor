#pragma once

#include <cstdint>
#include <optional>

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>

#include "TimeBase.h"
#include "Timeline.h"

namespace VideoEditor {

struct Project;

enum class ExportVideoCodec { Mpeg4Part2, H264, H265 };
enum class ExportQuality { Low, Medium, High };
enum class ExportRateControl { Bitrate, Quality };

struct ExportSettings {
    ExportVideoCodec codec = ExportVideoCodec::H264;
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    ExportRateControl rateControl = ExportRateControl::Quality;
    ExportQuality quality = ExportQuality::Medium;
    // 0 means "derive from quality"; otherwise 100..200000 kbps.
    int bitrateKbps = 0;
    bool preferHardware = true;
    bool burnSubtitles = true;
    bool writeSrt = false;
    QString outputPath;
    QString srtPath;
    // Empty = automatic selection by EncoderCatalog.
    QString encoderId;
    // Advanced/test hook. Segment boundaries are multiples of 1024 samples.
    int segmentTargetUs = 8'000'000;

    bool operator==(const ExportSettings &) const = default;
};

struct ExportPreset {
    QString id;
    QString name;
    ExportVideoCodec codec = ExportVideoCodec::H264;
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    bool matchTimeline = false;
    ExportQuality quality = ExportQuality::Medium;
    int bitrateKbps = 0;
};

struct ExportTiming {
    int64_t totalFrames = 0;
    int64_t totalSamples = 0;
    int64_t segmentSamples = 0; // always a multiple of 1024
    int segmentCount = 0;
    int sampleRate = 48000;
};

QString exportCodecToString(ExportVideoCodec codec);
QString exportCodecDisplayName(ExportVideoCodec codec);
QString exportCodecFfmpegName(ExportVideoCodec codec);
bool exportCodecFromString(const QString &text, ExportVideoCodec *codec);

QList<ExportPreset> builtinExportPresets();
ExportSettings settingsFromPreset(const ExportPreset &preset,
                                  const TimelineSettings &timeline);
int effectiveBitrateKbps(const ExportSettings &settings);

// Frame PTS in microseconds, rounded half up. CFR only.
int64_t framePtsUs(int64_t frame, const Rational &fps);
// Smallest frame index whose rounded PTS is >= timeUs.
int64_t firstVideoFrameAtOrAfter(int64_t timeUs, const Rational &fps);

ExportTiming computeExportTiming(int64_t durationUs, const Rational &fps,
                                 int sampleRate, int segmentTargetUs);
int64_t segmentStartUs(const ExportTiming &timing, int segmentIndex);
void videoFrameRangeForSegment(const ExportTiming &timing, const Rational &fps,
                               int segmentIndex, int64_t *frameBegin,
                               int64_t *frameEnd);
void audioSampleRangeForSegment(const ExportTiming &timing, int segmentIndex,
                                int64_t *sampleBegin, int64_t *sampleEnd);

QString validateExportSettings(const ExportSettings &settings,
                               const TimelineSettings &timeline);
// Rejects an output video or SRT sidecar whose destination is an imported
// asset or the project file itself; finalization replaces the destination, so
// allowing either would delete source media. Paths are canonicalized and
// compared case-insensitively on Windows. `projectPath` may be empty for an
// unsaved document.
QString validateExportDestinations(const ExportSettings &settings,
                                   const Project &project,
                                   const QString &projectPath);
QString normalizedOutputPath(QString path);
QString defaultSrtPathFor(const QString &outputPath);

QJsonObject exportSettingsToJson(const ExportSettings &settings);
std::optional<ExportSettings> exportSettingsFromJson(const QJsonObject &json,
                                                     QString *error = nullptr);

} // namespace VideoEditor

// ExportSettings crosses a QThread boundary in ExportWorker::run.
Q_DECLARE_METATYPE(VideoEditor::ExportSettings)
