#include <gtest/gtest.h>

#include "ProjectDocument.h"

using namespace VideoEditor;

namespace {

Asset makeAsset(int index)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/%1.mp4").arg(index);
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = 1'000'000;
    info.supported = true;
    return Asset{QStringLiteral("asset-%1").arg(index), info.path, info, {}};
}

} // namespace

TEST(ProjectDocument, AddRemoveUndoRedo)
{
    ProjectDocument doc;
    doc.addAsset(makeAsset(1));
    doc.addAsset(makeAsset(2));
    EXPECT_EQ(doc.project().assets.size(), 2);
    EXPECT_TRUE(doc.canUndo());

    doc.undo();
    EXPECT_EQ(doc.project().assets.size(), 1);
    EXPECT_TRUE(doc.canRedo());

    doc.redo();
    EXPECT_EQ(doc.project().assets.size(), 2);

    doc.removeAsset(QStringLiteral("asset-1"));
    EXPECT_EQ(doc.project().assets.size(), 1);
    doc.undo();
    EXPECT_EQ(doc.project().assets.size(), 2);
    EXPECT_EQ(doc.project().assets.first().id, QStringLiteral("asset-1"));
}

TEST(ProjectDocument, NewCommandClearsRedo)
{
    ProjectDocument doc;
    doc.addAsset(makeAsset(1));
    doc.undo();
    EXPECT_TRUE(doc.canRedo());
    doc.addAsset(makeAsset(2));
    EXPECT_FALSE(doc.canRedo());
}

TEST(ProjectDocument, SetProjectMergesBuiltinSubtitleStyles)
{
    Project project;
    project.subtitleStyles = {SubtitleStyle{}}; // legacy v2 snapshot
    ProjectDocument doc;
    doc.setProject(project);

    ASSERT_EQ(doc.project().subtitleStyles.size(), 3);
    EXPECT_EQ(doc.project().subtitleStyles.at(0).id, QStringLiteral("default"));
    EXPECT_EQ(doc.project().subtitleStyles.at(1).id, QStringLiteral("boxed"));
    EXPECT_EQ(doc.project().subtitleStyles.at(2).id,
              QStringLiteral("bold-outline"));
}

TEST(ProjectDocument, UndoLimitKeepsLatestHundred)
{
    ProjectDocument doc;
    for (int i = 0; i < 105; ++i)
        doc.addAsset(makeAsset(i));

    for (int i = 0; i < 100; ++i)
        doc.undo();

    EXPECT_EQ(doc.project().assets.size(), 5);
}
