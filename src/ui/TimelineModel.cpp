#include "TimelineModel.h"

#include <QFileInfo>

namespace VideoEditor {

TimelineModel::TimelineModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QList<TimelineModel::Row> TimelineModel::buildRows(const Project &project)
{
    QList<Row> rows;
    for (int trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex) {
        const Track &track = project.tracks.at(trackIndex);
        if (track.type == TrackType::Subtitle) {
            for (const SubtitleCue &cue : track.cues) {
                Row row;
                row.kind = QStringLiteral("cue");
                row.id = cue.id;
                row.trackId = track.id;
                row.trackType = track.type;
                row.trackIndex = trackIndex;
                row.cue = cue;
                rows.append(row);
            }
        } else {
            for (const Clip &clip : track.clips) {
                Row row;
                row.kind = QStringLiteral("clip");
                row.id = clip.id;
                row.trackId = track.id;
                row.trackType = track.type;
                row.trackIndex = trackIndex;
                row.clip = clip;
                rows.append(row);
            }
        }
    }
    return rows;
}

QString TimelineModel::rowKey(const Row &row)
{
    return row.kind + QLatin1Char(':') + row.id;
}

int TimelineModel::rowForKey(const QString &key) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (rowKey(m_rows.at(i)) == key)
            return i;
    }
    return -1;
}

void TimelineModel::setProject(const Project &project)
{
    m_assetNames.clear();
    for (const Asset &asset : project.assets)
        m_assetNames.insert(asset.id, QFileInfo(asset.path).fileName());

    const QList<Row> next = buildRows(project);
    QHash<QString, int> nextByKey;
    for (int i = 0; i < next.size(); ++i)
        nextByKey.insert(rowKey(next.at(i)), i);

    // Remove rows that disappeared, from the end so indexes stay valid.
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (!nextByKey.contains(rowKey(m_rows.at(i)))) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }

    // Existing rows: update in place.
    for (int i = 0; i < next.size(); ++i) {
        const int existing = rowForKey(rowKey(next.at(i)));
        if (existing < 0)
            continue;
        if (!(m_rows.at(existing) == next.at(i))) {
            m_rows[existing] = next.at(i);
            emit dataChanged(index(existing), index(existing));
        }
    }

    // New rows are appended; the QML delegates position themselves from
    // StartUsRole/TrackIndexRole, so row order carries no visual meaning.
    for (const Row &row : next) {
        if (rowForKey(rowKey(row)) >= 0)
            continue;
        const int position = m_rows.size();
        beginInsertRows(QModelIndex(), position, position);
        m_rows.append(row);
        endInsertRows();
    }

    if (!m_selectedId.isEmpty()) {
        bool stillPresent = false;
        for (const Row &row : m_rows) {
            if (row.id == m_selectedId)
                stillPresent = true;
        }
        if (!stillPresent) {
            m_selectedId.clear();
            emit selectionChanged();
        }
    }
}

void TimelineModel::selectItem(const QString &id)
{
    if (m_selectedId == id)
        return;
    m_selectedId = id;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0), index(m_rows.size() - 1), {SelectedRole});
    }
    emit selectionChanged();
}

int TimelineModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_rows.size());
}

QVariant TimelineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    const bool isClip = row.kind == QStringLiteral("clip");

    switch (role) {
    case KindRole:
        return row.kind;
    case ItemIdRole:
        return row.id;
    case TrackIdRole:
        return row.trackId;
    case TrackTypeRole:
        return trackTypeToString(row.trackType);
    case TrackIndexRole:
        return row.trackIndex;
    case StartUsRole:
        return QVariant::fromValue<qint64>(
            isClip ? row.clip.timelineStartUs : row.cue.startUs);
    case DurationUsRole:
        return QVariant::fromValue<qint64>(
            isClip ? row.clip.timelineDurationUs()
                   : row.cue.endUs - row.cue.startUs);
    case AssetIdRole:
        return isClip ? row.clip.assetId : QString();
    case NameRole:
        if (isClip) {
            const QString name = m_assetNames.value(row.clip.assetId);
            return name.isEmpty() ? row.clip.assetId : name;
        }
        return row.cue.text.left(20);
    case SpeedMilliRole:
        return isClip ? row.clip.speedMilli : 0;
    case EnabledRole:
        return isClip ? row.clip.enabled : row.cue.enabled;
    case LinkedRole:
        return isClip && !row.clip.linkId.isEmpty();
    case VolumeMilliRole:
        return isClip ? row.clip.audio.volumeMilli : 0;
    case PanMilliRole:
        return isClip ? row.clip.audio.panMilli : 0;
    case FadeInUsRole:
        return QVariant::fromValue<qint64>(
            isClip ? row.clip.audio.fadeInUs : 0);
    case FadeOutUsRole:
        return QVariant::fromValue<qint64>(
            isClip ? row.clip.audio.fadeOutUs : 0);
    case TextRole:
        return isClip ? QString() : row.cue.text;
    case StyleIdRole:
        return isClip ? QString() : row.cue.styleId;
    case SelectedRole:
        return row.id == m_selectedId;
    case KeyframeCountRole:
        return isClip ? int(row.clip.audio.keyframes.size()) : 0;
    case KeyframesRole: {
        QVariantList keyframes;
        if (isClip) {
            for (const VolumeKeyframe &keyframe : row.clip.audio.keyframes) {
                QVariantMap entry;
                entry.insert(QStringLiteral("timeUs"),
                             QVariant::fromValue<qint64>(keyframe.timeUs));
                entry.insert(QStringLiteral("volumeMilli"),
                             keyframe.volumeMilli);
                keyframes.append(entry);
            }
        }
        return keyframes;
    }
    case PositionXPermilleRole:
        return isClip ? 0 : row.cue.positionXPermille;
    case PositionYPermilleRole:
        return isClip ? 0 : row.cue.positionYPermille;
    default:
        return {};
    }
}

QHash<int, QByteArray> TimelineModel::roleNames() const
{
    return {
        {KindRole, QByteArrayLiteral("kind")},
        {ItemIdRole, QByteArrayLiteral("itemId")},
        {TrackIdRole, QByteArrayLiteral("trackId")},
        {TrackTypeRole, QByteArrayLiteral("trackType")},
        {TrackIndexRole, QByteArrayLiteral("trackIndex")},
        {StartUsRole, QByteArrayLiteral("startUs")},
        {DurationUsRole, QByteArrayLiteral("durationUs")},
        {AssetIdRole, QByteArrayLiteral("assetId")},
        {NameRole, QByteArrayLiteral("name")},
        {SpeedMilliRole, QByteArrayLiteral("speedMilli")},
        {EnabledRole, QByteArrayLiteral("enabled")},
        {LinkedRole, QByteArrayLiteral("linked")},
        {VolumeMilliRole, QByteArrayLiteral("volumeMilli")},
        {PanMilliRole, QByteArrayLiteral("panMilli")},
        {FadeInUsRole, QByteArrayLiteral("fadeInUs")},
        {FadeOutUsRole, QByteArrayLiteral("fadeOutUs")},
        {TextRole, QByteArrayLiteral("text")},
        {StyleIdRole, QByteArrayLiteral("styleId")},
        {SelectedRole, QByteArrayLiteral("selected")},
        {KeyframeCountRole, QByteArrayLiteral("keyframeCount")},
        {KeyframesRole, QByteArrayLiteral("keyframes")},
        {PositionXPermilleRole, QByteArrayLiteral("positionXPermille")},
        {PositionYPermilleRole, QByteArrayLiteral("positionYPermille")},
    };
}

} // namespace VideoEditor
