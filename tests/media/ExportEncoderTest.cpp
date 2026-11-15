#include <gtest/gtest.h>

#include <QString>

#include "ExportEncoderCatalog.h"

using namespace VideoEditor;

TEST(ExportEncoder, Mpeg4SoftwareIsAlwaysAvailable)
{
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    bool found = false;
    for (const EncoderDescriptor &encoder : catalog.encoders) {
        if (encoder.id == QStringLiteral("mpeg4")) {
            found = true;
            EXPECT_TRUE(encoder.available) << encoder.unavailableReason.toStdString();
            EXPECT_FALSE(encoder.hardware);
        }
    }
    EXPECT_TRUE(found);
    bool hardware = true;
    QString reason;
    EXPECT_EQ(catalog.selectEncoder(ExportVideoCodec::Mpeg4Part2, true,
                                    &hardware, &reason),
              QStringLiteral("mpeg4"));
    EXPECT_FALSE(hardware);
    EXPECT_TRUE(reason.isEmpty());
}

TEST(ExportEncoder, H264AlwaysHasSomeEncoder)
{
    const EncoderCatalog catalog = detectEncoders(320, 180, Rational{30, 1});
    bool hardware = false;
    QString reason;
    const QString id = catalog.selectEncoder(ExportVideoCodec::H264, true,
                                             &hardware, &reason);
    EXPECT_FALSE(id.isEmpty()) << reason.toStdString();
    EXPECT_TRUE(catalog.hasAny(ExportVideoCodec::H264));
}

TEST(ExportEncoder, HardwarePreferenceAndSoftwarePreferencePickTheRightRow)
{
    EncoderCatalog catalog;
    EncoderDescriptor hardware;
    hardware.id = QStringLiteral("h264_amf");
    hardware.codec = ExportVideoCodec::H264;
    hardware.hardware = true;
    hardware.available = true;
    EncoderDescriptor software;
    software.id = QStringLiteral("h264_mf");
    software.codec = ExportVideoCodec::H264;
    software.hardware = false;
    software.available = true;
    catalog.encoders = {hardware, software};

    bool isHardware = false;
    QString reason;
    EXPECT_EQ(catalog.selectEncoder(ExportVideoCodec::H264, true, &isHardware,
                                    &reason),
              QStringLiteral("h264_amf"));
    EXPECT_TRUE(isHardware);
    EXPECT_EQ(catalog.selectEncoder(ExportVideoCodec::H264, false, &isHardware,
                                    &reason),
              QStringLiteral("h264_mf"));
    EXPECT_FALSE(isHardware);

    const EncoderCatalog noHardware = EncoderCatalog::withoutHardwareForTest();
    EXPECT_FALSE(noHardware.selectEncoder(ExportVideoCodec::H264, true,
                                          &isHardware, &reason).isEmpty());
    EXPECT_FALSE(isHardware);
}

TEST(ExportEncoder, H265WithoutHardwareIsRejectedWithReason)
{
    const EncoderCatalog catalog = EncoderCatalog::withoutHardwareForTest();
    bool hardware = false;
    QString reason;
    EXPECT_TRUE(catalog.selectEncoder(ExportVideoCodec::H265, true,
                                      &hardware, &reason).isEmpty());
    EXPECT_FALSE(reason.isEmpty());
    EXPECT_NE(reason.indexOf(QStringLiteral("H.265")), -1);
    EXPECT_FALSE(catalog.hasHardware(ExportVideoCodec::H265));
    EXPECT_FALSE(catalog.hasAny(ExportVideoCodec::H265));
}

TEST(ExportEncoder, EncoderKeysDistinguishHardwareAndSoftwareMf)
{
    EncoderCatalog catalog;
    EncoderDescriptor hardware;
    hardware.id = QStringLiteral("h264_mf");
    hardware.hardware = true;
    EncoderDescriptor software = hardware;
    software.hardware = false;
    EXPECT_NE(hardware.key(), software.key());
    catalog.encoders = {hardware, software};
    ASSERT_NE(catalog.findByKey(hardware.key()), nullptr);
    EXPECT_TRUE(catalog.findByKey(hardware.key())->hardware);
    ASSERT_NE(catalog.findByKey(software.key()), nullptr);
    EXPECT_FALSE(catalog.findByKey(software.key())->hardware);
}
