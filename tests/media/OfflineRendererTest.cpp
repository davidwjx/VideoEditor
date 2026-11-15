#include <gtest/gtest.h>

#include <array>

#include <QDir>
#include <QFileInfo>

#include "D3D11Context.h"
#include "MediaProbe.h"
#include "OfflineFrameRenderer.h"

using namespace VideoEditor;

namespace {

Project redProject(const QString &nodePath)
{
    Asset asset;
    asset.id = QStringLiteral("red");
    asset.path = nodePath;
    asset.info = *MediaProbe::probe(nodePath);
    Project project;
    project.timeline.width = 320;
    project.timeline.height = 180;
    project.timeline.fps = Rational{30, 1};
    project.assets.append(asset);
    project.ensureDefaultTracks();
    Track *video = nullptr;
    for (Track &track : project.tracks) {
        if (track.type == TrackType::Video) { video = &track; break; }
    }
    EXPECT_NE(video, nullptr);
    Clip clip;
    clip.id = QStringLiteral("c1");
    clip.assetId = asset.id;
    clip.sourceInUs = 0;
    clip.sourceOutUs = asset.info.durationUs;
    clip.timelineStartUs = 0;
    video->clips.append(clip);
    return project;
}

} // namespace

TEST(OfflineRenderer, D3D11UnavailableSkips)
{
    QString error;
    auto context = D3D11Context::create(true, &error);
    if (!context)
        GTEST_SKIP() << error.toStdString();
}

TEST(OfflineRenderer, RedClipCompositesToRedCentreAndBlackLetterbox)
{
    const QString red = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                            .filePath(QStringLiteral("red.mp4"));
    if (!QFileInfo::exists(red))
        GTEST_SKIP() << "missing red.mp4";
    const Project project = redProject(red);
    OfflineFrameRenderer renderer;
    QString error;
    if (!renderer.open(project, 320, 180, &error))
        GTEST_SKIP() << error.toStdString();

    std::vector<uint8_t> rgba;
    ASSERT_TRUE(renderer.renderFrame(0, &rgba, &error)) << error.toStdString();
    ASSERT_EQ(rgba.size(), size_t(320 * 180 * 4));
    const auto pixel = [&rgba](int x, int y) {
        const size_t i = size_t(y * 320 + x) * 4;
        return std::array<int, 3>{rgba[i], rgba[i + 1], rgba[i + 2]};
    };
    const auto centre = pixel(160, 90);
    EXPECT_GT(centre[0], 160);       // red
    EXPECT_LT(centre[1], 90);
    EXPECT_LT(centre[2], 90);
    const auto corner = pixel(2, 2); // square source, letterboxed
    EXPECT_LT(corner[0] + corner[1] + corner[2], 30);
}

TEST(OfflineRenderer, SubtitleIsDrawnAboveVideo)
{
    const QString red = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                            .filePath(QStringLiteral("red.mp4"));
    if (!QFileInfo::exists(red))
        GTEST_SKIP() << "missing red.mp4";
    Project project = redProject(red);
    project.ensureSubtitleStyles();
    for (Track &track : project.tracks) {
        if (track.type != TrackType::Subtitle)
            continue;
        SubtitleCue cue;
        cue.id = QStringLiteral("cue1");
        cue.startUs = 0;
        cue.endUs = 1'000'000;
        cue.text = QStringLiteral("测试字幕");
        track.cues.append(cue);
    }
    OfflineFrameRenderer renderer;
    QString error;
    if (!renderer.open(project, 320, 180, &error))
        GTEST_SKIP() << error.toStdString();

    std::vector<uint8_t> withSubtitle;
    ASSERT_TRUE(renderer.renderFrame(0, &withSubtitle, &error)) << error.toStdString();
    // The subtitle band near y=0.88*height must contain non-red, non-black pixels.
    int bright = 0;
    for (int y = 140; y < 176; ++y) {
        for (int x = 40; x < 280; ++x) {
            const size_t i = size_t(y * 320 + x) * 4;
            if (withSubtitle[i] > 120 && withSubtitle[i + 1] > 120
                && withSubtitle[i + 2] > 120) {
                ++bright;
            }
        }
    }
    EXPECT_GT(bright, 20);
}
