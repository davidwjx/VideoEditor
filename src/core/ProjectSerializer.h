#pragma once

#include <optional>
#include <QJsonObject>
#include <QString>

#include "Project.h"

namespace VideoEditor {

class ProjectSerializer {
public:
    static QJsonObject toJson(const Project &project);
    // documentPath is optional metadata used by recovery snapshots so a
    // restored document still resolves sidecars next to its saved file. It is
    // absent from files written by older versions and then loads as empty.
    static std::optional<Project> fromJson(const QJsonObject &json,
                                           QString *error = nullptr,
                                           QString *documentPath = nullptr);
    static bool save(const Project &project, const QString &path,
                     QString *error = nullptr,
                     const QString &documentPath = {});
    static std::optional<Project> load(const QString &path,
                                       QString *error = nullptr,
                                       QString *documentPath = nullptr);
};

} // namespace VideoEditor
