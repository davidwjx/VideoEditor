#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "AppController.h"
#include "PreviewController.h"
#include "TimelineController.h"
#include "TimelineModel.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(TimelineUi, ControllerIsExposedAndEditable)
{
    AppController controller;
    ASSERT_NE(controller.timeline(), nullptr);
    ASSERT_NE(controller.timeline()->timelineModel(), nullptr);
    EXPECT_NE(controller.audioEngine(), nullptr);

    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;

    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());
    EXPECT_EQ(controller.timeline()->timelineModel()->rowCount(), 2); // video + audio

    controller.timeline()->seekUs(1'000'000);
    controller.timeline()->splitAtPlayhead();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 2);
    controller.undo();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 1);
}

TEST(TimelineUi, ImportsSrtAndExportsIt)
{
    AppController controller;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString srtPath = dir.filePath(QStringLiteral("in.srt"));
    QFile file(srtPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("1\n00:00:00,000 --> 00:00:01,000\n字幕测试\n");
    file.close();

    EXPECT_EQ(controller.timeline()->importSrtFile(srtPath), QStringLiteral("1"));
    ASSERT_EQ(controller.document()->project().tracks.at(2).cues.size(), 1);

    const QString outPath = dir.filePath(QStringLiteral("out.srt"));
    ASSERT_TRUE(controller.timeline()->exportSrtFile(outPath));
    QFile out(outPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_TRUE(out.readAll().contains(QStringLiteral("字幕测试").toUtf8()));
}

TEST(TimelineUi, OpenTimelineSwitchesPreviewMode)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    controller.timeline()->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0);
    controller.refreshTimelinePreview();
    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_GT(controller.preview()->durationUs(), 0);
}
