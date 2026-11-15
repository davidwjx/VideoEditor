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
    QAbstractItemModelTester modelTester(
        &model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QAbstractItemModelTester trackTester(
        &tracks, QAbstractItemModelTester::FailureReportingMode::Fatal);

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

TEST(TimelineModel, GroupsLanesByTypeAndMatchesCompositionOrder)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    Track video2;
    video2.id = QStringLiteral("track-v2");
    video2.type = TrackType::Video;
    video2.name = QStringLiteral("V2");
    editor.addTrack(video2);
    editor.addClip(QStringLiteral("track-v1"), clip(QStringLiteral("v1-c"), 0));
    editor.addClip(QStringLiteral("track-v2"), clip(QStringLiteral("v2-c"), 0));
    Clip audio = clip(QStringLiteral("a1-c"), 0);
    audio.assetId = QStringLiteral("asset-a");
    editor.addClip(QStringLiteral("track-a1"), audio);

    TimelineModel model;
    model.setProject(document.project());
    const auto trackIndexFor = [&model](const QString &id) {
        for (int i = 0; i < model.rowCount(); ++i) {
            if (model.data(model.index(i, 0), TimelineModel::ItemIdRole)
                    .toString()
                == id) {
                return model.data(model.index(i, 0),
                                  TimelineModel::TrackIndexRole)
                    .toInt();
            }
        }
        return -1;
    };
    // Display lanes: V2 (top composited layer), V1, A1, S1. TrackIndex is the
    // display row, so QML lanes and headers line up.
    EXPECT_EQ(trackIndexFor(QStringLiteral("v2-c")), 0);
    EXPECT_EQ(trackIndexFor(QStringLiteral("v1-c")), 1);
    EXPECT_EQ(trackIndexFor(QStringLiteral("a1-c")), 2);
}

TEST(TrackListModel, ReconcilesTrackReorder)
{
    ProjectDocument document;
    TrackListModel model;
    model.setProject(document.project());
    QAbstractItemModelTester tester(
        &model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    ASSERT_EQ(model.rowCount(), 3);

    Track extra;
    extra.id = QStringLiteral("track-v2");
    extra.type = TrackType::Video;
    extra.name = QStringLiteral("V2");
    TimelineEditor editor(document);
    editor.addTrack(extra);
    model.setProject(document.project());
    ASSERT_EQ(model.rowCount(), 4);
    // Lanes are grouped by type with video stacked top-down in composition
    // order: V2 (highest layer) before V1, then audio, then subtitles.
    EXPECT_EQ(model.data(model.index(0, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v2"));
    EXPECT_EQ(model.data(model.index(1, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v1"));

    editor.moveTrack(3, 0);
    model.setProject(document.project());
    // Project order is now V2, V1, A1, S1; display order reverses the video
    // group, so V1 is again the bottom composited lane.
    EXPECT_EQ(model.data(model.index(0, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v1"));
    EXPECT_EQ(model.data(model.index(1, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v2"));
}
