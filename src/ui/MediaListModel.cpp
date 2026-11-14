#include "MediaListModel.h"

#include <QFileInfo>

#include "CodecSupport.h"
#include "TimeBase.h"

namespace VideoEditor {

MediaListModel::MediaListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void MediaListModel::setAssets(const QList<Asset> &assets)
{
    beginResetModel();
    m_assets = assets;
    endResetModel();
}

int MediaListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_assets.size());
}

QVariant MediaListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_assets.size())
        return {};

    const Asset &asset = m_assets.at(index.row());
    switch (role) {
    case IdRole:
        return asset.id;
    case NameRole:
        return QFileInfo(asset.path).fileName();
    case PathRole:
        return asset.path;
    case CodecRole:
        return friendlyCodecName(classifyVideoCodec(asset.info.videoCodec).kind);
    case ResolutionRole:
        return QStringLiteral("%1×%2").arg(asset.info.width).arg(asset.info.height);
    case DurationRole:
        return formatDuration(asset.info.durationUs);
    case CompatibleRole:
        return asset.info.supported;
    case ReasonRole:
        return asset.info.unsupportedReason;
    case ProxyPathRole:
        return asset.proxyPath;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaListModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {NameRole, "name"},
        {PathRole, "path"},
        {CodecRole, "codec"},
        {ResolutionRole, "resolution"},
        {DurationRole, "duration"},
        {CompatibleRole, "compatible"},
        {ReasonRole, "reason"},
        {ProxyPathRole, "proxyPath"},
    };
}

} // namespace VideoEditor
