#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <QJsonArray>

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

namespace {

Project sampleTimelineProject()
{
    Project project;
    project.name = QStringLiteral("M2 工程");
    project.timeline.width = 1920;
    project.timeline.height = 1080;
    project.timeline.fps = Rational{30, 1};
    project.timeline.dropFrameTimecode = false;
    project.ensureDefaultTracks();

    MediaInfo info;
    info.path = QStringLiteral("C:/videos/素材.mp4");
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.audioCodec = QStringLiteral("aac");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = 10'000'000;
    info.hasAudio = true;
    info.supported = true;
    project.assets.append(Asset{QStringLiteral("asset-1"), info.path, info, {}});

    Clip video;
    video.id = QStringLiteral("clip-v");
    video.assetId = QStringLiteral("asset-1");
    video.linkId = QStringLiteral("link-1");
    video.sourceInUs = 1'000'000;
    video.sourceOutUs = 5'000'000;
    video.timelineStartUs = 500'000;
    video.speedMilli = 1500;
    video.preservePitch = true;
    video.transform.scaleMilli = 1200;
    video.transform.opacityMilli = 900;
    project.tracks[0].clips.append(video);

    Clip audio = video;
    audio.id = QStringLiteral("clip-a");
    audio.audio.volumeMilli = 750;
    audio.audio.panMilli = -250;
    audio.audio.fadeInUs = 200'000;
    audio.audio.fadeOutUs = 300'000;
    audio.audio.keyframes.append(VolumeKeyframe{0, 1000});
    audio.audio.keyframes.append(VolumeKeyframe{1'000'000, 500});
    project.tracks[1].clips.append(audio);

    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 0;
    cue.endUs = 2'000'000;
    cue.text = QStringLiteral("第一行\n第二行");
    cue.styleId = QStringLiteral("default");
    project.tracks[2].cues.append(cue);
    return project;
}

} // namespace

TEST(ProjectSerializer, RoundTripsTimelineSchemaV2)
{
    const Project original = sampleTimelineProject();
    const QJsonObject json = ProjectSerializer::toJson(original);
    EXPECT_EQ(json.value(QStringLiteral("schemaVersion")).toInt(),
              Project::kSchemaVersion);
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, original);
}

TEST(ProjectSerializer, MigratesVersionOneProject)
{
    QJsonObject v1 = ProjectSerializer::toJson(sampleTimelineProject());
    v1.insert(QStringLiteral("schemaVersion"), 1);
    v1.remove(QStringLiteral("timeline"));
    v1.remove(QStringLiteral("tracks"));
    v1.remove(QStringLiteral("subtitleStyles"));

    QString error;
    const auto restored = ProjectSerializer::fromJson(v1, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    ASSERT_EQ(restored->assets.size(), 1);
    EXPECT_EQ(restored->assets.first().path, QStringLiteral("C:/videos/素材.mp4"));
    EXPECT_EQ(restored->tracks.size(), 3);
    EXPECT_TRUE(restored->tracks.at(0).clips.isEmpty());
    EXPECT_EQ(restored->timeline.fps, (Rational{30, 1}));
    EXPECT_EQ(restored->subtitleStyles.size(), 1);

    // Saving a migrated project writes schemaVersion 2.
    const QJsonObject saved = ProjectSerializer::toJson(*restored);
    EXPECT_EQ(saved.value(QStringLiteral("schemaVersion")).toInt(), 2);
}

TEST(ProjectSerializer, RejectsInvalidClipRanges)
{
    QJsonObject json = ProjectSerializer::toJson(sampleTimelineProject());
    QJsonArray tracks = json.value(QStringLiteral("tracks")).toArray();
    QJsonObject videoTrack = tracks.at(0).toObject();
    QJsonArray clips = videoTrack.value(QStringLiteral("clips")).toArray();
    QJsonObject clip = clips.at(0).toObject();
    clip.insert(QStringLiteral("sourceOutUs"), clip.value(QStringLiteral("sourceInUs")).toDouble());
    clips.replace(0, clip);
    videoTrack.insert(QStringLiteral("clips"), clips);
    tracks.replace(0, videoTrack);
    json.insert(QStringLiteral("tracks"), tracks);

    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(json, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("片段")));
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
