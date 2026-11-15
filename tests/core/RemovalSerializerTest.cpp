#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include "ProjectSerializer.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalSerializer, RoundTripsSchemaV3)
{
    Project original = projectWithClip();
    original.removalLayers.append(sampleLayer());
    original.tracks[0].clips[0].removalLayerId = QStringLiteral("rl-1");

    const QJsonObject json = ProjectSerializer::toJson(original);
    EXPECT_EQ(json.value(QStringLiteral("schemaVersion")).toInt(), 3);
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, original);
}

TEST(RemovalSerializer, MigratesV2WithEmptyRemovalLayers)
{
    Project v2 = projectWithClip();
    QJsonObject json = ProjectSerializer::toJson(v2);
    json.insert(QStringLiteral("schemaVersion"), 2);
    json.remove(QStringLiteral("removalLayers"));
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_TRUE(restored->removalLayers.isEmpty());
}

TEST(RemovalSerializer, RejectsDuplicateLayerIds)
{
    Project p = projectWithClip();
    p.removalLayers.append(sampleLayer());
    p.removalLayers.append(sampleLayer());
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("重复")));
}

TEST(RemovalSerializer, RejectsUnsortedMaskKeyframes)
{
    Project p = projectWithClip();
    RemovalLayer layer = sampleLayer();
    layer.maskKeyframes[0].sourceTimeUs = 6'000'000;
    p.removalLayers.append(layer);
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("排序")));
}

TEST(RemovalSerializer, RejectsLayerBeyondAssetDuration)
{
    Project p = projectWithClip();
    RemovalLayer layer = sampleLayer();
    layer.sourceOutUs = 20'000'000; // asset duration is 10s
    p.removalLayers.append(layer);
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("超出素材时长")));
}

TEST(RemovalSerializer, RejectsLayerWhoseClipLacksBackLink)
{
    Project p = projectWithClip(); // clip.removalLayerId stays empty
    p.removalLayers.append(sampleLayer());
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("未关联")));
}

TEST(RemovalSerializer, RejectsTwoLayersTargetingOneClip)
{
    Project p = projectWithClip();
    RemovalLayer first = sampleLayer();
    RemovalLayer second = sampleLayer();
    second.id = QStringLiteral("rl-2");
    p.removalLayers.append(first);
    p.removalLayers.append(second);
    p.tracks[0].clips[0].removalLayerId = first.id;
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("多个")));
}

TEST(RemovalSerializer, RejectsDanglingClipLink)
{
    Project p = projectWithClip();
    p.tracks[0].clips[0].removalLayerId = QStringLiteral("rl-missing");
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("不存在")));
}

TEST(RemovalSerializer, RejectsUnknownRepairMode)
{
    Project p = projectWithClip();
    p.removalLayers.append(sampleLayer());
    p.tracks[0].clips[0].removalLayerId = QStringLiteral("rl-1");
    QJsonObject json = ProjectSerializer::toJson(p);
    QJsonArray layers = json.value(QStringLiteral("removalLayers")).toArray();
    QJsonObject layer = layers.at(0).toObject();
    QJsonObject parameters = layer.value(QStringLiteral("parameters")).toObject();
    parameters.insert(QStringLiteral("mode"), QStringLiteral("turbo"));
    layer.insert(QStringLiteral("parameters"), parameters);
    layers.replace(0, layer);
    json.insert(QStringLiteral("removalLayers"), layers);

    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(json, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("修复模式")));
}

TEST(RemovalSchema, EnumStringMappingsRoundTrip)
{
    const QList<RemovalTrackingState> states = {
        RemovalTrackingState::NotTracked, RemovalTrackingState::Tracking,
        RemovalTrackingState::Partial, RemovalTrackingState::Complete,
        RemovalTrackingState::LowConfidence, RemovalTrackingState::Failed};
    for (RemovalTrackingState state : states) {
        bool ok = false;
        EXPECT_EQ(removalTrackingStateFromString(
                      removalTrackingStateToString(state), &ok),
                  state);
        EXPECT_TRUE(ok);
    }
    bool ok = true;
    EXPECT_EQ(removalTrackingStateFromString(QStringLiteral("mystery"), &ok),
              RemovalTrackingState::Failed);
    EXPECT_FALSE(ok);

    for (RepairMode mode : {RepairMode::Fast, RepairMode::Quality}) {
        ok = false;
        EXPECT_EQ(repairModeFromString(repairModeToString(mode), &ok), mode);
        EXPECT_TRUE(ok);
    }
    ok = true;
    EXPECT_EQ(repairModeFromString(QStringLiteral("turbo"), &ok), RepairMode::Fast);
    EXPECT_FALSE(ok);
}

TEST(RemovalSchema, DisplaySizeAppliesRotation)
{
    MediaInfo info;
    info.width = 1920;
    info.height = 1080;
    Asset asset{QStringLiteral("asset-1"), QStringLiteral("C:/x.mp4"), info, {}};
    EXPECT_EQ(displaySizeForAsset(asset), QSize(1920, 1080));

    asset.info.rotationDegrees = 90;
    EXPECT_EQ(displaySizeForAsset(asset), QSize(1080, 1920));
    asset.info.rotationDegrees = 270;
    EXPECT_EQ(displaySizeForAsset(asset), QSize(1080, 1920));
    asset.info.rotationDegrees = 180;
    EXPECT_EQ(displaySizeForAsset(asset), QSize(1920, 1080));

    asset.info.rotationDegrees = 0;
    asset.info.width = 0;
    EXPECT_EQ(displaySizeForAsset(asset), QSize(1920, 1080));
}
