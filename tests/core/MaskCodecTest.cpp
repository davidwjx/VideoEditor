#include <gtest/gtest.h>
#include <QRegularExpression>
#include "MaskCodec.h"

using namespace VideoEditor;

TEST(MaskCodec, RoundTripsRandomMask)
{
    BinaryMask mask;
    mask.width = 37;
    mask.height = 23;
    mask.bits.assign(mask.width * mask.height, 0);
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            mask.set(x, y, ((x * 7 + y * 13) % 11) < 5);

    const QByteArray encoded = MaskCodec::encode(mask);
    QString error;
    const auto decoded = MaskCodec::decode(encoded, &error);
    ASSERT_TRUE(decoded.has_value()) << error.toStdString();
    EXPECT_EQ(decoded->width, mask.width);
    EXPECT_EQ(decoded->height, mask.height);
    EXPECT_EQ(decoded->bits, mask.bits);
}

TEST(MaskCodec, HashIsStableAndChangesWithPixels)
{
    BinaryMask a;
    a.width = a.height = 16;
    a.bits.assign(256, 0);
    a.set(8, 8, true);
    BinaryMask b = a;
    EXPECT_EQ(MaskCodec::hash(a), MaskCodec::hash(b));

    b.set(9, 8, true);
    EXPECT_NE(MaskCodec::hash(a), MaskCodec::hash(b));
    EXPECT_EQ(MaskCodec::hash(a).size(), 64);
    EXPECT_TRUE(MaskCodec::hash(a).contains(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))));
}

TEST(MaskCodec, RejectsTruncatedPayload)
{
    BinaryMask mask;
    mask.width = mask.height = 8;
    mask.bits.assign(64, 0);
    QByteArray encoded = MaskCodec::encode(mask);
    encoded.truncate(encoded.size() - 4);
    QString error;
    EXPECT_FALSE(MaskCodec::decode(encoded, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));
}

TEST(MaskCodec, RejectsBadMagicOversizeAndRunTotalMismatch)
{
    BinaryMask mask;
    mask.width = mask.height = 8;
    mask.bits.assign(64, 0);
    QByteArray badMagic = MaskCodec::encode(mask);
    badMagic[0] = 'X';
    QString error;
    EXPECT_FALSE(MaskCodec::decode(badMagic, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));

    const auto put = [](QByteArray &data, quint32 value) {
        data.append(char(value & 0xff));
        data.append(char((value >> 8) & 0xff));
        data.append(char((value >> 16) & 0xff));
        data.append(char((value >> 24) & 0xff));
    };

    QByteArray oversize("VEMASK01", 8);
    put(oversize, 8193);
    put(oversize, 1);
    put(oversize, 0);
    error.clear();
    EXPECT_FALSE(MaskCodec::decode(oversize, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));

    QByteArray badTotal("VEMASK01", 8);
    put(badTotal, 8);
    put(badTotal, 8);
    put(badTotal, 2);
    put(badTotal, 10);
    put(badTotal, 10); // 20 pixels, header says 64
    error.clear();
    EXPECT_FALSE(MaskCodec::decode(badTotal, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));
}

TEST(MaskEditor, StrokeAndBoxAreClampedAndDeterministic)
{
    BinaryMask mask;
    mask.width = 64;
    mask.height = 64;
    mask.bits.assign(4096, 0);
    MaskEditor::paintStroke(mask, QPoint(-10, 32), QPoint(80, 32), 5, false);
    const QRect bounds = MaskEditor::contentBounds(mask);
    EXPECT_FALSE(bounds.isEmpty());
    EXPECT_GE(bounds.left(), 0);
    EXPECT_LE(bounds.right(), 63);

    BinaryMask boxed;
    boxed.width = boxed.height = 64;
    boxed.bits.assign(4096, 0);
    MaskEditor::paintBox(boxed, QRect(50, 50, 40, 40), false);
    EXPECT_TRUE(boxed.at(63, 63));
    EXPECT_FALSE(boxed.at(0, 0));
}
