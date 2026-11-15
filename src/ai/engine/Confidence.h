// src/ai/engine/Confidence.h
#pragma once

#include <cstdint>

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace VideoEditor {

// Per-frame confidence used to gate memory/keyframe promotion. The enum order
// is not meaningful; compare by value.
enum class FrameConfidence { Good, Fair, Low, Missing };

struct ConfidenceSample {
    float predictedIou = 0.0f;
    float occlusion = 0.0f;
    int areaPx = 0;
    int previousAreaPx = 0;
    int64_t timeUs = 0;
};

inline float areaChangePercent(int area, int prev)
{
    if (prev <= 0)
        return area > 0 ? 100.0f : 0.0f;
    return qAbs(area - prev) * 100.0f / float(prev);
}

// Good: high IoU, low occlusion and a stable area (<= 30%).
// Missing: heavy occlusion or an empty mask.
// Fair: usable but not memory-worthy (moderate IoU and area change <= 60%).
// Low: everything else.
inline FrameConfidence classifyConfidence(const ConfidenceSample &s)
{
    if (s.occlusion > 0.7f || s.areaPx <= 0)
        return FrameConfidence::Missing;
    const float change = areaChangePercent(s.areaPx, s.previousAreaPx);
    if (s.predictedIou >= 0.85f && s.occlusion <= 0.2f && change <= 30.0f)
        return FrameConfidence::Good;
    // Both conditions are required: a high IoU with an implausible area jump
    // is only Fair, and a large area with a low IoU remains Low.
    if (s.predictedIou >= 0.65f && change <= 60.0f)
        return FrameConfidence::Fair;
    return FrameConfidence::Low;
}

inline bool shouldWriteMemory(FrameConfidence state, float areaChangePct)
{
    return (state == FrameConfidence::Good || state == FrameConfidence::Fair)
        && areaChangePct <= 60.0f;
}

// English state token ("good", "fair", "low", "missing"); the UI translates it.
QString frameConfidenceToString(FrameConfidence state);

// {"frames":[{"timeUs":...,"iou":...,"occlusion":...,"areaPx":...,"state":"good"}]}
QJsonObject confidenceSummary(const QList<ConfidenceSample> &samples);

} // namespace VideoEditor
