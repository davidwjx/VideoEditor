#pragma once

#include <QList>
#include <QString>

#include "MediaInfo.h"

namespace VideoEditor {

struct Asset {
    QString id;
    QString path;
    MediaInfo info;
    QString proxyPath;

    bool operator==(const Asset &) const = default;
};

struct Project {
    static constexpr int kSchemaVersion = 1;

    QString name = QStringLiteral("未命名工程");
    QList<Asset> assets;

    bool operator==(const Project &) const = default;
};

} // namespace VideoEditor
