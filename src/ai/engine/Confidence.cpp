#include "Confidence.h"

#include <QJsonArray>

namespace VideoEditor {

QString frameConfidenceToString(FrameConfidence state)
{
    switch (state) {
    case FrameConfidence::Good:
        return QStringLiteral("good");
    case FrameConfidence::Fair:
        return QStringLiteral("fair");
    case FrameConfidence::Low:
        return QStringLiteral("low");
    case FrameConfidence::Missing:
        return QStringLiteral("missing");
    }
    return QStringLiteral("missing");
}

QJsonObject confidenceSummary(const QList<ConfidenceSample> &samples)
{
    QJsonArray frames;
    for (const ConfidenceSample &sample : samples) {
        frames.append(QJsonObject{
            {QStringLiteral("timeUs"), static_cast<double>(sample.timeUs)},
            {QStringLiteral("iou"), static_cast<double>(sample.predictedIou)},
            {QStringLiteral("occlusion"), static_cast<double>(sample.occlusion)},
            {QStringLiteral("areaPx"), sample.areaPx},
            {QStringLiteral("state"),
             frameConfidenceToString(classifyConfidence(sample))},
        });
    }
    return QJsonObject{{QStringLiteral("frames"), frames}};
}

} // namespace VideoEditor
