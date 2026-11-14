#include "Srt.h"

#include <algorithm>
#include <optional>
#include <string>

#include <QRegularExpression>
#include <QStringConverter>
#include <QStringList>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace VideoEditor {

namespace {

QString decodeUtf16(const QByteArray &bytes, bool bigEndian)
{
    const int units = (bytes.size() - 2) / 2;
    QByteArray swapped;
    const char *data = bytes.constData() + 2;
    if (bigEndian) {
        swapped.resize(units * 2);
        for (int i = 0; i < units * 2; i += 2) {
            swapped[i] = bytes.constData()[2 + i + 1];
            swapped[i + 1] = bytes.constData()[2 + i];
        }
        data = swapped.constData();
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(data), units);
}

#ifdef _WIN32
// Decodes with an explicit Windows code page. Returns nullopt when the bytes
// cannot be represented in that code page.
std::optional<QString> decodeWindowsCodePage(const QByteArray &bytes, UINT codePage)
{
    if (bytes.isEmpty())
        return QString();
    const int size = int(bytes.size());
    const int wideLength = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
                                               bytes.constData(), size, nullptr, 0);
    if (wideLength <= 0)
        return std::nullopt;
    std::wstring wide(size_t(wideLength), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.constData(), size, wide.data(), wideLength);
    return QString::fromStdWString(wide);
}
#endif

int parseMilliseconds(const QString &digits)
{
    QString padded = digits;
    while (padded.size() < 3)
        padded.append(QLatin1Char('0'));
    return padded.left(3).toInt();
}

bool parseTimeToken(const QRegularExpressionMatch &match, int base, int64_t *us)
{
    const int64_t hours = match.captured(base + 0).toLongLong();
    const int64_t minutes = match.captured(base + 1).toLongLong();
    const int64_t seconds = match.captured(base + 2).toLongLong();
    const int ms = parseMilliseconds(match.captured(base + 3));
    if (hours > 99 || minutes > 59 || seconds > 59)
        return false;
    *us = ((hours * 3600 + minutes * 60 + seconds) * 1000 + ms) * 1000;
    return true;
}

QString normalizeLineBreaks(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

} // namespace

QString decodeTextFile(const QByteArray &bytes)
{
    if (bytes.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(bytes.mid(3));
    if (bytes.startsWith("\xFF\xFE"))
        return decodeUtf16(bytes, false);
    if (bytes.startsWith("\xFE\xFF"))
        return decodeUtf16(bytes, true);

    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8.decode(bytes);
    if (!utf8.hasError())
        return text;

#ifdef _WIN32
    if (const auto ansi = decodeWindowsCodePage(bytes, CP_ACP))
        return *ansi;
    // Systems configured with UTF-8 as their ANSI code page fail CP_ACP for
    // legacy Chinese files, so also try GB18030 (a GBK superset) before
    // falling back to lossy UTF-8.
    if (const auto gb = decodeWindowsCodePage(bytes, 54936))
        return *gb;
#endif
    return QString::fromUtf8(bytes);
}

bool parseSrtTime(const QString &text, int64_t *startUs, int64_t *endUs)
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*$)"));
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch())
        return false;
    return parseTimeToken(match, 1, startUs) && parseTimeToken(match, 5, endUs);
}

SrtParseResult parseSrt(const QByteArray &bytes, const QString &defaultStyleId)
{
    SrtParseResult result;
    const QString content = normalizeLineBreaks(decodeTextFile(bytes));

    const QStringList blocks = content.split(
        QRegularExpression(QStringLiteral("\n[ \t]*\n")), Qt::SkipEmptyParts);
    int cueIndex = 0;
    for (const QString &rawBlock : blocks) {
        QString block = rawBlock;
        while (block.startsWith(QLatin1Char('\n')))
            block.remove(0, 1);
        while (block.endsWith(QLatin1Char('\n')))
            block.chop(1);
        if (block.trimmed().isEmpty())
            continue;
        ++cueIndex;

        const QStringList lines = block.split(QLatin1Char('\n'));
        int timeLine = 0;
        if (!lines.first().contains(QStringLiteral("-->")))
            timeLine = 1;
        if (timeLine >= lines.size()) {
            result.error = QStringLiteral("第 %1 条字幕缺少时间码。").arg(cueIndex);
            return result;
        }

        int64_t startUs = 0;
        int64_t endUs = 0;
        if (!parseSrtTime(lines.at(timeLine), &startUs, &endUs)) {
            result.error = QStringLiteral("第 %1 条字幕时间码无效：%2")
                               .arg(cueIndex)
                               .arg(lines.at(timeLine).trimmed());
            return result;
        }
        if (endUs <= startUs) {
            result.error = QStringLiteral("第 %1 条字幕的结束时间早于或等于开始时间。")
                               .arg(cueIndex);
            return result;
        }

        const QStringList textLines = lines.mid(timeLine + 1);
        if (textLines.isEmpty()) {
            result.error = QStringLiteral("第 %1 条字幕没有文本。").arg(cueIndex);
            return result;
        }
        SubtitleCue cue;
        cue.id = QStringLiteral("cue-%1").arg(cueIndex);
        cue.startUs = startUs;
        cue.endUs = endUs;
        cue.text = textLines.join(QLatin1Char('\n'));
        cue.styleId = defaultStyleId;
        result.cues.append(cue);
    }

    if (result.cues.isEmpty() && result.error.isEmpty())
        result.error = QStringLiteral("SRT 文件中没有可导入的字幕。");
    return result;
}

QString formatSrtTime(int64_t us)
{
    us = qMax<int64_t>(0, us);
    const int64_t totalMs = us / 1000;
    const int64_t ms = totalMs % 1000;
    const int64_t totalSeconds = totalMs / 1000;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

QByteArray writeSrt(const QList<SubtitleCue> &cues)
{
    QList<SubtitleCue> sorted;
    for (const SubtitleCue &cue : cues) {
        if (cue.enabled)
            sorted.append(cue);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const SubtitleCue &a, const SubtitleCue &b) {
                         return a.startUs < b.startUs;
                     });

    QString output;
    int index = 1;
    for (const SubtitleCue &cue : sorted) {
        QString text = normalizeLineBreaks(cue.text);
        while (text.startsWith(QLatin1Char('\n')))
            text.remove(0, 1);
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
        output += QStringLiteral("%1\r\n%2 --> %3\r\n%4\r\n\r\n")
                      .arg(index++)
                      .arg(formatSrtTime(cue.startUs), formatSrtTime(cue.endUs),
                           text);
    }
    QByteArray bytes("\xEF\xBB\xBF", 3);
    bytes += output.toUtf8();
    return bytes;
}

} // namespace VideoEditor
