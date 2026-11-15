#include <gtest/gtest.h>
#include <QJsonObject>
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
