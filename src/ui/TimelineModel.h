#pragma once

#include <QAbstractListModel>
#include <QHash>

#include "Project.h"

namespace VideoEditor {

class TimelineModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        ItemIdRole,
        TrackIdRole,
        TrackTypeRole,
        TrackIndexRole,
        StartUsRole,
        DurationUsRole,
        AssetIdRole,
        NameRole,
        SpeedMilliRole,
        EnabledRole,
        LinkedRole,
        VolumeMilliRole,
        PanMilliRole,
        FadeInUsRole,
        FadeOutUsRole,
        TextRole,
        StyleIdRole,
        SelectedRole,
        KeyframeCountRole,
        KeyframesRole,
        PositionXPermilleRole,
        PositionYPermilleRole,
        SourceInUsRole,
        SourceOutUsRole
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setProject(const Project &project);
    void selectItem(const QString &id);
    QString selectedItemId() const { return m_selectedId; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void selectionChanged();

private:
    struct Row {
        QString kind;       // "clip" or "cue"
        QString id;
        QString trackId;
        TrackType trackType = TrackType::Video;
        int trackIndex = 0;
        Clip clip;
        SubtitleCue cue;
        bool operator==(const Row &) const = default;
    };

    static QList<Row> buildRows(const Project &project);
    static QString rowKey(const Row &row);
    int rowForKey(const QString &key) const;

    QList<Row> m_rows;
    QHash<QString, QString> m_assetNames;
    QString m_selectedId;
};

} // namespace VideoEditor
