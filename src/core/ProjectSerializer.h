#pragma once

#include <optional>
#include <QJsonObject>
#include <QString>

#include "Project.h"

namespace VideoEditor {

class ProjectSerializer {
public:
    static QJsonObject toJson(const Project &project);
    static std::optional<Project> fromJson(const QJsonObject &json,
                                           QString *error = nullptr);
    static bool save(const Project &project, const QString &path,
                     QString *error = nullptr);
    static std::optional<Project> load(const QString &path,
                                       QString *error = nullptr);
};

} // namespace VideoEditor
