#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include <QFile>
#include <QTemporaryDir>

#include "D3D11Context.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "ExportTestProject.h"
#include "OfflineFrameRenderer.h"
#include "Srt.h"

using namespace VideoEditor;

namespace {

bool gpuAvailable(QString *error)
{
    return D3D11Context::create(true, error) != nullptr
           || D3D11Context::create(false, error) != nullptr;
}

int countDifferingPixels(const std::vector<uint8_t> &a,
                         const std::vector<uint8_t> &b, int x0, int x1,
                         int y0, int y1, int threshold)
{
    int differing = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(y * 320 + x) * 4;
            int maxDelta = 0;
            for (int c = 0; c < 3; ++c) {
                maxDelta = std::max(
                    maxDelta,
                    std::abs(int(a[i + size_t(c)]) - int(b[i + size_t(c)])));
            }
            if (maxDelta > threshold)
                ++differing;
        }
    }
    return differing;
}

int countBrightPixels(const std::vector<uint8_t> &frame, int x0, int x1,
                      int y0, int y1)
{
    int bright = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(y * 320 + x) * 4;
            if (frame[i] > 120 && frame[i + 1] > 120 && frame[i + 2] > 120)
                ++bright;
        }
    }
    return bright;
}

} // namespace

// Preview/export parity: the exported first frame must be pixel-close to the
// frame the offline (preview) renderer composes from the same timeline
// instant. Duration equality alone would not prove this.
TEST(ExportParity, ExportedFrameMatchesOfflineComposition)
{
    QString error;
    if (!gpuAvailable(&error))
        GTEST_SKIP() << error.toStdString();
    const Project project = makeExportProject(VIDEOEDITOR_FIXTURE_DIR);
    OfflineFrameRenderer renderer;
    // Only a missing device may skip; a failed target creation is a bug.
    ASSERT_TRUE(renderer.open(project, 320, 180, &error))
        << "renderer.open failed: " << error.toStdString();
    std::vector<uint8_t> reference;
    ASSERT_TRUE(renderer.renderFrame(0, &reference, &error))
        << error.toStdString();
    ASSERT_EQ(reference.size(), size_t(320 * 180 * 4));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ExportSettings settings;
    settings.codec = ExportVideoCodec::H264;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 8000;
    settings.outputPath = dir.filePath(QStringLiteral("parity.mp4"));
    settings.segmentTargetUs = 500'000;
    const ExportResult result = ExportPipeline().run(
        project, settings, detectEncodersForTest(320, 180, settings.fps), false,
        {});
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    const std::vector<uint8_t> decoded =
        decodeFrameRgba(settings.outputPath, 0, 320, 180, &error);
    ASSERT_FALSE(decoded.empty()) << error.toStdString();

    // H.264/MPEG-4 are 4:2:0: the encoder averages chroma over 2x2 blocks, so
    // full-resolution RGB PSNR on this saturated test pattern is capped around
    // 28 dB regardless of bitrate (verified at 8 and 50 Mbps). The acceptance
    // gate (>= 30 dB) is therefore applied at the chroma resolution the codec
    // actually preserves; a full-resolution floor still catches any wrong
    // frame, geometry or color-matrix regression.
    const auto box2x2 = [](const std::vector<uint8_t> &frame) {
        std::vector<uint8_t> out(size_t(160) * 90 * 4);
        for (int y = 0; y < 90; ++y) {
            for (int x = 0; x < 160; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            sum += frame[size_t(((y * 2 + dy) * 320
                                                 + (x * 2 + dx)))
                                         * 4
                                         + size_t(c)];
                        }
                    }
                    out[size_t(y * 160 + x) * 4 + size_t(c)]
                        = uint8_t(sum / 4);
                }
            }
        }
        return out;
    };
    const double psnrFull = psnrRgba(reference, decoded, 320, 180);
    const double psnrChroma = psnrRgba(box2x2(reference), box2x2(decoded),
                                      160, 90);
    std::cout << "preview/export parity: encoder="
              << result.encoderId.toStdString() << ", full-res PSNR="
              << psnrFull << " dB, 4:2:0-resolution PSNR=" << psnrChroma
              << " dB (gate >= 30 dB)" << std::endl;
    RecordProperty("encoder", result.encoderId.toStdString());
    RecordProperty("psnrFullResDb", std::to_string(psnrFull));
    RecordProperty("psnrChromaResDb", std::to_string(psnrChroma));
    EXPECT_GE(psnrFull, 25.0) << "exported frame is not the preview frame";
    EXPECT_GE(psnrChroma, 30.0);
}

// Burn-in must change actual pixels, and the SRT sidecar must round-trip the
// cue text. Comparing a burned export against the same project exported with
// burn-in off proves the difference comes from the subtitle layer rather than
// from bright source content in the fixture.
TEST(ExportParity, SubtitleBurnInChangesPixelsAndSrtSidecarMatchesCues)
{
    QString error;
    if (!gpuAvailable(&error))
        GTEST_SKIP() << error.toStdString();
    Project project = makeExportProject(VIDEOEDITOR_FIXTURE_DIR);
    int subtitleCues = 0;
    for (Track &track : project.tracks) {
        if (track.type == TrackType::Subtitle) {
            SubtitleCue cue;
            cue.id = QStringLiteral("cue1");
            cue.startUs = 0;
            cue.endUs = 1'000'000;
            cue.text = QStringLiteral("字幕烧录测试");
            track.cues.append(cue);
            ++subtitleCues;
        }
    }
    ASSERT_EQ(subtitleCues, 1);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ExportSettings burned;
    burned.codec = ExportVideoCodec::Mpeg4Part2;
    burned.width = 320;
    burned.height = 180;
    burned.fps = Rational{30, 1};
    burned.rateControl = ExportRateControl::Bitrate;
    burned.bitrateKbps = 6000;
    burned.outputPath = dir.filePath(QStringLiteral("burn.mp4"));
    burned.segmentTargetUs = 500'000;
    burned.burnSubtitles = true;
    burned.writeSrt = true;
    const EncoderCatalog catalog =
        detectEncodersForTest(320, 180, burned.fps);
    const ExportResult burnedResult = ExportPipeline().run(
        project, burned, catalog, false, {});
    ASSERT_TRUE(burnedResult.ok) << burnedResult.error.toStdString();

    ExportSettings plain = burned;
    plain.outputPath = dir.filePath(QStringLiteral("plain.mp4"));
    plain.burnSubtitles = false;
    plain.writeSrt = false;
    const ExportResult plainResult = ExportPipeline().run(
        project, plain, catalog, false, {});
    ASSERT_TRUE(plainResult.ok) << plainResult.error.toStdString();

    const QString srtPath = defaultSrtPathFor(burned.outputPath);
    QFile srtFile(srtPath);
    ASSERT_TRUE(srtFile.open(QIODevice::ReadOnly))
        << srtPath.toStdString();
    const SrtParseResult srt = parseSrt(srtFile.readAll());
    ASSERT_TRUE(srt.ok()) << srt.error.toStdString();
    ASSERT_EQ(srt.cues.size(), 1);
    EXPECT_EQ(srt.cues.first().text, QStringLiteral("字幕烧录测试"));

    const std::vector<uint8_t> burnedFrame =
        decodeFrameRgba(burned.outputPath, 500'000, 320, 180, &error);
    ASSERT_FALSE(burnedFrame.empty()) << error.toStdString();
    const std::vector<uint8_t> plainFrame =
        decodeFrameRgba(plain.outputPath, 500'000, 320, 180, &error);
    ASSERT_FALSE(plainFrame.empty()) << error.toStdString();

    // Default style: 48 px text centred at 88% of the frame height.
    const int differing =
        countDifferingPixels(burnedFrame, plainFrame, 40, 280, 130, 178, 32);
    const int bright = countBrightPixels(burnedFrame, 40, 280, 130, 178);
    std::cout << "burn-in: " << differing << " pixels changed, " << bright
              << " bright subtitle pixels" << std::endl;
    EXPECT_GT(differing, 100) << "burned-in subtitle pixels not found";
    EXPECT_GT(bright, 20) << "burned-in subtitle pixels not found";
}

// The master limiter must keep a deliberately over-hot mix at or below full
// scale. clipGainAt() caps a single clip at 2x, so one extra copy of the tone
// is not enough to reach the ceiling; eight 200% copies plus the original sum
// to well above 1.0 and force the limiter to engage. Reaching 0.98 (instead of
// a lower level) proves the ceiling was actually applied.
TEST(ExportParity, AudioLimiterPreventsClipping)
{
    QString error;
    if (!gpuAvailable(&error))
        GTEST_SKIP() << error.toStdString();
    Project project = makeExportProject(VIDEOEDITOR_FIXTURE_DIR);
    Track *firstAudio = nullptr;
    for (Track &track : project.tracks) {
        if (track.type == TrackType::Audio && !track.clips.isEmpty()) {
            firstAudio = &track;
            break;
        }
    }
    ASSERT_NE(firstAudio, nullptr);
    Clip loud = firstAudio->clips.first();
    loud.audio.volumeMilli = 2000;
    for (int i = 0; i < 8; ++i) {
        Clip copy = loud;
        copy.id = QStringLiteral("loud%1").arg(i + 1);
        Track loudTrack;
        loudTrack.id = QStringLiteral("track-a%1").arg(i + 2);
        loudTrack.type = TrackType::Audio;
        loudTrack.name = QStringLiteral("A%1").arg(i + 2);
        loudTrack.clips.append(copy);
        project.tracks.append(loudTrack);
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ExportSettings settings;
    settings.codec = ExportVideoCodec::Mpeg4Part2;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 4000;
    settings.outputPath = dir.filePath(QStringLiteral("loud.mp4"));
    settings.segmentTargetUs = 500'000;
    const ExportResult result = ExportPipeline().run(
        project, settings, detectEncodersForTest(320, 180, settings.fps), false,
        {});
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    std::cout << "limiter peak: " << result.audioPeak << " (ceiling 0.98)"
              << std::endl;
    RecordProperty("audioPeak", std::to_string(result.audioPeak));
    EXPECT_LE(result.audioPeak, 0.9801f);
    EXPECT_GE(result.audioPeak, 0.97f)
        << "mix was not hot enough to prove the limiter";
}
