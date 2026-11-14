#include <gtest/gtest.h>

#include <QStringConverter>

#include "Srt.h"

using namespace VideoEditor;

TEST(Srt, ParsesCrLfWithBomAndIndexLines)
{
    QByteArray bytes("\xEF\xBB\xBF", 3);
    bytes += "1\r\n00:00:01,000 --> 00:00:03,500\r\n第一行\r\n第二行\r\n\r\n"
             "2\r\n00:00:04.250 --> 00:00:05.000\r\nHello\r\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 2);
    EXPECT_EQ(result.cues.at(0).startUs, 1'000'000);
    EXPECT_EQ(result.cues.at(0).endUs, 3'500'000);
    EXPECT_EQ(result.cues.at(0).text, QStringLiteral("第一行\n第二行"));
    EXPECT_EQ(result.cues.at(1).startUs, 4'250'000);
    EXPECT_EQ(result.cues.at(1).text, QStringLiteral("Hello"));
    EXPECT_EQ(result.cues.at(1).styleId, QStringLiteral("default"));
}

TEST(Srt, AcceptsShortMillisecondFields)
{
    const QByteArray bytes = "00:00:00,5 --> 00:00:01,25\ntext\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().startUs, 500'000);
    EXPECT_EQ(result.cues.first().endUs, 1'250'000);
}

TEST(Srt, DecodesUtf16Le)
{
    const QString text = QStringLiteral("1\n00:00:00,000 --> 00:00:01,000\n中文字幕\n");
    QByteArray bytes;
    bytes.append('\xFF');
    bytes.append('\xFE');
    const char16_t *units = reinterpret_cast<const char16_t *>(text.utf16());
    for (int i = 0; i < text.size(); ++i) {
        bytes.append(char(units[i] & 0xFF));
        bytes.append(char((units[i] >> 8) & 0xFF));
    }
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().text, QStringLiteral("中文字幕"));
}

TEST(Srt, DecodesUtf16Be)
{
    const QString text = QStringLiteral("1\n00:00:00,000 --> 00:00:01,000\n大端字幕\n");
    QByteArray bytes;
    bytes.append('\xFE');
    bytes.append('\xFF');
    const char16_t *units = reinterpret_cast<const char16_t *>(text.utf16());
    for (int i = 0; i < text.size(); ++i) {
        bytes.append(char((units[i] >> 8) & 0xFF));
        bytes.append(char(units[i] & 0xFF));
    }
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().text, QStringLiteral("大端字幕"));
}

TEST(Srt, DecodesLegacyGbkAnsi)
{
    // "中文字幕" encoded as GBK. Real-world SRT files saved by Chinese tools
    // are frequently GBK rather than UTF-8.
    const QByteArray gbk = QByteArray::fromHex("d6d0cec4d7d6c4bb");
    const QByteArray utf8 = QStringLiteral("中文字幕").toUtf8();
    QStringEncoder systemEncoder(QStringConverter::System);
    const QByteArray systemBytes = systemEncoder.encode(QStringLiteral("中文字幕"));
    if (systemEncoder.hasError() || (systemBytes != gbk && systemBytes != utf8))
        GTEST_SKIP() << "System ANSI code page can neither produce GBK nor UTF-8";

    QByteArray bytes = "1\n00:00:00,000 --> 00:00:01,000\n";
    bytes += gbk;
    bytes += "\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().text, QStringLiteral("中文字幕"));
}

TEST(Srt, ReportsBadTimecodeWithCueIndex)
{
    const SrtParseResult result = parseSrt("1\nnot a timecode\nhello\n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("第 1 条")));
    EXPECT_TRUE(result.error.contains(QStringLiteral("时间码")));
}

TEST(Srt, RejectsInvertedRanges)
{
    const SrtParseResult result = parseSrt(
        "1\n00:00:05,000 --> 00:00:02,000\nhello\n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("结束时间")));
}

TEST(Srt, ExportsUtf8BomCrLfAndRoundTrips)
{
    QList<SubtitleCue> cues;
    SubtitleCue first;
    first.id = QStringLiteral("b");
    first.startUs = 2'000'000;
    first.endUs = 3'000'000;
    first.text = QStringLiteral("第二条");
    SubtitleCue second;
    second.id = QStringLiteral("a");
    second.startUs = 0;
    second.endUs = 1'000'000;
    second.text = QStringLiteral("第一条\n换行");
    cues = {first, second};

    const QByteArray bytes = writeSrt(cues);
    ASSERT_TRUE(bytes.startsWith("\xEF\xBB\xBF"));
    EXPECT_TRUE(bytes.contains("00:00:00,000 --> 00:00:01,000\r\n第一条"));
    EXPECT_TRUE(bytes.contains("\r\n\r\n"));
    EXPECT_TRUE(bytes.endsWith("\r\n"));

    const SrtParseResult parsed = parseSrt(bytes);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toStdString();
    ASSERT_EQ(parsed.cues.size(), 2);
    EXPECT_EQ(parsed.cues.at(0).text, QStringLiteral("第一条\n换行"));
    EXPECT_EQ(parsed.cues.at(1).text, QStringLiteral("第二条"));
}

TEST(Srt, SkipsDisabledCuesOnExport)
{
    QList<SubtitleCue> cues;
    SubtitleCue enabled;
    enabled.id = QStringLiteral("a");
    enabled.startUs = 0;
    enabled.endUs = 1'000'000;
    enabled.text = QStringLiteral("显示");
    SubtitleCue disabled = enabled;
    disabled.id = QStringLiteral("b");
    disabled.text = QStringLiteral("隐藏");
    disabled.enabled = false;
    cues = {enabled, disabled};

    const SrtParseResult parsed = parseSrt(writeSrt(cues));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.cues.size(), 1);
    EXPECT_EQ(parsed.cues.first().text, QStringLiteral("显示"));
}

TEST(Srt, KeepsOverlappingCuesAndStableExportOrder)
{
    const QByteArray bytes =
        "\n\n1\n00:00:00,000 --> 00:00:03,000\nA\n\n"
        "2\n00:00:01,000 --> 00:00:02,000\nB\n\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 2);
    EXPECT_EQ(result.cues.at(0).endUs, 3'000'000);
    EXPECT_EQ(result.cues.at(1).startUs, 1'000'000);

    const SrtParseResult reparsed = parseSrt(writeSrt(result.cues));
    ASSERT_TRUE(reparsed.ok()) << reparsed.error.toStdString();
    ASSERT_EQ(reparsed.cues.size(), 2);
    EXPECT_EQ(reparsed.cues.at(0).text, QStringLiteral("A"));
    EXPECT_EQ(reparsed.cues.at(1).text, QStringLiteral("B"));
}

TEST(Srt, ToleratesArrowPaddingAndExtraBlankLines)
{
    const QByteArray bytes =
        "\n\n00:00:01,000  -->\t00:00:02,500\n带空格\n\n\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().startUs, 1'000'000);
    EXPECT_EQ(result.cues.first().endUs, 2'500'000);
    EXPECT_EQ(result.cues.first().text, QStringLiteral("带空格"));
}

TEST(Srt, ReportsMissingTimecode)
{
    const SrtParseResult result = parseSrt("1\n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("缺少时间码")));
}

TEST(Srt, ReportsMissingText)
{
    const SrtParseResult result = parseSrt(
        "1\n00:00:00,000 --> 00:00:01,000\n\n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("第 1 条")));
    EXPECT_TRUE(result.error.contains(QStringLiteral("没有文本")));
}

TEST(Srt, ReportsWhitespaceOnlyText)
{
    const SrtParseResult result = parseSrt(
        "1\n00:00:00,000 --> 00:00:01,000\n   \n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("没有文本")));
}

TEST(Srt, ReportsEmptyFile)
{
    const SrtParseResult result = parseSrt(QByteArray());
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("没有可导入的字幕")));
}

TEST(Srt, SkipsInvalidRangesAndEmptyTextOnExport)
{
    QList<SubtitleCue> cues;
    SubtitleCue valid;
    valid.id = QStringLiteral("valid");
    valid.startUs = 0;
    valid.endUs = 1'000'000;
    valid.text = QStringLiteral("有效");
    SubtitleCue inverted = valid;
    inverted.id = QStringLiteral("inverted");
    inverted.startUs = 2'000'000;
    inverted.endUs = 1'000'000;
    inverted.text = QStringLiteral("倒序");
    SubtitleCue emptyText = valid;
    emptyText.id = QStringLiteral("empty");
    emptyText.startUs = 3'000'000;
    emptyText.endUs = 4'000'000;
    emptyText.text = QStringLiteral("   ");
    cues = {valid, inverted, emptyText};

    const SrtParseResult parsed = parseSrt(writeSrt(cues));
    ASSERT_TRUE(parsed.ok()) << parsed.error.toStdString();
    ASSERT_EQ(parsed.cues.size(), 1);
    EXPECT_EQ(parsed.cues.first().text, QStringLiteral("有效"));
}

TEST(Srt, FormatsTime)
{
    EXPECT_EQ(formatSrtTime(3'661'234'000), QStringLiteral("01:01:01,234"));
    EXPECT_EQ(formatSrtTime(-1), QStringLiteral("00:00:00,000"));
}
