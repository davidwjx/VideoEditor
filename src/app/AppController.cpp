#include "AppController.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include "CodecSupport.h"
#include "MediaListModel.h"
#include "MediaProbe.h"
#include "PreviewController.h"
#include "ProxyPlanner.h"
#include "ProxyService.h"

namespace VideoEditor {

namespace {

// Proxy generation runs the FFmpeg CLI. Prefer the executable shipped next to
// the app, then the documented override, then whatever is on PATH.
QString resolveFfmpegPath()
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("ffmpeg.exe"));
    if (QFileInfo::exists(bundled))
        return bundled;
    const QString configured = qEnvironmentVariable("VIDEOEDITOR_FFMPEG");
    if (!configured.isEmpty())
        return configured;
    return QStringLiteral("ffmpeg");
}

} // namespace

AppController::AppController(QObject *parent, QString recoveryPath)
    : QObject(parent)
    , m_document(new ProjectDocument(this))
    , m_mediaModel(new MediaListModel(this))
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (recoveryPath.isEmpty()) {
        recoveryPath = QDir(appData).filePath(
            QStringLiteral("recovery/project.autosave.json"));
    }
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
    // Prompt only once, at startup, for a recovery file left by a prior session.
    // Autosaving during this session must not raise the prompt.
    m_recoveryAvailable = QFileInfo::exists(m_store->recoveryFilePath());

    m_preview = new PreviewController(this);
    connect(m_preview, &PreviewController::framePresented, this,
            [this](qint64) { emit selftestFramePresented(); });
    connect(m_preview, &PreviewController::error, this,
            &AppController::setLastError);

    const QString cacheRoot = QDir(appData).filePath(QStringLiteral("cache"));
    m_proxyService = new ProxyService(cacheRoot, resolveFfmpegPath(), this);
    connect(m_proxyService, &ProxyService::ready, this,
            [this](const QString &assetId, const QString &proxyPath) {
                m_document->setAssetProxyPath(assetId, proxyPath);
            });
    connect(m_proxyService, &ProxyService::failed, this,
            [this](const QString &, const QString &message) {
                setLastError(message);
            });
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

        // Proxies are cache metadata: generated in the background and stored
        // outside the undo stack, so an import never blocks on ffmpeg.
        const ProxyPlan plan = planProxy(asset.info);
        if (plan.needed)
            m_proxyService->request(asset.id, asset.path, plan.height);
    }

    if (failures.isEmpty())
        clearError();
    else
        setLastError(failures.join(QLatin1Char('\n')));
}

void AppController::openAsset(const QString &id)
{
    for (const Asset &asset : m_document->project().assets) {
        if (asset.id == id) {
            // Undo/redo, a cleared cache, or a project saved before generation
            // finished can leave the asset without a usable proxy. Re-request
            // it so the preview does not fall back to the source forever.
            if (needsProxyGeneration(asset.info, asset.proxyPath)) {
                m_proxyService->request(asset.id, asset.path,
                                        planProxy(asset.info).height);
            }
            m_currentAssetId = id;
            m_preview->openMedia(asset.path,
                                 m_useProxy ? asset.proxyPath : QString());
            return;
        }
    }
}

void AppController::setUseProxy(bool enabled)
{
    if (m_useProxy == enabled)
        return;
    m_useProxy = enabled;
    emit useProxyChanged();
    // Re-opening the current asset is what actually switches the preview
    // between the proxy and the source; changing the flag alone does nothing.
    if (!m_currentAssetId.isEmpty())
        openAsset(m_currentAssetId);
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
    m_currentAssetId.clear();
    // The restored document replaces whatever was previewed; stop and clear
    // the old preview so its clip does not keep playing behind the new project.
    m_preview->closeMedia();
    m_store->discardRecovery();
    setRecoveryAvailable(false);
    clearError();
}

void AppController::discardRecovery()
{
    m_store->discardRecovery();
    setRecoveryAvailable(false);
}

void AppController::saveProject(const QUrl &url)
{
    QString path = url.toLocalFile();
    // The Qt Quick save dialog does not expose a default suffix, so add the
    // project extension here when the user did not type one.
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".veproj");

    QString error;
    if (!m_document->save(path, &error)) {
        setLastError(error);
        return;
    }
    // The project on disk now supersedes any autosaved snapshot; keeping the
    // recovery file would let the next launch roll back edits saved after the
    // last autosave.
    m_store->discardRecovery();
    setRecoveryAvailable(false);
    clearError();
}

void AppController::openProject(const QUrl &url)
{
    QString error;
    if (!m_document->load(url.toLocalFile(), &error)) {
        setLastError(error);
        return;
    }
    m_currentAssetId.clear();
    // The loaded project replaces the current document, so the previous
    // asset's preview must stop instead of playing on behind it.
    m_preview->closeMedia();
    clearError();
}

void AppController::discardRecoveryIfClean()
{
    if (m_document->isDirty() || m_recoveryAvailable)
        return;
    m_store->discardRecovery();
}

void AppController::selftest(const QString &path)
{
    importUrls({QUrl::fromLocalFile(path)});
    if (m_mediaModel->rowCount() <= 0)
        return;
    const Asset &asset = m_document->project().assets.first();
    openAsset(asset.id);
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

    // Drop the remembered asset when undo/redo or a project load removes it,
    // so toggling the proxy cannot try to reopen a stale id.
    if (!m_currentAssetId.isEmpty()) {
        const auto &assets = m_document->project().assets;
        const auto it = std::find_if(assets.cbegin(), assets.cend(),
                                     [this](const Asset &asset) {
                                         return asset.id == m_currentAssetId;
                                     });
        if (it == assets.cend())
            m_currentAssetId.clear();
    }
}

void AppController::setRecoveryAvailable(bool available)
{
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
    if (!m_store->saveRecovery(m_document->project(), &error))
        setLastError(error);
}

} // namespace VideoEditor
