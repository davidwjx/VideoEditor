#include "TrackingPipeline.h"

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMap>
#include <QSize>

#include "Confidence.h"
#include "FlowPropagator.h"
#include "FrameReader.h"
#include "Sam2Tracker.h"

namespace VideoEditor {

namespace {

int maskArea(const BinaryMask &mask)
{
    return int(std::count_if(mask.bits.begin(), mask.bits.end(),
                             [](uint8_t value) { return value != 0; }));
}

// Guard against a segmentation refinement that jumps to a different object or
// a different part of the frame. Area alone is not enough: a same-sized mask
// can jump far away and still look plausible.
float maskIou(const BinaryMask &a, const BinaryMask &b)
{
    if (!a.isValid() || !b.isValid() || a.width != b.width
        || a.height != b.height) {
        return 0.0f;
    }
    int64_t intersection = 0;
    int64_t unions = 0;
    for (size_t i = 0; i < a.bits.size(); ++i) {
        const bool inA = a.bits[i] != 0;
        const bool inB = b.bits[i] != 0;
        if (inA && inB)
            ++intersection;
        if (inA || inB)
            ++unions;
    }
    return unions > 0 ? float(intersection) / float(unions) : 1.0f;
}


} // namespace

struct TrackingPipeline::Impl {
    Sam2Tracker *tracker = nullptr;
    int refineInterval = 5;

    struct Anchor {
        int index = 0;
        BinaryMask mask;
    };

    struct PendingSample {
        ConfidenceSample sample;
        bool forcedFair = false;
    };

    void emitProgress(const TrackingRunCallbacks &callbacks, int frame,
                      int totalFrames, int completed, QElapsedTimer &clock) const
    {
        if (!callbacks.event || totalFrames <= 0)
            return;
        const double percent =
            std::clamp(100.0 * double(completed) / double(totalFrames), 0.0,
                       100.0);
        qint64 etaMs = 0;
        if (completed > 0) {
            const double perFrame = double(clock.elapsed()) / double(completed);
            etaMs = qint64(perFrame * double(totalFrames - completed));
        }
        callbacks.event(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("progress")},
            {QStringLiteral("stage"), QStringLiteral("tracking")},
            {QStringLiteral("frame"), frame},
            {QStringLiteral("totalFrames"), totalFrames},
            {QStringLiteral("percent"), percent},
            {QStringLiteral("etaMs"), static_cast<double>(etaMs)},
        });
        if (completed % 30 == 0 || completed == totalFrames) {
            const int chunk = completed > 0 ? (completed - 1) / 30 : 0;
            callbacks.event(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("chunkDone")},
                {QStringLiteral("stage"), QStringLiteral("tracking")},
                {QStringLiteral("chunk"), chunk},
                {QStringLiteral("frame"), frame},
                {QStringLiteral("completedFrames"), completed},
            });
        }
    }

    bool writeMask(const AiJobSpec &spec, int frame, const BinaryMask &mask,
                   QString *error) const
    {
        const QString directory =
            QDir(spec.cacheDir).filePath(QStringLiteral("tracking/masks"));
        if (!QDir().mkpath(directory)) {
            if (error)
                *error = QStringLiteral("无法创建跟踪遮罩缓存目录：%1。")
                             .arg(directory);
            return false;
        }
        const QByteArray encoded = MaskCodec::encode(mask);
        if (encoded.isEmpty()) {
            if (error)
                *error = QStringLiteral("跟踪遮罩数据无效。");
            return false;
        }
        const QString path = QDir(directory).filePath(
            QStringLiteral("frame_%1.rle")
                .arg(frame, 6, 10, QLatin1Char('0')));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(encoded) != encoded.size()) {
            if (error)
                *error = QStringLiteral("无法写入跟踪遮罩：%1。").arg(path);
            return false;
        }
        file.close();
        return true;
    }

    QString writeConfidence(const AiJobSpec &spec,
                            const QList<ConfidenceSample> &samples,
                            QString *error) const
    {
        const QByteArray bytes =
            QJsonDocument(confidenceSummary(samples))
                .toJson(QJsonDocument::Compact);
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                .toHex());
        const QString directory =
            QDir(spec.sidecarRoot).filePath(QStringLiteral("tracking"));
        if (!QDir().mkpath(directory)) {
            if (error)
                *error = QStringLiteral("无法创建跟踪置信度目录：%1。")
                             .arg(directory);
            return {};
        }
        const QString path =
            QDir(directory).filePath(hash + QStringLiteral(".json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(bytes) != bytes.size()) {
            if (error)
                *error = QStringLiteral("无法写入跟踪置信度：%1。").arg(path);
            return {};
        }
        file.close();
        return hash;
    }

    QList<Anchor> loadAnchors(const AiJobSpec &spec, int totalFrames,
                              QSize displaySize, QString *error) const
    {
        QList<Anchor> anchors;
        int previousIndex = -1;
        for (const MaskKeyframe &keyframe : spec.maskKeyframes) {
            if (keyframe.sourceTimeUs < spec.sourceInUs
                || keyframe.sourceTimeUs >= spec.sourceOutUs) {
                if (error) {
                    *error = QStringLiteral(
                        "遮罩关键帧时间必须位于跟踪区间内。");
                }
                return {};
            }
            const int index = int(microsToFrames(
                keyframe.sourceTimeUs - spec.sourceInUs, spec.fps));
            if (index < 0 || index >= totalFrames) {
                if (error)
                    *error = QStringLiteral("遮罩关键帧超出跟踪帧范围。");
                return {};
            }
            if (index <= previousIndex) {
                if (error)
                    *error = QStringLiteral("多个遮罩关键帧映射到同一帧。");
                return {};
            }
            const QString path = QDir(spec.sidecarRoot)
                                     .filePath(QStringLiteral("masks/%1.rle")
                                                   .arg(keyframe.maskHash));
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                if (error) {
                    *error = QStringLiteral("无法读取关键帧遮罩：%1。")
                                 .arg(path);
                }
                return {};
            }
            const QByteArray bytes = file.readAll();
            file.close();
            QString decodeError;
            const auto mask = MaskCodec::decode(bytes, &decodeError);
            if (!mask) {
                if (error) {
                    *error = QStringLiteral("关键帧遮罩损坏：%1。")
                                 .arg(decodeError);
                }
                return {};
            }
            if (mask->width != displaySize.width()
                || mask->height != displaySize.height()) {
                if (error) {
                    *error = QStringLiteral(
                        "关键帧遮罩尺寸与任务显示尺寸不一致。");
                }
                return {};
            }
            anchors.append(Anchor{index, *mask});
            previousIndex = index;
        }
        return anchors;
    }

    // Fills the sample from DIS metrics and optionally refines with the
    // tracker. Returns true when a failed refinement forced the sample to
    // Fair (the caller must then preserve that state when reordering).
    void refineOrFallback(int frameIndex, const QImage &image,
                          QSize displaySize, BinaryMask &mask, int &area,
                          bool flowOk, ConfidenceSample *sample,
                          bool *forcedFair) const
    {
        if (forcedFair)
            *forcedFair = false;
        if (!tracker || refineInterval <= 0
            || (frameIndex % refineInterval) != 0)
            return;
        const TrackingPrompt prompt = Sam2Tracker::promptFromMask(mask);
        if (prompt.points.isEmpty())
            return;
        QString segmentError;
        const auto refined = tracker->segment(image, prompt, &segmentError);
        bool plausible = false;
        if (refined && refined->mask.isValid() && refined->areaPx > 0) {
            const int framePixels = displaySize.width() * displaySize.height();
            const float change = areaChangePercent(refined->areaPx, area);
            plausible = refined->areaPx <= framePixels * 3 / 5
                && change <= 60.0f;
        }
        if (plausible) {
            mask = refined->mask;
            area = refined->areaPx;
            sample->predictedIou = refined->predictedIou;
            sample->occlusion = refined->occlusion;
            sample->areaPx = area;
        } else if (flowOk) {
            // Keep the propagated mask, force Fair so a failed refinement
            // never reads as Good.
            sample->predictedIou = 0.70f;
            sample->occlusion = 0.0f;
            sample->previousAreaPx = area;
            sample->areaPx = area;
            if (forcedFair)
                *forcedFair = true;
        }
    }

    TrackingRunResult runCore(
        const AiJobSpec &spec, int totalFrames, QSize displaySize,
        const std::function<QImage(int, QString *)> &frameAt,
        const QList<Anchor> &anchors, const TrackingRunCallbacks &callbacks)
    {
        TrackingRunResult result;
        const auto fail = [&result](const QString &message) {
            result.ok = false;
            result.cancelled = false;
            result.error = message;
            result.state = RemovalTrackingState::Failed;
            return result;
        };

        if (totalFrames <= 0)
            return fail(QStringLiteral("跟踪帧数为零。"));
        if (anchors.isEmpty())
            return fail(QStringLiteral("缺少遮罩关键帧。"));
        if (!anchors.first().mask.isValid())
            return fail(QStringLiteral("起始遮罩无效。"));

        QElapsedTimer clock;
        clock.start();
        QList<ConfidenceSample> samples;
        samples.reserve(totalFrames);

        const auto cancelled = [&callbacks]() {
            return callbacks.cancel && callbacks.cancel();
        };

        // Frames before the first keyframe copy the authored mask with Fair
        // confidence (documented deviation: no backward propagation before the
        // first authored frame).
        const Anchor &first = anchors.first();
        const int firstArea = maskArea(first.mask);
        bool wasCancelled = false;
        for (int i = 0; i < first.index; ++i) {
            if (cancelled()) {
                wasCancelled = true;
                break;
            }
            if (!writeMask(spec, i, first.mask, &result.error))
                return fail(result.error);
            ConfidenceSample sample;
            sample.timeUs = spec.sourceInUs + framesToMicros(i, spec.fps);
            sample.predictedIou = firstArea > 0 ? 0.80f : 0.0f;
            sample.occlusion = 0.0f;
            sample.areaPx = firstArea;
            sample.previousAreaPx = firstArea;
            samples.append(sample);
            emitProgress(callbacks, i, totalFrames, samples.size(), clock);
        }

        for (int k = 0; k < anchors.size() && !wasCancelled; ++k) {
            const Anchor &anchor = anchors.at(k);
            if (cancelled()) {
                wasCancelled = true;
                break;
            }

            const int anchorArea = maskArea(anchor.mask);
            if (!writeMask(spec, anchor.index, anchor.mask, &result.error))
                return fail(result.error);
            ConfidenceSample authored;
            authored.timeUs =
                spec.sourceInUs + framesToMicros(anchor.index, spec.fps);
            authored.predictedIou = anchorArea > 0 ? 1.0f : 0.0f;
            authored.occlusion = 0.0f;
            authored.areaPx = anchorArea;
            authored.previousAreaPx = anchorArea;
            samples.append(authored);
            emitProgress(callbacks, anchor.index, totalFrames, samples.size(),
                         clock);

            const bool hasRightAnchor = (k + 1 < anchors.size());
            const int rightIndex =
                hasRightAnchor ? anchors.at(k + 1).index : totalFrames;
            const int midpoint = hasRightAnchor
                ? (anchor.index + rightIndex) / 2
                : totalFrames - 1;

            QString frameError;
            BinaryMask forwardMask = anchor.mask;
            int forwardArea = anchorArea;
            QImage previousImage;
            if (midpoint > anchor.index) {
                previousImage = frameAt(anchor.index, &frameError);
                if (previousImage.isNull()) {
                    return fail(frameError.isEmpty()
                                    ? QStringLiteral("读取跟踪帧失败。")
                                    : frameError);
                }
            }

            // Forward from the left anchored keyframe to the midpoint.
            for (int i = anchor.index + 1; i <= midpoint; ++i) {
                if (cancelled()) {
                    wasCancelled = true;
                    break;
                }
                QImage currentImage = frameAt(i, &frameError);
                if (currentImage.isNull()) {
                    return fail(frameError.isEmpty()
                                    ? QStringLiteral("读取跟踪帧失败。")
                                    : frameError);
                }

                const FlowPropagationResult flow = m_flow.propagate(
                    previousImage, currentImage, forwardMask);
                BinaryMask mask = forwardMask;
                int area = forwardArea;
                ConfidenceSample sample;
                sample.timeUs = spec.sourceInUs + framesToMicros(i, spec.fps);
                sample.previousAreaPx = forwardArea;
                if (flow.ok) {
                    mask = flow.mask;
                    area = flow.areaPx;
                    sample.predictedIou = std::clamp(
                        1.0f - flow.areaChangePercent / 200.0f, 0.0f, 1.0f);
                    sample.occlusion =
                        std::clamp(flow.meanFlowPx / 50.0f, 0.0f, 1.0f);
                } else {
                    sample.predictedIou = 0.0f;
                    sample.occlusion = 0.5f;
                }
                sample.areaPx = area;
                bool forcedFair = false;
                refineOrFallback(i, currentImage, displaySize, mask, area,
                                 flow.ok, &sample, &forcedFair);

                if (!writeMask(spec, i, mask, &result.error))
                    return fail(result.error);
                samples.append(sample);
                emitProgress(callbacks, i, totalFrames, samples.size(), clock);

                forwardMask = std::move(mask);
                forwardArea = area;
                previousImage = currentImage;
            }
            if (wasCancelled)
                break;

            // Backward from the right anchored keyframe to midpoint+1. This
            // keeps drift bounded when the object moves both ways between
            // authored keyframes.
            if (!hasRightAnchor || rightIndex - 1 <= midpoint)
                continue;

            BinaryMask backwardMask = anchors.at(k + 1).mask;
            int backwardArea = maskArea(backwardMask);
            QImage sourceImage = frameAt(rightIndex, &frameError);
            if (sourceImage.isNull()) {
                return fail(frameError.isEmpty()
                                ? QStringLiteral("读取跟踪帧失败。")
                                : frameError);
            }
            QMap<int, PendingSample> pending;
            for (int i = rightIndex - 1; i > midpoint; --i) {
                if (cancelled()) {
                    wasCancelled = true;
                    break;
                }
                QImage previousFrame = frameAt(i, &frameError);
                if (previousFrame.isNull()) {
                    return fail(frameError.isEmpty()
                                    ? QStringLiteral("读取跟踪帧失败。")
                                    : frameError);
                }

                const FlowPropagationResult flow = m_flow.propagateBackward(
                    previousFrame, sourceImage, backwardMask);
                BinaryMask mask = backwardMask;
                int area = backwardArea;
                ConfidenceSample sample;
                sample.timeUs = spec.sourceInUs + framesToMicros(i, spec.fps);
                sample.previousAreaPx = backwardArea;
                if (flow.ok) {
                    mask = flow.mask;
                    area = flow.areaPx;
                    sample.predictedIou = std::clamp(
                        1.0f - flow.areaChangePercent / 200.0f, 0.0f, 1.0f);
                    sample.occlusion =
                        std::clamp(flow.meanFlowPx / 50.0f, 0.0f, 1.0f);
                } else {
                    sample.predictedIou = 0.0f;
                    sample.occlusion = 0.5f;
                }
                sample.areaPx = area;
                bool forcedFair = false;
                refineOrFallback(i, previousFrame, displaySize, mask, area,
                                 flow.ok, &sample, &forcedFair);

                if (!writeMask(spec, i, mask, &result.error))
                    return fail(result.error);
                pending.insert(i, PendingSample{sample, forcedFair});

                backwardMask = std::move(mask);
                backwardArea = area;
                sourceImage = previousFrame;
            }
            // Samples must stay in temporal order in the confidence JSON; the
            // backward pass discovered them in reverse, so emit the pending
            // entries ascending. A cancel mid-backward still preserves the
            // masks already written and their confidence samples.
            int previousTemporalArea = forwardArea;
            bool firstPending = true;
            for (auto it = pending.constBegin(); it != pending.constEnd();
                 ++it) {
                ConfidenceSample sample = it.value().sample;
                if (firstPending && it.key() > midpoint + 1)
                    previousTemporalArea = sample.areaPx;
                firstPending = false;
                if (!it.value().forcedFair)
                    sample.previousAreaPx = previousTemporalArea;
                samples.append(sample);
                emitProgress(callbacks, it.key(), totalFrames, samples.size(),
                             clock);
                previousTemporalArea = sample.areaPx;
            }
            if (wasCancelled)
                break;
            if (pending.size() != rightIndex - midpoint - 1)
                return fail(QStringLiteral("跟踪帧数不完整。"));
        }

        if (wasCancelled) {
            result.ok = true;
            result.cancelled = true;
            result.completedFrames = samples.size();
            result.state = RemovalTrackingState::Partial;
            result.confidenceHash =
                writeConfidence(spec, samples, &result.error);
            return result;
        }

        if (samples.size() != totalFrames)
            return fail(QStringLiteral("跟踪帧数不完整。"));
        result.completedFrames = samples.size();
        bool low = false;
        for (const ConfidenceSample &sample : samples) {
            const FrameConfidence state = classifyConfidence(sample);
            if (state == FrameConfidence::Low
                || state == FrameConfidence::Missing) {
                low = true;
                break;
            }
        }
        result.ok = true;
        result.state = low ? RemovalTrackingState::LowConfidence
                           : RemovalTrackingState::Complete;
        result.confidenceHash = writeConfidence(spec, samples, &result.error);
        if (result.confidenceHash.isEmpty()) {
            result.ok = false;
            result.state = RemovalTrackingState::Failed;
        }
        return result;
    }

    FlowPropagator m_flow;
};

TrackingPipeline::TrackingPipeline()
    : m_impl(std::make_unique<Impl>())
{
}

TrackingPipeline::~TrackingPipeline() = default;

void TrackingPipeline::setTracker(Sam2Tracker *tracker)
{
    m_impl->tracker = tracker;
}

void TrackingPipeline::setRefineInterval(int interval)
{
    m_impl->refineInterval = std::clamp(interval, 1, 60);
}

TrackingRunResult TrackingPipeline::run(const AiJobSpec &spec,
                                        const TrackingRunCallbacks &callbacks)
{
    TrackingRunResult result;
    if (!spec.fps.isValid()) {
        result.error = QStringLiteral("跟踪帧率无效。");
        return result;
    }
    int totalFrames =
        int(microsToFrames(spec.sourceOutUs - spec.sourceInUs, spec.fps));
    if (totalFrames <= 0) {
        result.error = QStringLiteral("跟踪时间范围不足一帧。");
        return result;
    }
    const QSize displaySize(spec.width, spec.height);
    if (displaySize.width() <= 0 || displaySize.height() <= 0) {
        result.error = QStringLiteral("跟踪显示尺寸无效。");
        return result;
    }

    // Reject a keyframe outside the requested range before touching the asset
    // so the caller always gets the precise keyframe error.
    for (const MaskKeyframe &keyframe : spec.maskKeyframes) {
        if (keyframe.sourceTimeUs < spec.sourceInUs
            || keyframe.sourceTimeUs >= spec.sourceOutUs) {
            result.error = QStringLiteral("遮罩关键帧时间必须位于跟踪区间内。");
            return result;
        }
    }

    QString error;
    FrameReader reader;
    if (!reader.open(spec.assetPath, spec.fps, spec.sourceInUs,
                     spec.sourceOutUs, displaySize, &error)) {
        result.error = error;
        return result;
    }
    // FrameReader clamps the range to the stream duration/nb_frames. The
    // pipeline must use the same bound or it will request a frame past EOF.
    totalFrames = std::min(totalFrames, reader.frameCount());
    if (totalFrames <= 0) {
        result.error = QStringLiteral("跟踪时间范围不足一帧。");
        return result;
    }

    const QList<Impl::Anchor> anchors =
        m_impl->loadAnchors(spec, totalFrames, displaySize, &error);
    if (anchors.isEmpty()) {
        result.error = error.isEmpty()
            ? QStringLiteral("缺少遮罩关键帧。")
            : error;
        return result;
    }

    const auto frameAt = [&reader](int index, QString *frameError) {
        return reader.readFrame(index, frameError);
    };
    return m_impl->runCore(spec, totalFrames, displaySize, frameAt, anchors,
                           callbacks);
}

TrackingRunResult TrackingPipeline::runFrames(
    const AiJobSpec &spec, const QList<QImage> &frames,
    const QList<int> &keyframeFrameIndices,
    const QList<BinaryMask> &keyframeMasks,
    const TrackingRunCallbacks &callbacks)
{
    TrackingRunResult result;
    if (frames.isEmpty()) {
        result.error = QStringLiteral("跟踪输入帧为空。");
        return result;
    }
    const QSize displaySize = frames.first().size();
    if (keyframeFrameIndices.size() != keyframeMasks.size()
        || keyframeFrameIndices.isEmpty()) {
        result.error = QStringLiteral("跟踪关键帧输入不匹配。");
        return result;
    }

    QList<Impl::Anchor> anchors;
    int previousIndex = -1;
    for (int i = 0; i < keyframeFrameIndices.size(); ++i) {
        const int index = keyframeFrameIndices.at(i);
        if (index < 0 || index >= frames.size()) {
            result.error = QStringLiteral("跟踪关键帧索引超出范围。");
            return result;
        }
        if (index <= previousIndex) {
            result.error = QStringLiteral("跟踪关键帧顺序无效。");
            return result;
        }
        if (!keyframeMasks.at(i).isValid()
            || keyframeMasks.at(i).width != displaySize.width()
            || keyframeMasks.at(i).height != displaySize.height()) {
            result.error = QStringLiteral("跟踪关键帧遮罩尺寸无效。");
            return result;
        }
        anchors.append(Impl::Anchor{index, keyframeMasks.at(i)});
        previousIndex = index;
    }

    const auto frameAt = [&frames](int index, QString *) {
        if (index < 0 || index >= frames.size())
            return QImage();
        return frames.at(index);
    };
    return m_impl->runCore(spec, frames.size(), displaySize, frameAt, anchors,
                           callbacks);
}

} // namespace VideoEditor
