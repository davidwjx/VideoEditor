#pragma once

#include <cstdint>

#include <QMetaType>
#include <QString>

#include "ExportValidator.h"

namespace VideoEditor {

enum class ExportStage {
    Preparing,
    EncodingAudio,
    Rendering,
    Finalizing,
    Validating,
    Done,
    Failed,
    Canceled
};

// Snapshot pushed to the UI while an export runs. All counters are absolute
// (including segments completed by an earlier, resumed run) so a progress bar
// never jumps backwards when a resume starts.
struct ExportProgress {
    ExportStage stage = ExportStage::Preparing;
    int64_t completedFrames = 0;
    int64_t totalFrames = 0;
    int completedSegments = 0;
    int totalSegments = 0;
    int percent = 0;
    double encodeFps = 0.0;
    int64_t elapsedMs = 0;
    int64_t etaMs = -1;
    qint64 outputBytes = 0;
    bool resumed = false;
};

struct ExportResult {
    bool ok = false;
    bool canceled = false;
    // True when completed parts and a checkpoint remain on disk, so the next
    // run with resume=true continues instead of restarting.
    bool resumable = false;
    // True when this run actually reused parts produced by an earlier run.
    bool resumed = false;
    bool usedHardware = false;
    QString encoderId;
    QString outputPath;
    ExportValidation validation;
    QString error;
    QString warning;
    float audioPeak = 0.0f;
};

} // namespace VideoEditor

// ExportWorker emits these through queued signal/slot connections across the
// export thread boundary; without these declarations Qt silently drops the
// argument at emit time.
Q_DECLARE_METATYPE(VideoEditor::ExportProgress)
Q_DECLARE_METATYPE(VideoEditor::ExportResult)
