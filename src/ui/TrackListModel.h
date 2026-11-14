#pragma once

#include <QAbstractListModel>
#include <QList>

#include "Project.h"

namespace VideoEditor {

class TrackListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        TrackIdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        TypeLabelRole,
        EnabledRole,
        MutedRole,
        SoloRole,
        LockedRole,
        IndexRole
    };

    explicit TrackListModel(QObject *parent = nullptr);

    void setProject(const Project &project);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    int indexOfTrack(const QString &trackId) const;

    QList<Track> m_tracks;
};

} // namespace VideoEditor
