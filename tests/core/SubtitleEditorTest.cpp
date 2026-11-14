#include <gtest/gtest.h>

#include "ProjectDocument.h"
#include "Srt.h"
#include "TimelineEditor.h"

using namespace VideoEditor;

namespace {

QString subtitleTrackId()
{
    return QStringLiteral("track-s1");
}

} // namespace

TEST(SubtitleEditor, AddsUpdatesAndRemovesCuesUndoably)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 0;
    cue.endUs = 2'000'000;
    cue.text = QStringLiteral("你好");
    ASSERT_EQ(editor.addSubtitleCue(subtitleTrackId(), cue), QStringLiteral("cue-1"));
    ASSERT_EQ(document.project().tracks.at(2).cues.size(), 1);

    SubtitleCue updated = cue;
    updated.text = QStringLiteral("你好，世界");
    updated.positionYPermille = 800;
    editor.updateSubtitleCue(subtitleTrackId(), cue.id, updated);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("你好，世界"));

    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text, QStringLiteral("你好"));
    document.redo();
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("你好，世界"));

    editor.removeSubtitleCue(subtitleTrackId(), cue.id);
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.size(), 1);
}

TEST(SubtitleEditor, AddsCueInStartOrderAndGeneratesId)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue later;
    later.startUs = 2'000'000;
    later.endUs = 3'000'000;
    later.text = QStringLiteral("后");
    const QString laterId = editor.addSubtitleCue(subtitleTrackId(), later);
    ASSERT_FALSE(laterId.isEmpty());

    SubtitleCue earlier;
    earlier.startUs = 1'000'000;
    earlier.endUs = 1'500'000;
    earlier.text = QStringLiteral("前");
    const QString earlierId = editor.addSubtitleCue(subtitleTrackId(), earlier);
    ASSERT_FALSE(earlierId.isEmpty());
    EXPECT_NE(earlierId, laterId);

    const Track &track = document.project().tracks.at(2);
    ASSERT_EQ(track.cues.size(), 2);
    EXPECT_EQ(track.cues.at(0).text, QStringLiteral("前"));
    EXPECT_EQ(track.cues.at(1).text, QStringLiteral("后"));
}

TEST(SubtitleEditor, RejectsInvalidCuesAndLockedTracks)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue invalid;
    invalid.startUs = 2'000'000;
    invalid.endUs = 2'000'000;
    invalid.text = QStringLiteral("零长度");
    EXPECT_TRUE(editor.addSubtitleCue(subtitleTrackId(), invalid).isEmpty());
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
    EXPECT_FALSE(document.canUndo());

    // Cues are only accepted on subtitle tracks.
    SubtitleCue cue;
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("视频轨");
    EXPECT_TRUE(editor.addSubtitleCue(QStringLiteral("track-v1"), cue).isEmpty());

    QList<Track> tracks = document.project().tracks;
    for (Track &track : tracks) {
        if (track.type == TrackType::Subtitle)
            track.locked = true;
    }
    document.replaceTracks(tracks);
    cue.text = QStringLiteral("锁定");
    EXPECT_TRUE(editor.addSubtitleCue(subtitleTrackId(), cue).isEmpty());
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
}

TEST(SubtitleEditor, AppliesStyleToSingleCueOrAll)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    for (int i = 0; i < 2; ++i) {
        SubtitleCue cue;
        cue.id = QStringLiteral("cue-%1").arg(i);
        cue.startUs = i * 1'000'000;
        cue.endUs = cue.startUs + 900'000;
        cue.text = QStringLiteral("字幕 %1").arg(i);
        editor.addSubtitleCue(subtitleTrackId(), cue);
    }

    editor.applySubtitleStyle(subtitleTrackId(), QStringLiteral("boxed"),
                              QStringLiteral("cue-1"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(0).styleId,
              QStringLiteral("default"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(1).styleId,
              QStringLiteral("boxed"));

    editor.applySubtitleStyle(subtitleTrackId(), QStringLiteral("bold-outline"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(0).styleId,
              QStringLiteral("bold-outline"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(1).styleId,
              QStringLiteral("bold-outline"));

    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.at(0).styleId,
              QStringLiteral("default"));
}

TEST(SubtitleEditor, ImportsSrtAsSingleUndoStep)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    const QByteArray srt =
        "1\n00:00:00,000 --> 00:00:01,000\n第一句\n\n"
        "2\n00:00:01,500 --> 00:00:02,000\n第二句\n";
    QString error;
    EXPECT_EQ(editor.importSrt(subtitleTrackId(), srt, &error), 2)
        << error.toStdString();
    EXPECT_EQ(document.project().tracks.at(2).cues.size(), 2);

    document.undo();
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());

    document.redo();
    const QByteArray exported = editor.exportSrt(subtitleTrackId());
    EXPECT_TRUE(exported.contains(QStringLiteral("第二句").toUtf8()));
}

TEST(SubtitleEditor, ReportsImportErrorsWithoutChangingDocument)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    QString error;
    EXPECT_EQ(editor.importSrt(subtitleTrackId(), "1\nbad timecode\ntext\n", &error), 0);
    EXPECT_TRUE(error.contains(QStringLiteral("第 1 条")));
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
    EXPECT_FALSE(document.canUndo());
}
