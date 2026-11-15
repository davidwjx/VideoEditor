#include "SubtitleRasterizer.h"

#include <algorithm>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRect>
#include <QStringList>
#include <QVector>

namespace VideoEditor {

namespace {

// Safe area: 5% margins left/right and at the bottom, so subtitles never touch
// the frame edge regardless of the project resolution.
constexpr double kSafeMarginFraction = 0.05;

QColor colorFromArgb(quint32 argb)
{
    QColor color;
    color.setRgba(argb);
    return color;
}

const SubtitleStyle &resolveStyle(const SubtitleLayer &layer,
                                  const QList<SubtitleStyle> &styles)
{
    for (const SubtitleStyle &style : styles) {
        if (style.id == layer.style.id)
            return style;
    }
    // TimelineEvaluator already attaches the resolved style; keep it when the
    // caller did not pass a style table that contains the id.
    return layer.style;
}

QFont makeFont(const SubtitleStyle &style, int pixelSize)
{
    QFont font;
    // CJK fallback: the configured family first, then common Chinese fonts,
    // then a Latin fallback. Qt substitutes per glyph for missing coverage.
    QStringList families;
    if (!style.fontFamily.isEmpty())
        families.append(style.fontFamily);
    families.append(QStringLiteral("Microsoft YaHei"));
    families.append(QStringLiteral("SimHei"));
    families.append(QStringLiteral("Segoe UI"));
    font.setFamilies(families);
    font.setPixelSize(std::max(1, pixelSize));
    return font;
}

int fittingPixelSize(const SubtitleStyle &style, const QStringList &lines,
                     int availableWidth)
{
    int pixelSize = style.fontPixelSize > 0 ? style.fontPixelSize : 48;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const QFont font = makeFont(style, pixelSize);
        const QFontMetrics metrics(font);
        int widest = 0;
        for (const QString &line : lines)
            widest = std::max(widest, metrics.horizontalAdvance(line));
        if (widest <= availableWidth || widest <= 0)
            break;
        // Shrink proportionally; the second pass absorbs rounding.
        const int scaled = int(double(pixelSize) * double(availableWidth)
                               / double(widest));
        pixelSize = std::max(8, std::min(pixelSize - 1, scaled));
    }
    return std::max(1, pixelSize);
}

void drawLayer(QPainter &painter, const SubtitleLayer &layer,
               const SubtitleStyle &style, int width, int height)
{
    const QStringList lines = layer.text.split(QLatin1Char('\n'));
    bool hasText = false;
    for (const QString &line : lines)
        hasText = hasText || !line.isEmpty();
    if (!hasText)
        return;

    const int marginX = std::max(1, int(width * kSafeMarginFraction));
    const int marginBottom = std::max(1, int(height * kSafeMarginFraction));
    const int availableWidth = std::max(1, width - 2 * marginX);

    const int pixelSize = fittingPixelSize(style, lines, availableWidth);
    const QFont font = makeFont(style, pixelSize);
    const QFontMetrics metrics(font);
    const int lineHeight = std::max(1, metrics.height());
    const int totalHeight = lineHeight * lines.size();

    int anchorX = qBound(0, width * layer.positionXPermille / 1000, width);
    int anchorY = height * layer.positionYPermille / 1000;
    // Anchor is the bottom edge of the text block; stack lines upwards and keep
    // the whole block inside the safe area.
    anchorY = qBound(totalHeight, anchorY, std::max(totalHeight, height - marginBottom));
    const int top = anchorY - totalHeight;

    struct LineLayout {
        int x = 0;
        int width = 0;
        int baseline = 0;
    };
    QVector<LineLayout> layouts;
    layouts.reserve(lines.size());
    int blockLeft = width;
    int blockRight = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const int textWidth = metrics.horizontalAdvance(lines.at(i));
        // alignment: 1 = left edge on the anchor, 3 = right edge on the
        // anchor, anything else (including the default 2) = centered.
        int x = anchorX - textWidth / 2;
        if (style.alignment == 1)
            x = anchorX;
        else if (style.alignment == 3)
            x = anchorX - textWidth;
        const int maxX = std::max(marginX, width - marginX - textWidth);
        x = qBound(marginX, x, maxX);
        int baseline = top + i * lineHeight + metrics.ascent();
        // Clamp the last line so descenders stay inside the frame.
        baseline = std::min(baseline, height - marginBottom);
        layouts.append({x, textWidth, baseline});
        blockLeft = std::min(blockLeft, x);
        blockRight = std::max(blockRight, x + textWidth);
    }

    const QColor background = colorFromArgb(style.backgroundArgb);
    if (background.alpha() > 0 && blockRight > blockLeft) {
        const int paddingX = std::max(4, pixelSize / 4);
        const int paddingY = std::max(2, pixelSize / 8);
        const QRectF box(blockLeft - paddingX, top - paddingY,
                         blockRight - blockLeft + 2 * paddingX,
                         totalHeight + 2 * paddingY);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(box, 6.0, 6.0);
    }

    const QColor fill = colorFromArgb(style.colorArgb);
    if (style.outlineWidth > 0) {
        painter.setPen(QPen(colorFromArgb(style.outlineColorArgb),
                            style.outlineWidth, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.setBrush(fill);

    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).isEmpty())
            continue;
        QPainterPath path;
        path.addText(layouts.at(i).x, layouts.at(i).baseline, font,
                     lines.at(i));
        painter.drawPath(path);
    }
}

} // namespace

QImage SubtitleRasterizer::render(const QList<SubtitleLayer> &layers,
                                  int width, int height,
                                  const QList<SubtitleStyle> &styles)
{
    QImage image(std::max(1, width), std::max(1, height),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (layers.isEmpty())
        return image;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const SubtitleLayer &layer : layers)
        drawLayer(painter, layer, resolveStyle(layer, styles), image.width(),
                  image.height());

    painter.end();
    return image;
}

QString SubtitleRasterizer::cacheKey(const QList<SubtitleLayer> &layers,
                                     int width, int height)
{
    QStringList parts;
    parts.reserve(layers.size() * 12 + 2);
    for (const SubtitleLayer &layer : layers) {
        const SubtitleStyle &style = layer.style;
        parts.append(layer.trackId);
        parts.append(layer.cueId);
        parts.append(QString::number(qHash(layer.text), 16));
        parts.append(style.id);
        parts.append(style.fontFamily);
        parts.append(QString::number(style.fontPixelSize));
        parts.append(QString::number(style.colorArgb, 16));
        parts.append(QString::number(style.outlineColorArgb, 16));
        parts.append(QString::number(style.outlineWidth));
        parts.append(QString::number(style.backgroundArgb, 16));
        parts.append(QString::number(layer.positionXPermille));
        parts.append(QString::number(layer.positionYPermille));
    }
    parts.append(QString::number(width));
    parts.append(QString::number(height));
    return parts.join(QLatin1Char('|'));
}

} // namespace VideoEditor
