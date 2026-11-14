#pragma once

#include <optional>
#include <QObject>
#include <QString>
#include <QUndoStack>

#include "Project.h"

namespace VideoEditor {

class AddAssetCommand;
class RemoveAssetCommand;

class ProjectDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)

public:
    explicit ProjectDocument(QObject *parent = nullptr);

    const Project &project() const { return m_project; }
    void setProject(Project project, const QString &filePath = {});

    void addAsset(const Asset &asset);
    void removeAsset(const QString &assetId);
    void setAssetProxyPath(const QString &assetId, const QString &proxyPath);

    bool save(const QString &path, QString *error = nullptr);
    bool load(const QString &path, QString *error = nullptr);

    QString filePath() const { return m_filePath; }
    bool isDirty() const { return !m_undoStack.isClean(); }
    bool canUndo() const { return m_undoStack.canUndo(); }
    bool canRedo() const { return m_undoStack.canRedo(); }
    QUndoStack *undoStack() { return &m_undoStack; }
    void undo() { m_undoStack.undo(); }
    void redo() { m_undoStack.redo(); }

signals:
    void changed();
    void assetsChanged();
    void undoStateChanged();
    void dirtyChanged();

private:
    friend class AddAssetCommand;
    friend class RemoveAssetCommand;

    void insertAssetInternal(const Asset &asset);
    void insertAssetInternalAt(int index, const Asset &asset);
    void removeAssetInternal(const QString &assetId);

    Project m_project;
    QUndoStack m_undoStack;
    QString m_filePath;
};

} // namespace VideoEditor
