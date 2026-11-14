#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "TimelineMixer.h"

using namespace VideoEditor;

namespace {

QString tonePath()
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
        .filePath(QStringLiteral("tone.wav"));
}

Asset toneAsset()
{
    Asset asset;
    asset.id = QStringLiteral("asset-a");
    asset.path = tonePath();
    asset.info.kind = MediaKind::Audio;
    asset.info.hasAudio = true;
    asset.info.hasVideo = false;
    asset.info.audioCodec = QStringLiteral("pcm_s16le");
    asset.info.audioSampleRate = 48000;
    asset.info.audioChannels = 1;
    asset.info.durationUs = 2'000'000;
    asset.info.supported = true;
    return asset;
}

Project projectWithAudio(const QString &clipId, int64_t startUs,
                         int64_t sourceOutUs, int speedMilli = 1000)
{
    Project project;
    project.timeline.fps = Rational{30, 1};
    project.assets.append(toneAsset());
    Track track;
    track.id = QStringLiteral("a1");
    track.type = TrackType::Audio;
    Clip clip;
    clip.id = clipId;
    clip.assetId = QStringLiteral("asset-a");
    clip.timelineStartUs = startUs;
    clip.sourceInUs = 0;
    clip.sourceOutUs = sourceOutUs;
    clip.speedMilli = speedMilli;
    track.clips.append(clip);
    project.tracks.append(track);
    return project;
}

} // namespace

TEST(AudioMix, RendersAudibleStereoAtCenterPan)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);

    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    EXPECT_EQ(stats.activeLayers, 1);
    EXPECT_GT(stats.peak, 0.1f);
    for (int i = 0; i < 4800; i += 97)
        EXPECT_NEAR(out[size_t(i) * 2], out[size_t(i) * 2 + 1], 1e-4f);
    EXPECT_EQ(mixer.positionUs(), 100'000);
}

TEST(AudioMix, RespectsTrackMuteAndClipMute)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].muted = true;
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(4800) * 2);
    mixer.render(out.data(), 4800, nullptr);
    for (float sample : out)
        EXPECT_EQ(sample, 0.0f);

    project.tracks[0].muted = false;
    project.tracks[0].clips[0].audio.muted = true;
    mixer.setProject(project);
    mixer.setPosition(0);
    mixer.render(out.data(), 4800, nullptr);
    for (float sample : out)
        EXPECT_EQ(sample, 0.0f);
}

TEST(AudioMix, SoloOverridesOtherTracks)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    Track second = project.tracks.first();
    second.id = QStringLiteral("a2");
    second.solo = true;
    second.clips[0].id = QStringLiteral("c2");
    project.tracks.append(second);

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    EXPECT_EQ(stats.activeLayers, 1);
}

TEST(AudioMix, FadeInRampsFromSilence)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].clips[0].audio.fadeInUs = 1'000'000;
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(4800) * 2);
    mixer.render(out.data(), 4800, nullptr);
    EXPECT_NEAR(out[0], 0.0f, 1e-5f);
    const float late = std::abs(out[size_t(4700) * 2]);
    const float early = std::abs(out[size_t(100) * 2]);
    EXPECT_GT(late, early);
}

TEST(AudioMix, LimiterPreventsClipping)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].clips[0].audio.volumeMilli = 2000;
    Track second = project.tracks.first();
    second.id = QStringLiteral("a2");
    second.clips[0].id = QStringLiteral("c2");
    second.clips[0].audio.volumeMilli = 2000;
    project.tracks.append(second);

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(500'000);
    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    for (float sample : out)
        EXPECT_LE(std::abs(sample), 1.0f);
    EXPECT_LE(stats.peak, 0.98f);
}

TEST(AudioMix, LimiterEngagesOnOverdrivenMix)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    // Four overlapping copies at +6 dB exceed full scale; the master limiter
    // must scale the whole block down to the 0.98 ceiling.
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].clips[0].audio.volumeMilli = 2000;
    for (int i = 2; i <= 4; ++i) {
        Track extra = project.tracks.first();
        extra.id = QStringLiteral("a%1").arg(i);
        extra.clips[0].id = QStringLiteral("c%1").arg(i);
        extra.clips[0].audio.volumeMilli = 2000;
        project.tracks.append(extra);
    }

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(500'000);
    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    EXPECT_EQ(stats.activeLayers, 4);
    EXPECT_NEAR(stats.peak, 0.98f, 1e-6f);
    for (float sample : out)
        EXPECT_LE(std::abs(sample), 0.98f + 1e-6f);
}

TEST(AudioMix, InterpolatesVolumeKeyframes)
{
    Clip clip;
    clip.sourceOutUs = 10'000'000;
    clip.audio.volumeMilli = 1000;
    clip.audio.keyframes = {VolumeKeyframe{0, 1000}, VolumeKeyframe{1'000'000, 500}};
    EXPECT_FLOAT_EQ(clipGainAt(clip, 0), 1.0f);
    EXPECT_FLOAT_EQ(clipGainAt(clip, 500'000), 0.75f);
    EXPECT_FLOAT_EQ(clipGainAt(clip, 2'000'000), 0.5f);
}
