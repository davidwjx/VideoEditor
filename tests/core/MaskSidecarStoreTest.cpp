#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "MaskSidecarStore.h"

using namespace VideoEditor;

TEST(MaskSidecarStore, WritesReadsAndRelocatesMasks)
{
    QTemporaryDir dir;
    const QString projectPath = dir.filePath(QStringLiteral("我的 工程.veproj"));
    MaskSidecarStore store(projectPath);
    ASSERT_TRUE(store.ensureLayout());

    BinaryMask mask;
    mask.width = mask.height = 32;
    mask.bits.assign(1024, 0);
    mask.set(16, 16, true);

    QString hash;
    QString error;
    ASSERT_TRUE(store.writeMask(mask, &hash, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(store.maskPath(hash)));

    const auto read = store.readMask(hash, &error);
    ASSERT_TRUE(read.has_value()) << error.toStdString();
    EXPECT_EQ(read->bits, mask.bits);

    const QString newPath = dir.filePath(QStringLiteral("另存为.veproj"));
    ASSERT_TRUE(store.relocateTo(newPath, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(store.maskPath(hash)));

    QFile::remove(store.maskPath(hash));
    EXPECT_FALSE(store.readMask(hash, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));
}

TEST(MaskSidecarStore, MissingRootAndHashesAreGuarded)
{
    MaskSidecarStore store; // no project path and no fallback
    QString error;
    EXPECT_TRUE(store.sidecarRoot().isEmpty());
    EXPECT_TRUE(store.maskPath(QString(64, QLatin1Char('a'))).isEmpty());
    EXPECT_TRUE(store.trackingPath(QString(64, QLatin1Char('a'))).isEmpty());
    EXPECT_FALSE(store.ensureLayout(&error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(store.maskPath(QStringLiteral("../escape")).isEmpty());
}

TEST(MaskSidecarStore, UnsavedProjectUsesFallbackRootAndMovesOnSaveAs)
{
    QTemporaryDir dir;
    MaskSidecarStore store; // empty project path
    store.setProjectPath(QString(), dir.path());
    EXPECT_TRUE(store.sidecarRoot().contains(QStringLiteral("unsaved")));
    ASSERT_TRUE(store.ensureLayout());

    BinaryMask mask;
    mask.width = mask.height = 8;
    mask.bits.assign(64, 0);
    mask.set(4, 4, true);
    QString hash;
    QString error;
    ASSERT_TRUE(store.writeMask(mask, &hash, &error)) << error.toStdString();

    const QString saved = dir.filePath(QStringLiteral("未命名工程.veproj"));
    ASSERT_TRUE(store.relocateTo(saved, &error)) << error.toStdString();
    EXPECT_TRUE(store.sidecarRoot().startsWith(saved + QStringLiteral(".assets")));
    EXPECT_TRUE(QFileInfo::exists(store.maskPath(hash)));
}
