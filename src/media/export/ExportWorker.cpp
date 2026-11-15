#include "ExportWorker.h"

namespace VideoEditor {

ExportWorker::ExportWorker(QObject *parent)
    : QObject(parent)
{
}

void ExportWorker::setCatalog(const EncoderCatalog &catalog)
{
    m_catalog = catalog;
}

void ExportWorker::run(VideoEditor::Project project,
                       VideoEditor::ExportSettings settings, bool resume)
{
    // The cancellation flag is intentionally NOT reset here. A cancel queued
    // between starting the worker and dequeuing run() must win, otherwise the
    // click is silently lost and the export completes anyway. ExportController
    // creates one worker per export, so a fresh worker starts uncancelled.
    ExportPipeline pipeline;
    ExportPipeline::Hooks hooks;
    hooks.isCanceled = [this] { return m_cancel.load(); };
    hooks.onProgress = [this](const ExportProgress &value) {
        emit progress(value);
    };
    hooks.removalPreflight = m_removalChecker;
    hooks.removalCacheRoot = m_removalCacheRoot;
    const ExportResult result =
        pipeline.run(project, settings, m_catalog, resume, hooks, m_projectPath);
    emit finished(result);
}

void ExportWorker::cancel()
{
    m_cancel.store(true);
}

} // namespace VideoEditor
