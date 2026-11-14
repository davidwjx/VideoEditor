#include "ProjectStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "ProjectSerializer.h"

namespace VideoEditor {

ProjectStore::ProjectStore(QString recoveryFilePath)
    : m_recoveryFilePath(std::move(recoveryFilePath))
{
}

bool ProjectStore::save(const Project &project, const QString &file,
                        QString *error) const
{
    return ProjectSerializer::save(project, file, error);
}

std::optional<Project> ProjectStore::load(const QString &file, QString *error) const
{
    return ProjectSerializer::load(file, error);
}

bool ProjectStore::saveRecovery(const Project &project, QString *error) const
{
    return ProjectSerializer::save(project, m_recoveryFilePath, error);
}

std::optional<Project> ProjectStore::loadRecovery(QString *error) const
{
    if (!QFileInfo::exists(m_recoveryFilePath)) {
        if (error)
            *error = QStringLiteral("没有可恢复的工程。");
        return std::nullopt;
    }
    return ProjectSerializer::load(m_recoveryFilePath, error);
}

void ProjectStore::discardRecovery() const
{
    QFile::remove(m_recoveryFilePath);
}

QString ProjectStore::recoveryFilePath() const
{
    return m_recoveryFilePath;
}

} // namespace VideoEditor
