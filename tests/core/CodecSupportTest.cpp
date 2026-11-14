#include <gtest/gtest.h>

#include "CodecSupport.h"

using namespace VideoEditor;

TEST(CodecSupport, RecognizesSupportedCodecs)
{
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("mpeg4")).kind,
              VideoCodecKind::Mpeg4Part2);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("h264")).kind,
              VideoCodecKind::H264);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("hevc")).kind,
              VideoCodecKind::H265);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("wmv3")).kind,
              VideoCodecKind::WmvVc1);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("vc1")).kind,
              VideoCodecKind::WmvVc1);
    EXPECT_TRUE(classifyVideoCodec(QStringLiteral("h264")).supported);
}

TEST(CodecSupport, RejectsUnknownCodecWithActionableMessage)
{
    const CodecVerdict verdict = classifyVideoCodec(QStringLiteral("av1"));
    EXPECT_FALSE(verdict.supported);
    EXPECT_TRUE(verdict.reason.contains(QStringLiteral("不支持")));
    EXPECT_TRUE(verdict.reason.contains(QStringLiteral("转码")));
}
