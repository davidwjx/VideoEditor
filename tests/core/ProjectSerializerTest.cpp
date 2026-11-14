#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "ProjectSerializer.h"
#include "ProjectStore.h"

using namespace VideoEditor;

namespace {

Project sampleProject()
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/示例.mp4");
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.audioCodec = QStringLiteral("aac");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30000, 1001};
    info.durationUs = 12'345'678;
    info.rotationDegrees = 90;
    info.hasAudio = true;
    info.supported = true;

    Project project;
    project.name = QStringLiteral("测试工程");
    project.assets.append(Asset{QStringLiteral("asset-1"), info.path, info, {}});
    return project;
}

} // namespace

TEST(ProjectSerializer, RoundTripsAllFields)
{
    const Project original = sampleProject();
    const QJsonObject json = ProjectSerializer::toJson(original);
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, original);
}

TEST(ProjectSerializer, RejectsNewerSchema)
{
    QJsonObject json = ProjectSerializer::toJson(sampleProject());
    json.insert(QStringLiteral("schemaVersion"), Project::kSchemaVersion + 1);
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(json, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("版本")));
}

TEST(ProjectSerializer, SavesAtomicallyAndLoads)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("project.veproj"));
    const Project original = sampleProject();

    QString error;
    ASSERT_TRUE(ProjectSerializer::save(original, path, &error)) << error.toStdString();
    const auto loaded = ProjectSerializer::load(path, &error);
    ASSERT_TRUE(loaded.has_value()) << error.toStdString();
    EXPECT_EQ(*loaded, original);
}

TEST(ProjectStore, SavesAndLoadsRecovery)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.filePath(QStringLiteral("recovery/project.autosave.json")));

    QString error;
    ASSERT_TRUE(store.saveRecovery(sampleProject(), &error)) << error.toStdString();
    const auto restored = store.loadRecovery(&error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(restored->assets.size(), 1);

    store.discardRecovery();
    EXPECT_FALSE(store.loadRecovery(&error).has_value());
}
