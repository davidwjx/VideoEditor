#pragma once

#include <memory>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include "MediaListModel.h"
#include "ProjectDocument.h"
#include "ProjectStore.h"

namespace VideoEditor {

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(MediaListModel *mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryAvailableChanged)

public:
    explicit AppController(QObject *parent = nullptr, QString recoveryPath = {});
    ~AppController() override;

    MediaListModel *mediaModel() const { return m_mediaModel; }
    QString lastError() const { return m_lastError; }
    bool canUndo() const;
    bool canRedo() const;
    bool recoveryAvailable() const { return m_recoveryAvailable; }

    Q_INVOKABLE void importUrls(const QList<QUrl> &urls);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void restoreRecovery();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void saveProject(const QUrl &url);
    Q_INVOKABLE void openProject(const QUrl &url);

    ProjectDocument *document() const { return m_document; }
    ProjectStore *store() const { return m_store.get(); }

signals:
    void lastErrorChanged();
    void undoStateChanged();
    void recoveryAvailableChanged();
    void projectChanged();

private:
    void setLastError(const QString &message);
    void refreshModel();
    void setRecoveryAvailable(bool available);
    void autosave();

    ProjectDocument *m_document = nullptr;
    MediaListModel *m_mediaModel = nullptr;
    std::unique_ptr<ProjectStore> m_store;
    QTimer m_autosaveTimer;
    QString m_lastError;
    bool m_recoveryAvailable = false;
};

} // namespace VideoEditor
