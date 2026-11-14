#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "AppController.h"
#include "MediaListModel.h"
#include "ProjectStore.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

void writeRecovery(const QString &path)
{
    Project project;
    Asset asset;
    asset.id = QStringLiteral("asset-1");
    asset.path = QStringLiteral("C:/media/example.mp4");
    asset.info.path = asset.path;
    asset.info.supported = true;
    project.assets.append(asset);

    ProjectStore store(path);
    QString error;
    ASSERT_TRUE(store.saveRecovery(project, &error)) << error.toStdString();
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

TEST(AppController, RestoresRecoveryAtStartup)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    AppController controller(nullptr, recovery);
    EXPECT_TRUE(controller.recoveryAvailable());

    controller.restoreRecovery();

    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
    EXPECT_TRUE(controller.lastError().isEmpty());
}

TEST(AppController, DiscardRecoveryRemovesFileAndClearsPrompt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    AppController controller(nullptr, recovery);
    EXPECT_TRUE(controller.recoveryAvailable());

    controller.discardRecovery();

    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
}

TEST(AppController, CorruptRecoveryKeepsPromptAndReportsError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    QFile file(recovery);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{ this is not valid json");
    file.close();

    AppController controller(nullptr, recovery);
    EXPECT_TRUE(controller.recoveryAvailable());

    controller.restoreRecovery();

    EXPECT_FALSE(controller.lastError().isEmpty());
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("工程文件")));
    EXPECT_TRUE(QFileInfo::exists(recovery));
    EXPECT_TRUE(controller.recoveryAvailable());
}

TEST(MediaListModel, ExposesRolesForImportedFixture)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);

    const QModelIndex index = controller.mediaModel()->index(0, 0);
    ASSERT_TRUE(index.isValid());
    const auto role = [&controller, index](int role) {
        return controller.mediaModel()->data(index, role);
    };

    EXPECT_EQ(role(MediaListModel::NameRole).toString(), QStringLiteral("h264.mp4"));
    EXPECT_EQ(role(MediaListModel::PathRole).toString(), path);
    EXPECT_EQ(role(MediaListModel::CodecRole).toString(), QStringLiteral("H.264"));
    EXPECT_EQ(role(MediaListModel::ResolutionRole).toString(), QStringLiteral("640×360"));
    EXPECT_EQ(role(MediaListModel::DurationRole).toString(), QStringLiteral("00:00:02.000"));
    EXPECT_TRUE(role(MediaListModel::ProxyPathRole).toString().isEmpty());
}
