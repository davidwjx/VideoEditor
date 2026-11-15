#include <gtest/gtest.h>

#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

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

    const QImage second = reader.readFrame(1, &error);
    ASSERT_FALSE(second.isNull()) << error.toStdString();
    EXPECT_EQ(second.size(), QSize(320, 180));

    FrameReader scaled;
    ASSERT_TRUE(scaled.open(path, Rational{30, 1}, 0, 1'000'000,
                            QSize(640, 360), &error))
        << error.toStdString();
    const QImage frame = scaled.readFrame(0, &error);
    ASSERT_FALSE(frame.isNull()) << error.toStdString();
    EXPECT_EQ(frame.size(), QSize(640, 360));
}

TEST(FrameReader, ForwardAndBackwardDecodeSamePixels)
{
    const QString path = fixturePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/h264.mp4 is missing";

    FrameReader reader;
    QString error;
    ASSERT_TRUE(reader.open(path, Rational{30, 1}, 0, 2'000'000,
                            QSize(320, 180), &error))
        << error.toStdString();

    QList<QImage> forward;
    for (int i = 0; i <= 10; ++i) {
        const QImage frame = reader.readFrame(i, &error);
        ASSERT_FALSE(frame.isNull()) << error.toStdString();
        forward.append(frame);
    }
    // Reading index 5 now requires a backward seek.
    const QImage rewound = reader.readFrame(5, &error);
    ASSERT_FALSE(rewound.isNull()) << error.toStdString();
    EXPECT_EQ(rewound, forward.at(5));
}

TEST(FrameReader, SourceInOffsetMatchesFullDecode)
{
    const QString path = fixturePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/h264.mp4 is missing";

    QString error;
    FrameReader full;
    ASSERT_TRUE(full.open(path, Rational{30, 1}, 0, 2'000'000,
                          QSize(320, 180), &error))
        << error.toStdString();
    const QImage reference = full.readFrame(15, &error);
    ASSERT_FALSE(reference.isNull()) << error.toStdString();

    FrameReader offset;
    ASSERT_TRUE(offset.open(path, Rational{30, 1}, 500'000, 1'500'000,
                            QSize(320, 180), &error))
        << error.toStdString();
    EXPECT_EQ(offset.frameCount(), 30);
    const QImage first = offset.readFrame(0, &error);
    ASSERT_FALSE(first.isNull()) << error.toStdString();
    EXPECT_EQ(first, reference);
}

TEST(FrameReader, RejectsFpsMismatch)
{
    const QString path = fixturePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/h264.mp4 is missing";

    FrameReader reader;
    QString error;
    EXPECT_FALSE(reader.open(path, Rational{25, 1}, 0, 2'000'000,
                             QSize(320, 180), &error));
    EXPECT_NE(error.indexOf(QStringLiteral("帧率")), -1)
        << error.toStdString();
}

TEST(FrameReader, RejectsVariableFrameRate)
{
    const QString path = fixturePath(QStringLiteral("vfr.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/vfr.mp4 is missing";

    FrameReader reader;
    QString error;
    EXPECT_FALSE(reader.open(path, Rational{30, 1}, 0, 2'000'000,
                             QSize(320, 240), &error));
    EXPECT_NE(error.indexOf(QStringLiteral("VFR")), -1)
        << error.toStdString();
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

TEST(FrameReader, RotatedFixtureSwapsDisplaySize)
{
    const QString path = fixturePath(QStringLiteral("rotated.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "tests/fixtures/rotated.mp4 is missing";

    FrameReader reader;
    QString error;
    ASSERT_TRUE(reader.open(path, Rational{30, 1}, 0, 1'000'000,
                            QSize(360, 640), &error))
        << error.toStdString();
    EXPECT_TRUE(reader.rotationDegrees() == 90
                || reader.rotationDegrees() == 270)
        << reader.rotationDegrees();
    const QImage frame = reader.readFrame(0, &error);
    ASSERT_FALSE(frame.isNull()) << error.toStdString();
    EXPECT_EQ(frame.size(), QSize(360, 640));
}

TEST(FrameReader, AppliesGeneratedRotations)
{
    const QString source = fixturePath(QStringLiteral("h264.mp4"));
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!QFileInfo::exists(source) || ffmpeg.isEmpty())
        GTEST_SKIP() << "h264.mp4 or ffmpeg is unavailable";

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    // ffmpeg's -display_rotation is counter-clockwise; MediaProbe (and
    // FrameReader) report the equivalent clockwise display rotation.
    const struct {
        int angle;
        int expectedRotation;
        QSize expectedSize;
    } cases[] = {
        {90, 270, QSize(360, 640)},
        {180, 180, QSize(640, 360)},
        {270, 90, QSize(360, 640)},
    };

    for (const auto &testCase : cases) {
        const QString output = temp.filePath(
            QStringLiteral("rot%1.mp4").arg(testCase.angle));
        const QStringList arguments{
            QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
            QStringLiteral("-display_rotation"), QString::number(testCase.angle),
            QStringLiteral("-i"), source, QStringLiteral("-c"),
            QStringLiteral("copy"), output};
        const int exitCode = QProcess::execute(ffmpeg, arguments);
        if (exitCode != 0 || !QFileInfo::exists(output)) {
            GTEST_SKIP() << "could not generate a rotation fixture with ffmpeg";
        }

        FrameReader reader;
        QString error;
        ASSERT_TRUE(reader.open(output, Rational{30, 1}, 0, 1'000'000,
                                testCase.expectedSize, &error))
            << error.toStdString();
        EXPECT_EQ(reader.rotationDegrees(), testCase.expectedRotation);
        const QImage frame = reader.readFrame(0, &error);
        ASSERT_FALSE(frame.isNull()) << error.toStdString();
        EXPECT_EQ(frame.size(), testCase.expectedSize);
    }
}
