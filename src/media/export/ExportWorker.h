#pragma once

#include <atomic>

#include <QObject>

#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"

namespace VideoEditor {

// Runs ExportPipeline on a worker thread. The catalog is supplied from the
// thread that probed it (detectEncoders is expensive and should not run on the
// UI thread either, but the result is a plain value object).
//
// ExportWorker is moved to a QThread by the (future) ExportController; run()
// and cancel() are slots so the controller can use queued connections and
// cancel() never blocks the export thread.
class ExportWorker : public QObject {
    Q_OBJECT

public:
    explicit ExportWorker(QObject *parent = nullptr);

    void setCatalog(const EncoderCatalog &catalog);

public slots:
    void run(VideoEditor::Project project,
             VideoEditor::ExportSettings settings, bool resume);
    void cancel();

signals:
    void progress(VideoEditor::ExportProgress progress);
    void finished(VideoEditor::ExportResult result);

private:
    EncoderCatalog m_catalog;
    std::atomic_bool m_cancel{false};
};

} // namespace VideoEditor
