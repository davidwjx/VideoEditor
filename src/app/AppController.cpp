#include "AppController.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include "CodecSupport.h"
#include "MediaListModel.h"
#include "MediaProbe.h"

namespace VideoEditor {

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_document(new ProjectDocument(this))
    , m_mediaModel(new MediaListModel(this))
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    const QString recoveryPath = QDir(appData).filePath(
        QStringLiteral("recovery/project.autosave.json"));
    m_store = std::make_unique<ProjectStore>(recoveryPath);

    connect(m_document, &ProjectDocument::assetsChanged,
            this, &AppController::refreshModel);
    connect(m_document, &ProjectDocument::undoStateChanged,
            this, &AppController::undoStateChanged);
    connect(m_document, &ProjectDocument::dirtyChanged,
            this, &AppController::projectChanged);

    m_autosaveTimer.setInterval(30'000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &AppController::autosave);
    m_autosaveTimer.start();

    refreshModel();
    refreshRecoveryState();
}

AppController::~AppController() = default;

bool AppController::canUndo() const
{
    return m_document->canUndo();
}

bool AppController::canRedo() const
{
    return m_document->canRedo();
}

void AppController::importUrls(const QList<QUrl> &urls)
{
    QStringList failures;

    for (const QUrl &url : urls) {
        const QString path = url.toLocalFile();
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        if (!info) {
            failures.append(error);
            continue;
        }
        if (!info->supported) {
            failures.append(info->unsupportedReason);
            continue;
        }

        Asset asset;
        asset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        asset.path = path;
        asset.info = *info;
        m_document->addAsset(asset);
    }

    if (failures.isEmpty())
        clearError();
    else
        setLastError(failures.join(QLatin1Char('\n')));
}

void AppController::undo()
{
    m_document->undo();
}

void AppController::redo()
{
    m_document->redo();
}

void AppController::clearError()
{
    if (m_lastError.isEmpty())
        return;
    m_lastError.clear();
    emit lastErrorChanged();
}

void AppController::restoreRecovery()
{
    QString error;
    auto project = m_store->loadRecovery(&error);
    if (!project) {
        setLastError(error);
        return;
    }
    m_document->setProject(std::move(*project));
    m_store->discardRecovery();
    refreshRecoveryState();
    clearError();
}

void AppController::discardRecovery()
{
    m_store->discardRecovery();
    refreshRecoveryState();
}

void AppController::saveProject(const QUrl &url)
{
    QString error;
    if (!m_document->save(url.toLocalFile(), &error))
        setLastError(error);
    else
        clearError();
}

void AppController::openProject(const QUrl &url)
{
    QString error;
    if (!m_document->load(url.toLocalFile(), &error))
        setLastError(error);
    else
        clearError();
}

void AppController::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

void AppController::refreshModel()
{
    m_mediaModel->setAssets(m_document->project().assets);
}

void AppController::refreshRecoveryState()
{
    const bool available = QFileInfo::exists(m_store->recoveryFilePath());
    if (available == m_recoveryAvailable)
        return;
    m_recoveryAvailable = available;
    emit recoveryAvailableChanged();
}

void AppController::autosave()
{
    if (!m_document->isDirty())
        return;
    QString error;
    if (!m_store->saveRecovery(m_document->project(), &error)) {
        setLastError(error);
        return;
    }
    refreshRecoveryState();
}

} // namespace VideoEditor
