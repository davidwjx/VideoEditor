#include <gtest/gtest.h>

#include "ExportEncoderCatalog.h"
#include "VideoEncoder.h"

using namespace VideoEditor;

namespace {

ExportSettings settingsFor(ExportVideoCodec codec)
{
    ExportSettings settings;
    settings.codec = codec;
    settings.width = 320;
    settings.height = 180;
    settings.fps = Rational{30, 1};
    settings.rateControl = ExportRateControl::Bitrate;
    settings.bitrateKbps = 4000;
    settings.segmentTargetUs = 1'000'000;
    return settings;
}

EncoderDescriptor descriptorFor(const QString &id, ExportVideoCodec codec,
                                bool hardware)
{
    EncoderDescriptor descriptor;
    descriptor.id = id;
    descriptor.codec = codec;
    descriptor.hardware = hardware;
    descriptor.available = true;
    return descriptor;
}

} // namespace

TEST(VideoEncoder, Mpeg4EncodesOneCfrPacketPerFrame)
{
    VideoEncoder encoder;
    QString error;
    ASSERT_TRUE(encoder.open(descriptorFor(QStringLiteral("mpeg4"),
                                           ExportVideoCodec::Mpeg4Part2, false),
                             settingsFor(ExportVideoCodec::Mpeg4Part2), &error))
        << error.toStdString();
    EXPECT_EQ(encoder.pixelFormat(), AV_PIX_FMT_YUV420P);
    EXPECT_FALSE(encoder.isHardware());
    ASSERT_NE(encoder.context(), nullptr);
    std::vector<uint8_t> rgba(320 * 180 * 4, 128);
    int64_t totalPackets = 0;
    for (int64_t frame = 0; frame < 10; ++frame) {
        std::vector<EncodedPacket> packets;
        ASSERT_TRUE(encoder.encodeRgba(rgba.data(), frame, frame == 0,
                                       &packets, &error))
            << error.toStdString();
        for (const EncodedPacket &packet : packets) {
            EXPECT_FALSE(packet.data.empty());
            EXPECT_EQ(packet.pts, frame);
            EXPECT_EQ(packet.dts, frame);
            EXPECT_EQ(packet.duration, 1);
            if (frame == 0)
                EXPECT_TRUE(packet.key);
        }
        totalPackets += int64_t(packets.size());
    }
    std::vector<EncodedPacket> flushed;
    ASSERT_TRUE(encoder.flush(&flushed, &error)) << error.toStdString();
    totalPackets += int64_t(flushed.size());
    EXPECT_EQ(totalPackets, 10); // zero B-frames and no decoder delay
}

TEST(VideoEncoder, H264SoftwareEncodesWithEvenDimensions)
{
    const ExportSettings settings = settingsFor(ExportVideoCodec::H264);
    VideoEncoder encoder;
    QString error;
    ASSERT_TRUE(encoder.open(descriptorFor(QStringLiteral("h264_mf"),
                                           ExportVideoCodec::H264, false),
                             settings, &error))
        << error.toStdString();
    std::vector<uint8_t> rgba(320 * 180 * 4, 200);
    std::vector<EncodedPacket> packets;
    ASSERT_TRUE(encoder.encodeRgba(rgba.data(), 0, true, &packets, &error))
        << error.toStdString();
    std::vector<EncodedPacket> flushed;
    ASSERT_TRUE(encoder.flush(&flushed, &error)) << error.toStdString();
    packets.insert(packets.end(), flushed.begin(), flushed.end());
    ASSERT_FALSE(packets.empty());
    EXPECT_TRUE(packets.front().key);
}

TEST(VideoEncoder, OpenRejectsInvalidSettings)
{
    VideoEncoder encoder;
    QString error;
    ExportSettings settings = settingsFor(ExportVideoCodec::H264);
    settings.width = 321;
    EXPECT_FALSE(encoder.open(descriptorFor(QStringLiteral("h264_mf"),
                                            ExportVideoCodec::H264, false),
                              settings, &error));
    EXPECT_FALSE(error.isEmpty());
}

// The catalog probes every hardware encoder at 320x180. Any row that reports
// available must open through the same path here; when the machine exposes no
// hardware encoder the test skips instead of failing.
TEST(VideoEncoder, HardwareEncoderEncodesWhenAvailable)
{
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    const EncoderDescriptor *chosen = nullptr;
    for (const EncoderDescriptor &descriptor : catalog.encoders) {
        if (descriptor.hardware && descriptor.available) {
            chosen = &descriptor;
            break;
        }
    }
    if (!chosen)
        GTEST_SKIP() << "本机没有可用的硬件视频编码器，跳过硬件编码测试。";

    VideoEncoder encoder;
    QString error;
    ASSERT_TRUE(encoder.open(*chosen, settingsFor(chosen->codec), &error))
        << chosen->id.toStdString() << ": " << error.toStdString();
    EXPECT_TRUE(encoder.isHardware());
    EXPECT_EQ(encoder.pixelFormat(), encoderPixelFormat(*chosen));

    std::vector<EncodedPacket> packets;
    for (int64_t frame = 0; frame < 3; ++frame) {
        ASSERT_TRUE(encoder.encodeBlack(frame, frame == 0, &packets, &error))
            << chosen->id.toStdString() << ": " << error.toStdString();
    }
    std::vector<EncodedPacket> flushed;
    ASSERT_TRUE(encoder.flush(&flushed, &error)) << error.toStdString();
    packets.insert(packets.end(), flushed.begin(), flushed.end());
    EXPECT_FALSE(packets.empty());
    EXPECT_TRUE(packets.front().key);
}
