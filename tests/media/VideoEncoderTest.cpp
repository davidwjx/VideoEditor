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

// Encodes `frames` black frames (first one forced to a keyframe) and flushes.
// Failures are recorded with EXPECT so a broken encoder still lets the caller
// assert on whatever packets were produced.
std::vector<EncodedPacket> encodeBlackFrames(VideoEncoder &encoder, int frames,
                                             QString *error)
{
    std::vector<EncodedPacket> packets;
    for (int64_t frame = 0; frame < frames; ++frame) {
        const bool ok = encoder.encodeBlack(frame, frame == 0, &packets, error);
        EXPECT_TRUE(ok) << error->toStdString();
        if (!ok)
            return packets;
    }
    std::vector<EncodedPacket> flushed;
    const bool flushOk = encoder.flush(&flushed, error);
    EXPECT_TRUE(flushOk) << error->toStdString();
    packets.insert(packets.end(), flushed.begin(), flushed.end());
    return packets;
}

// Zero B-frames means packets arrive in display order with pts == dts and one
// frame of duration each, all tied to a submitted frame index.
void expectCfrPacketContract(const std::vector<EncodedPacket> &packets,
                             int frames)
{
    ASSERT_FALSE(packets.empty());
    EXPECT_TRUE(packets.front().key);
    for (size_t i = 0; i < packets.size(); ++i) {
        EXPECT_FALSE(packets[i].data.empty()) << "packet " << i;
        EXPECT_GE(packets[i].pts, 0) << "packet " << i;
        EXPECT_LT(packets[i].pts, frames) << "packet " << i;
        EXPECT_EQ(packets[i].dts, packets[i].pts) << "packet " << i;
        EXPECT_EQ(packets[i].duration, 1) << "packet " << i;
        if (i > 0) {
            EXPECT_GT(packets[i].pts, packets[i - 1].pts) << "packet " << i;
        }
    }
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
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets.front().key);
    EXPECT_EQ(packets.front().pts, 0);
    EXPECT_EQ(packets.front().dts, 0);
    EXPECT_EQ(packets.front().duration, 1);
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

TEST(VideoEncoder, OpenRejectsInvalidFrameRate)
{
    VideoEncoder encoder;
    QString error;
    ExportSettings settings = settingsFor(ExportVideoCodec::H264);
    settings.fps = Rational{0, 1};
    EXPECT_FALSE(encoder.open(descriptorFor(QStringLiteral("h264_mf"),
                                            ExportVideoCodec::H264, false),
                              settings, &error));
    EXPECT_FALSE(error.isEmpty());
}

// The catalog probes every hardware encoder at 320x180. Every row that reports
// available must open through the same path and satisfy the CFR packet
// contract; when the machine exposes no hardware encoder the test skips.
TEST(VideoEncoder, HardwareEncodersEncodeCfrWhenAvailable)
{
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    int exercised = 0;
    for (const EncoderDescriptor &descriptor : catalog.encoders) {
        if (!descriptor.hardware || !descriptor.available)
            continue;
        SCOPED_TRACE(descriptor.key().toStdString());
        VideoEncoder encoder;
        QString error;
        ASSERT_TRUE(encoder.open(descriptor, settingsFor(descriptor.codec),
                                 &error))
            << descriptor.key().toStdString() << ": " << error.toStdString();
        EXPECT_TRUE(encoder.isHardware());
        EXPECT_EQ(encoder.pixelFormat(), encoderPixelFormat(descriptor));
        ASSERT_NE(encoder.context(), nullptr);

        constexpr int kFrames = 5;
        const std::vector<EncodedPacket> packets
            = encodeBlackFrames(encoder, kFrames, &error);
        expectCfrPacketContract(packets, kFrames);
        ++exercised;
    }
    if (exercised == 0)
        GTEST_SKIP() << "本机没有可用的硬件视频编码器，跳过硬件编码测试。";
}

// H.265 is hardware-only, so its encode path is only reachable through a
// hardware row. Skips with a Chinese reason when none is present.
TEST(VideoEncoder, H265HardwareEncodesCfrWhenAvailable)
{
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    const EncoderDescriptor *chosen = nullptr;
    for (const EncoderDescriptor &descriptor : catalog.encoders) {
        if (descriptor.codec == ExportVideoCodec::H265 && descriptor.hardware
            && descriptor.available) {
            chosen = &descriptor;
            break;
        }
    }
    if (!chosen)
        GTEST_SKIP()
            << "本机没有可用的 H.265 硬件编码器，跳过 H.265 编码测试。";

    SCOPED_TRACE(chosen->key().toStdString());
    VideoEncoder encoder;
    QString error;
    ASSERT_TRUE(encoder.open(*chosen, settingsFor(ExportVideoCodec::H265),
                             &error))
        << chosen->key().toStdString() << ": " << error.toStdString();
    EXPECT_TRUE(encoder.isHardware());
    EXPECT_EQ(encoder.pixelFormat(), encoderPixelFormat(*chosen));

    constexpr int kFrames = 5;
    const std::vector<EncodedPacket> packets
        = encodeBlackFrames(encoder, kFrames, &error);
    expectCfrPacketContract(packets, kFrames);
}
