#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "AudioClock.h"
#include "TimelineMixer.h"

using namespace VideoEditor;

TEST(AudioSync, MixerPositionMatchesConsumedFramesWithoutDrift)
{
    Project project; // silent, only exercises the clock arithmetic
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> block(size_t(480) * 2);
    const int blocksPerHour = 3600 * 100;
    for (int i = 0; i < blocksPerHour; ++i)
        mixer.render(block.data(), 480, nullptr);
    // 480 frames * 100 blocks/s * 3600 s = exactly one hour; integer math must
    // land on 3.6e9 us with zero drift.
    EXPECT_EQ(mixer.positionUs(), 3'600'000'000LL);
}

TEST(AudioSync, ClockMapsToFrameWithinOneFrame)
{
    const Rational fps{30, 1};
    for (int64_t offset : {0LL, 5'000LL, 16'000LL, 33'000LL, 999'000LL}) {
        const int64_t frame = frameIndexForClock(1'000'000 + offset, fps);
        const int64_t frameStartUs = framesToMicros(frame, fps);
        EXPECT_LE(std::llabs(frameStartUs - (1'000'000 + offset)),
                  framesToMicros(1, fps));
    }
}

TEST(AudioSync, BeepFixtureOnsetsStayAligned)
{
    const QString path = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                             .filePath(QStringLiteral("sync.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing sync.mp4";

    Asset asset;
    asset.id = QStringLiteral("sync");
    asset.path = path;
    asset.info.kind = MediaKind::Video;
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.durationUs = 10'000'000;
    asset.info.audioSampleRate = 48000;
    asset.info.supported = true;

    Project project;
    project.assets.append(asset);
    Track track;
    track.id = QStringLiteral("a1");
    track.type = TrackType::Audio;
    Clip clip;
    clip.id = QStringLiteral("c1");
    clip.assetId = asset.id;
    clip.sourceOutUs = 10'000'000;
    track.clips.append(clip);
    project.tracks.append(track);

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    // Render slightly more than one second: the 1.0 s beep starts at sample
    // 48000 (exclusive end of the first second), so a 48000-frame block would
    // not contain its onset. One extra 10 ms block keeps the integer-time
    // assertion below intact.
    constexpr int kFrames = 48480;
    std::vector<float> out(size_t(kFrames) * 2);
    mixer.render(out.data(), kFrames, nullptr);

    // Find the first sample above 0.2 after 0.9 s; the beep should start at 1.0 s.
    int firstLoud = -1;
    for (int i = 48000 * 9 / 10; i < kFrames; ++i) {
        if (std::abs(out[size_t(i) * 2]) > 0.2f) {
            firstLoud = i;
            break;
        }
    }
    ASSERT_GE(firstLoud, 0);
    const int64_t onsetUs = int64_t(firstLoud) * 1'000'000 / 48000;
    const int64_t expectedFrame = frameIndexForClock(1'000'000, Rational{30, 1});
    const int64_t onsetFrame = frameIndexForClock(onsetUs, Rational{30, 1});
    EXPECT_LE(std::llabs(onsetFrame - expectedFrame), 1);
}
