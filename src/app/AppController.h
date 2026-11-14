#pragma once

#include <memory>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include "MediaListModel.h"
#include "ProjectDocument.h"
#include "ProjectStore.h"

namespace VideoEditor {

class PreviewController;
class ProxyService;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(MediaListModel *mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryAvailableChanged)
    Q_PROPERTY(bool useProxy READ useProxy WRITE setUseProxy NOTIFY useProxyChanged)

public:
    explicit AppController(QObject *parent = nullptr, QString recoveryPath = {});
    ~AppController() override;

    MediaListModel *mediaModel() const { return m_mediaModel; }
    QString lastError() const { return m_lastError; }
    bool canUndo() const;
    bool canRedo() const;
    bool recoveryAvailable() const { return m_recoveryAvailable; }
    bool useProxy() const { return m_useProxy; }
    void setUseProxy(bool enabled);

    Q_INVOKABLE void importUrls(const QList<QUrl> &urls);
    Q_INVOKABLE void openAsset(const QString &id);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void restoreRecovery();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void saveProject(const QUrl &url);
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void selftest(const QString &path);

    // Called on clean shutdown: removes a stale autosave only when the open
    // document is clean and no startup recovery prompt is still pending.
    void discardRecoveryIfClean();

    ProjectDocument *document() const { return m_document; }
    ProjectStore *store() const { return m_store.get(); }
    PreviewController *preview() const { return m_preview; }
    ProxyService *proxyService() const { return m_proxyService; }

signals:
    void lastErrorChanged();
    void undoStateChanged();
    void recoveryAvailableChanged();
    void useProxyChanged();
    void projectChanged();
    void selftestFramePresented();

private:
    void setLastError(const QString &message);
    void refreshModel();
    void setRecoveryAvailable(bool available);
    void autosave();

    ProjectDocument *m_document = nullptr;
    MediaListModel *m_mediaModel = nullptr;
    PreviewController *m_preview = nullptr;
    ProxyService *m_proxyService = nullptr;
    std::unique_ptr<ProjectStore> m_store;
    QTimer m_autosaveTimer;
    QString m_lastError;
    QString m_currentAssetId;
    bool m_recoveryAvailable = false;
    bool m_useProxy = true;
};

} // namespace VideoEditor
