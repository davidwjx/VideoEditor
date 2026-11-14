#pragma once

#include <QAbstractListModel>
#include <QList>

#include "Project.h"

namespace VideoEditor {

class MediaListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PathRole,
        CodecRole,
        ResolutionRole,
        DurationRole,
        CompatibleRole,
        ReasonRole,
        ProxyPathRole
    };

    explicit MediaListModel(QObject *parent = nullptr);

    void setAssets(const QList<Asset> &assets);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    QList<Asset> m_assets;
};

} // namespace VideoEditor
