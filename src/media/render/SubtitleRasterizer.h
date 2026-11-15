#pragma once

#include <QImage>
#include <QList>
#include <QString>

#include "TimelineEvaluator.h"

namespace VideoEditor {

// Rasterizes the subtitle layers active at one timeline instant into a single
// transparent full-frame image. This is edit-time CPU work: the worker sends
// the image to the GPU as one layer, and re-rasterizes only when the cue set,
// its styles or the output resolution change.
class SubtitleRasterizer {
public:
    static QImage render(const QList<SubtitleLayer> &layers, int width,
                         int height, const QList<SubtitleStyle> &styles);

    // Stable key for the rendered image: cue ids/text, resolved style fields,
    // positions and output size. Equal keys mean the cached image is reusable.
    static QString cacheKey(const QList<SubtitleLayer> &layers, int width,
                            int height);
};

} // namespace VideoEditor
