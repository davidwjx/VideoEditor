#pragma once

#include <functional>

#include <QString>

#include "ExportCheckpoint.h"
#include "ExportEncoderCatalog.h"
#include "ExportProgress.h"
#include "ExportSettings.h"
#include "Project.h"

namespace VideoEditor {

// Orchestrates one export: settings/encoder checks, disk space, checkpoint and
// resume validation, offline rendering, video/AAC encoding, segmented writing,
// assembly and final re-probe validation.
//
// `run` never throws and never blocks on the UI; it is meant to be called from
// an ExportWorker thread. All output is frozen at call time: edits made while
// the export runs do not affect the produced file.
class ExportPipeline {
public:
    using ProgressFn = std::function<void(const ExportProgress &)>;
    using CancelFn = std::function<bool()>;
    using FreeBytesFn = std::function<qint64(const QString &)>;

    struct Hooks {
        ProgressFn onProgress;
        CancelFn isCanceled;
        // Overridable for tests and for callers that know better than
        // QStorageInfo (for example a network share).
        FreeBytesFn freeBytes;
        // Checked before any output file or parts directory is created. The
        // message is returned verbatim on failure. Injected by the UI layer
        // (RemovalPreflight lives in videoeditor_ai, which media must not
        // depend on).
        std::function<RemovalPreflightResult(const Project &)> removalPreflight;
        // Repair cache root used by OfflineFrameRenderer. Missing caches abort
        // the run instead of falling back to the original frame.
        QString removalCacheRoot;
        // Test injection: fail right before this zero-based segment is written.
        int failAtSegment = -1;
    };

    // `projectPath` is the document file behind `project`, when it has one.
    // The pipeline refuses an output/SRT destination equal to that file or to
    // any imported asset, because finalization replaces the destination.
    ExportResult run(const Project &project, const ExportSettings &settings,
                     const EncoderCatalog &catalog, bool resume,
                     const Hooks &hooks,
                     const QString &projectPath = QString());
};

} // namespace VideoEditor
