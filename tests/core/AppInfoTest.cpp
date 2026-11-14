#include <gtest/gtest.h>

#include "AppInfo.h"

TEST(AppInfoTest, ReturnsSemanticVersion)
{
    EXPECT_EQ(VideoEditor::appVersion(), QStringLiteral("0.1.0"));
}
