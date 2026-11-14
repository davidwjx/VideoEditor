#pragma once

#include <optional>
#include <QString>

#include "Project.h"

namespace VideoEditor {

class ProjectStore {
public:
    explicit ProjectStore(QString recoveryFilePath);

    bool save(const Project &project, const QString &file,
              QString *error = nullptr) const;
    std::optional<Project> load(const QString &file,
                                QString *error = nullptr) const;
    bool saveRecovery(const Project &project, QString *error = nullptr) const;
    std::optional<Project> loadRecovery(QString *error = nullptr) const;
    void discardRecovery() const;
    QString recoveryFilePath() const;

private:
    QString m_recoveryFilePath;
};

} // namespace VideoEditor
