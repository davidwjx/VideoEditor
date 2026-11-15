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
    // A worker is single-use in practice, but clearing the flag makes a
    // restarted worker behave like a fresh one instead of canceling instantly.
    m_cancel.store(false);
    ExportPipeline pipeline;
    ExportPipeline::Hooks hooks;
    hooks.isCanceled = [this] { return m_cancel.load(); };
    hooks.onProgress = [this](const ExportProgress &value) {
        emit progress(value);
    };
    const ExportResult result =
        pipeline.run(project, settings, m_catalog, resume, hooks);
    emit finished(result);
}

void ExportWorker::cancel()
{
    m_cancel.store(true);
}

} // namespace VideoEditor
