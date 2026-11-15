// src/ai/AiProtocol.h
#pragma once

#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>

#include "Removal.h"
#include "TimeBase.h"

namespace VideoEditor {

// One unit of work handed to the local inference subprocess. The subprocess
// only reads this spec; it never opens the project file.
struct AiJobSpec {
    QString jobId;
    QString clipId;
    QString assetPath;
    QString sidecarRoot;
    QString cacheDir;
    QString cacheKey;              // 64 lowercase hex
    QString modelsManifestPath;
    QString trackingModelId;
    QString inpaintModelId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    QList<MaskKeyframe> maskKeyframes;
    RemovalParameters parameters;
    int redoFirstFrame = -1;
    int redoLastFrame = -1;
    bool resume = false;
    // Tracking-only jobs write mask/confidence sidecars but no repair chunks.
    bool trackingOnly = false;
};

QJsonObject aiJobSpecToJson(const AiJobSpec &spec);
std::optional<AiJobSpec> aiJobSpecFromJson(const QJsonObject &json,
                                           QString *error = nullptr);

// Result of one job. A cancel and an error are mutually exclusive in the
// manager's output: cancelled is only true when the user asked to stop.
struct AiJobResult {
    bool ok = false;
    bool cancelled = false;
    QString errorCode;
    QString errorMessage;
    QString cacheKey;
    int completedFrames = 0;
    QString confidenceHash;
    RemovalTrackingState trackingState = RemovalTrackingState::NotTracked;
};

// Newline-delimited JSON framing shared by both sides of the local socket.
// encodeLine appends '\n'; parseLine strips trailing CR/LF and rejects empty,
// oversized, non-object and event-less/cmd-less messages.
class AiProtocol {
public:
    static QByteArray encodeLine(const QJsonObject &object);
    static bool parseLine(const QByteArray &line, QJsonObject *out,
                          QString *error = nullptr);
    static constexpr int kMaxLineBytes = 1024 * 1024;
};

} // namespace VideoEditor

// Registered outside the namespace so QSignalSpy and queued connections can
// resolve the types by their fully-qualified names.
Q_DECLARE_METATYPE(VideoEditor::AiJobSpec)
Q_DECLARE_METATYPE(VideoEditor::AiJobResult)
