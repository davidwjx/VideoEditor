#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

#include "MediaInfo.h"
#include "Timeline.h"

namespace VideoEditor {

struct Asset {
    QString id;
    QString path;
    MediaInfo info;
    QString proxyPath;

    bool operator==(const Asset &) const = default;
};

struct Project {
    static constexpr int kSchemaVersion = 2;

    QString name = QStringLiteral("未命名工程");
    TimelineSettings timeline;
    QList<Asset> assets;
    QList<Track> tracks;
    QList<SubtitleStyle> subtitleStyles;

    void ensureDefaultTracks();
    const Asset *assetById(const QString &id) const;

    bool operator==(const Project &) const = default;
};

} // namespace VideoEditor

// Queued worker invocations (TimelineRenderWorker::setProject and
// AudioEngine::setProject) pass Project by value across threads. Qt's moc also
// references the type from several metaobjects combined into one translation
// unit, so the declaration must live in the type's own header rather than in
// any single consumer.
Q_DECLARE_METATYPE(VideoEditor::Project)
