#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QUrl>

#include "AppController.h"
#include "MediaListModel.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(AppController, ImportsSupportedFile)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_TRUE(controller.lastError().isEmpty());
}

TEST(AppController, ReportsUnsupportedOrMissingFile)
{
    AppController controller;
    controller.importUrls({QUrl::fromLocalFile(fixture(QStringLiteral("corrupt.mp4")))});
    EXPECT_EQ(controller.mediaModel()->rowCount(), 0);
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("无法打开")));

    controller.clearError();
    controller.importUrls({QUrl::fromLocalFile(fixture(QStringLiteral("missing.mp4")))});
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("不存在")));
}

TEST(AppController, UndoRemovesImportedAsset)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    controller.undo();
    EXPECT_EQ(controller.mediaModel()->rowCount(), 0);
    controller.redo();
    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
}
