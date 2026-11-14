#include "TrackListModel.h"

#include <QSet>

namespace VideoEditor {

namespace {

QString typeLabel(TrackType type)
{
    switch (type) {
    case TrackType::Video:
        return QStringLiteral("视频");
    case TrackType::Audio:
        return QStringLiteral("音频");
    case TrackType::Subtitle:
        return QStringLiteral("字幕");
    }
    return QStringLiteral("视频");
}

} // namespace

TrackListModel::TrackListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TrackListModel::indexOfTrack(const QString &trackId) const
{
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks.at(i).id == trackId)
            return i;
    }
    return -1;
}

void TrackListModel::setProject(const Project &project)
{
    const QList<Track> &next = project.tracks;

    // Keys that must survive this reconciliation.
    QSet<QString> nextIds;
    for (const Track &track : next)
        nextIds.insert(track.id);

    // Drop tracks that disappeared, from the end so indexes stay valid.
    for (int i = m_tracks.size() - 1; i >= 0; --i) {
        if (!nextIds.contains(m_tracks.at(i).id)) {
            beginRemoveRows(QModelIndex(), i, i);
            m_tracks.removeAt(i);
            endRemoveRows();
        }
    }

    // Reorder and insert so physical row order matches the project, which is
    // what TrackIndexRole and the QML track headers consume.
    for (int i = 0; i < next.size(); ++i) {
        const int current = indexOfTrack(next.at(i).id);
        if (current < 0) {
            beginInsertRows(QModelIndex(), i, i);
            m_tracks.insert(i, next.at(i));
            endInsertRows();
            continue;
        }
        if (current != i) {
            // Moving down needs a destination past the source row; moving up
            // inserts before the target row. Both end with the row at index i.
            const int destination = current > i ? i : i + 1;
            beginMoveRows(QModelIndex(), current, current, QModelIndex(),
                          destination);
            m_tracks.move(current, i);
            endMoveRows();
        }
        if (!(m_tracks.at(i) == next.at(i))) {
            m_tracks[i] = next.at(i);
            emit dataChanged(index(i), index(i));
        }
    }
}

int TrackListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_tracks.size());
}

QVariant TrackListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tracks.size())
        return {};
    const Track &track = m_tracks.at(index.row());

    switch (role) {
    case TrackIdRole:
        return track.id;
    case NameRole:
        return track.name;
    case TypeRole:
        return trackTypeToString(track.type);
    case TypeLabelRole:
        return typeLabel(track.type);
    case EnabledRole:
        return track.enabled;
    case MutedRole:
        return track.muted;
    case SoloRole:
        return track.solo;
    case LockedRole:
        return track.locked;
    case IndexRole:
        return index.row();
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackListModel::roleNames() const
{
    return {
        {TrackIdRole, QByteArrayLiteral("trackId")},
        {NameRole, QByteArrayLiteral("name")},
        {TypeRole, QByteArrayLiteral("type")},
        {TypeLabelRole, QByteArrayLiteral("typeLabel")},
        {EnabledRole, QByteArrayLiteral("enabled")},
        {MutedRole, QByteArrayLiteral("muted")},
        {SoloRole, QByteArrayLiteral("solo")},
        {LockedRole, QByteArrayLiteral("locked")},
        {IndexRole, QByteArrayLiteral("index")},
    };
}

} // namespace VideoEditor
