#include <gtest/gtest.h>

#include <QFileInfo>
#include <QImage>

#include "FrameReader.h"

using namespace VideoEditor;

namespace {

QString fixturePath(const QString &name)
{
    return QStringLiteral(VIDEOEDITOR_FIXTURE_DIR) + QLatin1Char('/') + name;
}

} // namespace

TEST(FrameReader, DecodesSequentialAndBackward)
{
    const QString path = fixturePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/h264.mp4 is missing";

    FrameReader reader;
    QString error;
    ASSERT_TRUE(reader.open(path, Rational{30, 1}, 0, 2'000'000,
                            QSize(320, 180), &error))
        << error.toStdString();
    EXPECT_EQ(reader.frameCount(), 60);
    EXPECT_EQ(reader.displaySize(), QSize(320, 180));

    const QImage first = reader.readFrame(0, &error);
    ASSERT_FALSE(first.isNull()) << error.toStdString();
    EXPECT_EQ(first.size(), QSize(320, 180));

    const QImage third = reader.readFrame(3, &error);
    ASSERT_FALSE(third.isNull()) << error.toStdString();

    // Backwards read: seeks to the keyframe and decodes forward again.
    const QImage second = reader.readFrame(1, &error);
    ASSERT_FALSE(second.isNull()) << error.toStdString();
    EXPECT_EQ(second.size(), QSize(320, 180));

    // A different display size scales the decoded frame.
    FrameReader scaled;
    ASSERT_TRUE(scaled.open(path, Rational{30, 1}, 0, 1'000'000,
                            QSize(640, 360), &error))
        << error.toStdString();
    const QImage frame = scaled.readFrame(0, &error);
    ASSERT_FALSE(frame.isNull()) << error.toStdString();
    EXPECT_EQ(frame.size(), QSize(640, 360));
}

TEST(FrameReader, ReadAllWithinBoundedRange)
{
    const QString path = fixturePath(QStringLiteral("red.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/red.mp4 is missing";

    FrameReader reader;
    QString error;
    ASSERT_TRUE(reader.open(path, Rational{30, 1}, 0, 1'000'000,
                            QSize(64, 64), &error))
        << error.toStdString();
    const QList<QImage> frames = reader.readAll(&error);
    ASSERT_FALSE(frames.isEmpty()) << error.toStdString();
    EXPECT_EQ(frames.size(), reader.frameCount());
    for (const QImage &frame : frames)
        EXPECT_EQ(frame.size(), QSize(64, 64));
}

TEST(FrameReader, RejectsMissingFile)
{
    FrameReader reader;
    QString error;
    EXPECT_FALSE(reader.open(fixturePath(QStringLiteral("does-not-exist.mp4")),
                             Rational{30, 1}, 0, 1'000'000, QSize(64, 64),
                             &error));
    EXPECT_FALSE(error.isEmpty());
}
