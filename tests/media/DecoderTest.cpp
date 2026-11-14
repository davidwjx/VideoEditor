#include <gtest/gtest.h>

#include <cstdio>
#include <memory>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "D3D11Context.h"
#include "FramePublisher.h"
#include "FrameReadback.h"
#include "MediaProbe.h"
#include "SharedTextureRing.h"
#include "VideoDecoderFactory.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

// GTEST_SKIP() expands to a return statement and therefore cannot live in a
// value-returning helper; tests skip after checking the returned pointer.
std::unique_ptr<D3D11Context> requireContext(bool video, QString *error)
{
    return D3D11Context::create(video, error);
}

// Surface which path the factory selected so the test log records it.
void notePath(const QString &fixtureName, const IVideoDecoder &decoder)
{
    std::fprintf(stderr, "[decoder-path] %s -> %s\n",
                 fixtureName.toUtf8().constData(),
                 decoder.isHardware() ? "hardware" : "software");
}

} // namespace

TEST(Decoder, DecodesH264FirstFrame)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(true, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());

    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("h264.mp4"), *result.decoder);

    DecodedFrame frame;
    QString error;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    EXPECT_EQ(frame.width, 640);
    EXPECT_EQ(frame.height, 360);
    EXPECT_GE(frame.ptsUs, 0);
    EXPECT_TRUE(frame.kind == DecodedFrame::Kind::HardwareTexture
                || frame.kind == DecodedFrame::Kind::CpuRgba);
}

TEST(Decoder, Decodes10BitH265WithFallback)
{
    const QString path = fixture(QStringLiteral("h265_10bit.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(true, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("h265_10bit.mp4"), *result.decoder);

    DecodedFrame frame;
    QString error;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    EXPECT_EQ(frame.width, 640);
    EXPECT_EQ(frame.height, 360);
}

TEST(Decoder, SeeksToHalfSecond)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(true, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("h264.mp4 (seek)"), *result.decoder);

    QString error;
    ASSERT_TRUE(result.decoder->seek(1'000'000, &error)) << error.toStdString();
    DecodedFrame frame;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    EXPECT_GE(frame.ptsUs, 900'000);
    EXPECT_LT(frame.ptsUs, 1'300'000);
}

TEST(Decoder, FallsBackToSoftwareWhenVideoContextDisabled)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(false, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("h264.mp4 (no video device)"), *result.decoder);
    EXPECT_FALSE(result.decoder->isHardware());
}

TEST(Decoder, HandlesUnicodePath)
{
    const QString path = fixture(QStringLiteral("中文 文件名.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(false, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("中文 文件名.mp4"), *result.decoder);
    DecodedFrame frame;
    ASSERT_TRUE(result.decoder->decodeNext(frame, nullptr));
}

TEST(Decoder, VfrTimestampsAreMonotonic)
{
    const QString path = fixture(QStringLiteral("vfr.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(false, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("vfr.mp4"), *result.decoder);

    int64_t previous = -1;
    for (int i = 0; i < 10; ++i) {
        DecodedFrame frame;
        if (!result.decoder->decodeNext(frame, nullptr))
            break;
        EXPECT_GT(frame.ptsUs, previous);
        previous = frame.ptsUs;
    }
}

TEST(Decoder, PublishesRedFrameToSharedTexture)
{
    const QString path = fixture(QStringLiteral("red.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(false, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    notePath(QStringLiteral("red.mp4"), *result.decoder);

    DecodedFrame frame;
    ASSERT_TRUE(result.decoder->decodeNext(frame, nullptr));

    SharedTextureRing ring(*context);
    QString error;
    ASSERT_TRUE(ring.create(frame.width, frame.height, 1, &error)) << error.toStdString();

    FramePublisher publisher(*context, ring);
    ASSERT_TRUE(publisher.publish(frame, 0, &error)) << error.toStdString();

    const auto rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(frame.width) * frame.height * 4)
        << error.toStdString();
    EXPECT_GT(rgba[0], 200);
    EXPECT_LT(rgba[1], 60);
    EXPECT_LT(rgba[2], 60);
}

TEST(Decoder, PublishesHardwareNv12FrameToSharedTexture)
{
    const QString path = fixture(QStringLiteral("red.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    QString contextError;
    auto context = requireContext(true, &contextError);
    if (!context) GTEST_SKIP() << "D3D11 unavailable: " << contextError.toStdString();

    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    if (!result.decoder->isHardware())
        GTEST_SKIP() << "hardware decoder unavailable: "
                     << result.hardwareFallbackReason.toStdString();
    notePath(QStringLiteral("red.mp4 (hardware publish)"), *result.decoder);

    DecodedFrame frame;
    QString error;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    ASSERT_EQ(frame.kind, DecodedFrame::Kind::HardwareTexture);

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(frame.width, frame.height, 1, &error)) << error.toStdString();

    FramePublisher publisher(*context, ring);
    ASSERT_TRUE(publisher.publish(frame, 0, &error)) << error.toStdString();
    EXPECT_EQ(publisher.lastWrittenSlot(), 0);

    const auto rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(frame.width) * frame.height * 4)
        << error.toStdString();
    EXPECT_GT(rgba[0], 200);
    EXPECT_LT(rgba[1], 60);
    EXPECT_LT(rgba[2], 60);
}
