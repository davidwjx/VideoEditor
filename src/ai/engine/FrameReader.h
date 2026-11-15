// src/ai/engine/FrameReader.h
#pragma once

#include <cstdint>
#include <memory>

#include <QImage>
#include <QList>
#include <QSize>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

// Decodes a bounded source range with FFmpeg's software decoder and returns
// RGB32 frames at the display geometry (rotation metadata applied and the
// frame scaled to the requested size). Sequential reads decode forward; a
// backwards request seeks to the previous keyframe and decodes forward again.
class FrameReader {
public:
    FrameReader();
    ~FrameReader();
    FrameReader(const FrameReader &) = delete;
    FrameReader &operator=(const FrameReader &) = delete;

    bool open(const QString &path, Rational fps, int64_t sourceInUs,
              int64_t sourceOutUs, QSize displaySize, QString *error = nullptr);
    void close();
    bool isOpen() const;

    // Number of frames in [sourceInUs, sourceOutUs) at the requested rate.
    int frameCount() const;
    QSize displaySize() const;
    int rotationDegrees() const;

    // Index 0 is the first frame at or after sourceInUs. Returns a null image
    // and sets the error on failure.
    QImage readFrame(int index, QString *error = nullptr);
    QList<QImage> readAll(QString *error = nullptr);

    QString lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
