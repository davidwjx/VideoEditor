#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "AppController.h"
#include "MediaListModel.h"
#include "PreviewController.h"
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

TEST(AppController, ImportsAudioOnlyAsset)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("bgm.mp3"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_TRUE(controller.lastError().isEmpty());
    const QVariant codec = controller.mediaModel()->data(
        controller.mediaModel()->index(0, 0), MediaListModel::CodecRole);
    EXPECT_TRUE(codec.toString().contains(QStringLiteral("音频")));
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

TEST(AppController, SavesAndReopensProject)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString projectPath = dir.filePath(QStringLiteral("project"));

    {
        AppController controller;
        controller.importUrls({QUrl::fromLocalFile(path)});
        ASSERT_EQ(controller.mediaModel()->rowCount(), 1);

        // No suffix: saveProject must append .veproj.
        controller.saveProject(QUrl::fromLocalFile(projectPath));
        EXPECT_TRUE(controller.lastError().isEmpty());
        EXPECT_TRUE(QFileInfo::exists(projectPath + QStringLiteral(".veproj")));
    }

    AppController reopened;
    reopened.openProject(
        QUrl::fromLocalFile(projectPath + QStringLiteral(".veproj")));
    EXPECT_TRUE(reopened.lastError().isEmpty());
    ASSERT_EQ(reopened.mediaModel()->rowCount(), 1);

    const QModelIndex index = reopened.mediaModel()->index(0, 0);
    ASSERT_TRUE(index.isValid());
    EXPECT_EQ(reopened.mediaModel()->data(index, MediaListModel::NameRole).toString(),
              QStringLiteral("h264.mp4"));
    EXPECT_EQ(reopened.mediaModel()->data(index, MediaListModel::CodecRole).toString(),
              QStringLiteral("H.264"));
    EXPECT_EQ(reopened.mediaModel()->data(index, MediaListModel::PathRole).toString(),
              path);
}

TEST(AppController, OpeningProjectStopsCurrentPreview)
{
    const QString media = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(media))
        GTEST_SKIP() << "missing fixture";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    AppController controller(nullptr,
                             dir.filePath(QStringLiteral("recovery.json")));
    controller.importUrls({QUrl::fromLocalFile(media)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);

    controller.openAsset(controller.document()->project().assets.first().id);
    ASSERT_EQ(controller.preview()->mediaName(), QStringLiteral("h264.mp4"));

    const QString projectPath = dir.filePath(QStringLiteral("project"));
    controller.saveProject(QUrl::fromLocalFile(projectPath));
    ASSERT_TRUE(controller.lastError().isEmpty());

    // Loading another project replaces the document and must clear the old
    // preview instead of leaving its last frame or playback behind.
    controller.openProject(
        QUrl::fromLocalFile(projectPath + QStringLiteral(".veproj")));
    EXPECT_TRUE(controller.lastError().isEmpty());
    EXPECT_TRUE(controller.preview()->mediaName().isEmpty());
    EXPECT_TRUE(controller.preview()->decodeMode().isEmpty());
    EXPECT_EQ(controller.preview()->positionMs(), 0);
    EXPECT_EQ(controller.preview()->durationMs(), 0);
    EXPECT_FALSE(controller.preview()->playing());
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

TEST(AppController, SuccessfulSaveDiscardsExistingRecovery)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    const QString media = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(media))
        GTEST_SKIP() << "missing fixture";

    AppController controller(nullptr, recovery);
    ASSERT_TRUE(controller.recoveryAvailable());

    controller.importUrls({QUrl::fromLocalFile(media)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    ASSERT_TRUE(controller.document()->isDirty());

    controller.saveProject(QUrl::fromLocalFile(dir.filePath(QStringLiteral("saved"))));

    EXPECT_TRUE(controller.lastError().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
}

TEST(AppController, CleanExitDiscardsStaleRecoveryWithoutPendingPrompt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));

    // No recovery file at startup, so no prompt is pending.
    AppController controller(nullptr, recovery);
    ASSERT_FALSE(controller.recoveryAvailable());
    ASSERT_FALSE(controller.document()->isDirty());

    // Simulate an autosave written later in the session, after the startup
    // prompt check, that the user has since made obsolete.
    writeRecovery(recovery);
    ASSERT_TRUE(QFileInfo::exists(recovery));

    controller.discardRecoveryIfClean();

    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
}

TEST(AppController, CleanExitKeepsPendingRecovery)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    AppController controller(nullptr, recovery);
    ASSERT_TRUE(controller.recoveryAvailable());
    ASSERT_FALSE(controller.document()->isDirty());

    // Ignoring the modal must not destroy the snapshot the user was offered.
    controller.discardRecoveryIfClean();

    EXPECT_TRUE(QFileInfo::exists(recovery));
    EXPECT_TRUE(controller.recoveryAvailable());
}

TEST(PreviewController, CloseMediaClearsPreviewState)
{
    const QString media = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(media))
        GTEST_SKIP() << "missing fixture";

    PreviewController controller;
    controller.openMedia(media);
    EXPECT_EQ(controller.mediaName(), QStringLiteral("h264.mp4"));

    controller.closeMedia();

    EXPECT_TRUE(controller.mediaName().isEmpty());
    EXPECT_TRUE(controller.decodeMode().isEmpty());
    EXPECT_EQ(controller.positionMs(), 0);
    EXPECT_EQ(controller.durationMs(), 0);
    EXPECT_FALSE(controller.playing());
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
