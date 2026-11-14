#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QSignalSpy>

#include "ProjectDocument.h"
#include "TimelineEditor.h"
#include "TimelineModel.h"
#include "TrackListModel.h"

using namespace VideoEditor;

namespace {

Asset asset(const QString &id, int64_t durationUs, MediaKind kind)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/%1.mp4").arg(id);
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = kind == MediaKind::Video ? 1920 : 0;
    info.height = kind == MediaKind::Video ? 1080 : 0;
    info.frameRate = Rational{30, 1};
    info.durationUs = durationUs;
    info.hasAudio = true;
    info.hasVideo = kind == MediaKind::Video;
    info.kind = kind;
    info.supported = true;
    return Asset{id, info.path, info, {}};
}

Clip clip(const QString &id, int64_t startUs)
{
    Clip value;
    value.id = id;
    value.assetId = QStringLiteral("asset-v");
    value.timelineStartUs = startUs;
    value.sourceInUs = 0;
    value.sourceOutUs = 2'000'000;
    return value;
}

} // namespace

TEST(TimelineModel, UpdatesIncrementallyWithoutReset)
{
    ProjectDocument document;
    document.addAsset(asset(QStringLiteral("asset-v"), 10'000'000, MediaKind::Video));
    TimelineEditor editor(document);
    TimelineModel model;
    TrackListModel tracks;
    model.setProject(document.project());
    tracks.setProject(document.project());
    QAbstractItemModelTester modelTester(&model);
    QAbstractItemModelTester trackTester(&tracks);

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

    QObject::connect(&document, &ProjectDocument::timelineChanged,
                     &model, [&] { model.setProject(document.project()); });
    QObject::connect(&document, &ProjectDocument::tracksChanged,
                     &tracks, [&] { tracks.setProject(document.project()); });

    editor.addClip(QStringLiteral("track-v1"), clip(QStringLiteral("c1"), 0));
    EXPECT_EQ(insertSpy.count(), 1);
    EXPECT_EQ(model.rowCount(), 1);

    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("c1"), 1'000'000);
    EXPECT_GE(dataSpy.count(), 1);
    EXPECT_EQ(model.data(model.index(0, 0), TimelineModel::StartUsRole).toLongLong(),
              1'000'000);

    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("c1"), false);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(resetSpy.count(), 0);
}

TEST(TimelineModel, ExposesClipsAndCuesWithTrackIndex)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    editor.addClip(QStringLiteral("track-v1"), clip(QStringLiteral("c1"), 0));
    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("字幕");
    editor.addSubtitleCue(QStringLiteral("track-s1"), cue);

    TimelineModel model;
    model.setProject(document.project());
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.data(model.index(0, 0), TimelineModel::KindRole).toString(),
              QStringLiteral("clip"));
    EXPECT_EQ(model.data(model.index(1, 0), TimelineModel::KindRole).toString(),
              QStringLiteral("cue"));
    EXPECT_EQ(model.data(model.index(1, 0), TimelineModel::TrackIndexRole).toInt(), 2);
    EXPECT_EQ(model.data(model.index(1, 0), TimelineModel::TextRole).toString(),
              QStringLiteral("字幕"));
}

TEST(TrackListModel, ReconcilesTrackReorder)
{
    ProjectDocument document;
    TrackListModel model;
    model.setProject(document.project());
    QAbstractItemModelTester tester(&model);
    ASSERT_EQ(model.rowCount(), 3);

    Track extra;
    extra.id = QStringLiteral("track-v2");
    extra.type = TrackType::Video;
    extra.name = QStringLiteral("V2");
    TimelineEditor editor(document);
    editor.addTrack(extra);
    model.setProject(document.project());
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.data(model.index(3, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v2"));

    editor.moveTrack(3, 0);
    model.setProject(document.project());
    EXPECT_EQ(model.data(model.index(0, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v2"));
}
