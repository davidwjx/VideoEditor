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

TEST(SubtitleEditor, UpdateMovesCueAndKeepsTrackSorted)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue cue;
    cue.id = QStringLiteral("cue-a");
    cue.startUs = 0;
    cue.endUs = 900'000;
    cue.text = QStringLiteral("A");
    ASSERT_EQ(editor.addSubtitleCue(subtitleTrackId(), cue), QStringLiteral("cue-a"));
    cue.id = QStringLiteral("cue-b");
    cue.startUs = 2'000'000;
    cue.endUs = 2'900'000;
    cue.text = QStringLiteral("B");
    ASSERT_EQ(editor.addSubtitleCue(subtitleTrackId(), cue), QStringLiteral("cue-b"));
    cue.id = QStringLiteral("cue-c");
    cue.startUs = 4'000'000;
    cue.endUs = 4'900'000;
    cue.text = QStringLiteral("C");
    ASSERT_EQ(editor.addSubtitleCue(subtitleTrackId(), cue), QStringLiteral("cue-c"));

    SubtitleCue moved;
    moved.startUs = 3'000'000;
    moved.endUs = 3'500'000;
    moved.text = QStringLiteral("A2");
    editor.updateSubtitleCue(subtitleTrackId(), QStringLiteral("cue-a"), moved);

    QList<SubtitleCue> cues = document.project().tracks.at(2).cues;
    ASSERT_EQ(cues.size(), 3);
    EXPECT_EQ(cues.at(0).id, QStringLiteral("cue-b"));
    EXPECT_EQ(cues.at(1).id, QStringLiteral("cue-a"));
    EXPECT_EQ(cues.at(1).startUs, 3'000'000);
    EXPECT_EQ(cues.at(1).text, QStringLiteral("A2"));
    EXPECT_EQ(cues.at(2).id, QStringLiteral("cue-c"));

    document.undo();
    cues = document.project().tracks.at(2).cues;
    ASSERT_EQ(cues.size(), 3);
    EXPECT_EQ(cues.at(0).id, QStringLiteral("cue-a"));
    EXPECT_EQ(cues.at(0).startUs, 0);
    EXPECT_EQ(cues.at(1).id, QStringLiteral("cue-b"));
    EXPECT_EQ(cues.at(2).id, QStringLiteral("cue-c"));

    document.redo();
    cues = document.project().tracks.at(2).cues;
    ASSERT_EQ(cues.size(), 3);
    EXPECT_EQ(cues.at(0).id, QStringLiteral("cue-b"));
    EXPECT_EQ(cues.at(1).id, QStringLiteral("cue-a"));
}

TEST(SubtitleEditor, RejectsDuplicateCueIds)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue cue;
    cue.id = QStringLiteral("dup");
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("第一");
    ASSERT_EQ(editor.addSubtitleCue(subtitleTrackId(), cue), QStringLiteral("dup"));
    ASSERT_EQ(document.undoStack()->count(), 1);

    SubtitleCue duplicate;
    duplicate.id = QStringLiteral("dup");
    duplicate.startUs = 2'000'000;
    duplicate.endUs = 3'000'000;
    duplicate.text = QStringLiteral("第二");
    EXPECT_TRUE(editor.addSubtitleCue(subtitleTrackId(), duplicate).isEmpty());
    EXPECT_EQ(document.project().tracks.at(2).cues.size(), 1);
    EXPECT_EQ(document.undoStack()->count(), 1);

    document.undo();
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
}

TEST(SubtitleEditor, ClampsCuePositions)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue cue;
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("位置");
    cue.positionXPermille = -50;
    cue.positionYPermille = 2000;
    const QString id = editor.addSubtitleCue(subtitleTrackId(), cue);
    ASSERT_FALSE(id.isEmpty());
    EXPECT_EQ(document.project().tracks.at(2).cues.first().positionXPermille, 0);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().positionYPermille, 1000);

    cue.startUs = 1'500'000;
    cue.endUs = 2'500'000;
    cue.text = QStringLiteral("位置 2");
    cue.positionXPermille = 1500;
    cue.positionYPermille = -1;
    editor.updateSubtitleCue(subtitleTrackId(), id, cue);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().positionXPermille, 1000);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().positionYPermille, 0);
}

TEST(SubtitleEditor, RejectsEmptyTextCues)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue blank;
    blank.startUs = 0;
    blank.endUs = 1'000'000;
    blank.text = QStringLiteral("   ");
    EXPECT_TRUE(editor.addSubtitleCue(subtitleTrackId(), blank).isEmpty());
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
    EXPECT_FALSE(document.canUndo());

    SubtitleCue cue;
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("有文本");
    const QString id = editor.addSubtitleCue(subtitleTrackId(), cue);
    ASSERT_FALSE(id.isEmpty());

    SubtitleCue cleared = cue;
    cleared.text = QStringLiteral("\n\t ");
    editor.updateSubtitleCue(subtitleTrackId(), id, cleared);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("有文本"));
    EXPECT_EQ(document.undoStack()->count(), 1);
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

TEST(SubtitleEditor, ReportsMissingOrLockedTrackOnImport)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    const QByteArray srt =
        "1\n00:00:00,000 --> 00:00:01,000\n文本\n";

    QString error;
    EXPECT_EQ(editor.importSrt(QStringLiteral("track-missing"), srt, &error), 0);
    EXPECT_TRUE(error.contains(QStringLiteral("未找到"))) << error.toStdString();

    error.clear();
    EXPECT_EQ(editor.importSrt(QStringLiteral("track-v1"), srt, &error), 0);
    EXPECT_TRUE(error.contains(QStringLiteral("未找到"))) << error.toStdString();

    QList<Track> tracks = document.project().tracks;
    for (Track &track : tracks) {
        if (track.type == TrackType::Subtitle)
            track.locked = true;
    }
    document.replaceTracks(tracks);

    error.clear();
    EXPECT_EQ(editor.importSrt(subtitleTrackId(), srt, &error), 0);
    EXPECT_TRUE(error.contains(QStringLiteral("锁定"))) << error.toStdString();
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
    EXPECT_FALSE(document.canUndo());
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
