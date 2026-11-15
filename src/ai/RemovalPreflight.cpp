#include "RemovalPreflight.h"

#include <algorithm>

#include <QCryptographicHash>

#include "AssetFingerprint.h"

namespace VideoEditor {

namespace {

// Deterministic identity for a model bundle: sorted "fileKey:sha256" pairs.
// Model ids alone are not enough because an approved record can be replaced by
// a different binary bundle without changing its id.
QString modelBundleHash(const AiModelRecord *record)
{
    if (!record)
        return QString();
    QStringList entries;
    for (auto it = record->files.cbegin(); it != record->files.cend(); ++it)
        entries.append(it.key() + QLatin1Char(':') + it.value().sha256);
    std::sort(entries.begin(), entries.end());
    return QString::fromLatin1(
        QCryptographicHash::hash(entries.join(QLatin1Char('\n')).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

QString cacheMessageFor(const QString &clipId)
{
    return QStringLiteral(
               "片段“%1”的 AI 修复结果缺失或已过期。请先生成修复，再导出。")
        .arg(clipId);
}

} // namespace

RemovalKeyInput RemovalPreflight::keyInputFor(const Project &project,
                                              const Clip &clip,
                                              const RemovalLayer &layer,
                                              const ModelRegistry &models)
{
    RemovalKeyInput input;
    const Asset *asset = project.assetById(clip.assetId);
    input.assetFingerprint = asset ? fingerprintFile(asset->path) : QString();
    input.sourceInUs = layer.sourceInUs;
    input.sourceOutUs = layer.sourceOutUs;
    input.maskKeyframes = layer.maskKeyframes;
    input.parameters = layer.parameters;

    const AiModelRecord *tracking = models.defaultTrackingModel();
    const AiModelRecord *inpaint = models.defaultInpaintModel(QString());
    input.trackingModelId = tracking ? tracking->id : QString();
    input.trackingModelHash = modelBundleHash(tracking);
    input.inpaintModelId = inpaint ? inpaint->id : QString();
    input.inpaintModelHash = modelBundleHash(inpaint);

    const QString manifestVersion = models.manifest().pipelineVersion;
    input.pipelineVersion =
        manifestVersion.isEmpty() ? QStringLiteral("m4.0") : manifestVersion;
    input.colorSpace = project.timeline.colorSpace;

    const Rational fps = asset && asset->info.frameRate.isValid()
                             ? asset->info.frameRate
                             : project.timeline.fps;
    input.fps = fps.isValid() ? fps : Rational{30, 1};

    const QSize size =
        asset ? displaySizeForAsset(*asset)
              : QSize(project.timeline.width, project.timeline.height);
    input.width = size.width() > 0 ? size.width() : project.timeline.width;
    input.height = size.height() > 0 ? size.height() : project.timeline.height;
    return input;
}

RemovalPreflightResult RemovalPreflight::check(const Project &project,
                                               const RemovalCache &cache,
                                               const ModelRegistry &models)
{
    RemovalPreflightResult result;

    QString gateReason;
    const bool enabled = models.featureEnabled(&gateReason);
    QStringList messageLines;

    const auto addIssue = [&result](const QString &clipId,
                                    const RemovalLayer &layer,
                                    const Clip &clip, const QString &reason) {
        RemovalPreflightIssue issue;
        issue.clipId = clipId;
        issue.layerId = layer.id;
        issue.startUs = clip.timelineStartUs;
        issue.endUs = clip.timelineEndUs();
        issue.reason = reason;
        result.issues.append(issue);
    };

    for (const Track &track : project.tracks) {
        if (track.type != TrackType::Video || !track.enabled)
            continue;
        for (const Clip &clip : track.clips) {
            if (!clip.enabled || clip.removalLayerId.isEmpty())
                continue;
            const RemovalLayer *layer =
                project.removalLayerById(clip.removalLayerId);
            if (!layer) {
                RemovalPreflightIssue issue;
                issue.clipId = clip.id;
                issue.startUs = clip.timelineStartUs;
                issue.endUs = clip.timelineEndUs();
                issue.reason = QStringLiteral("AI 修复层缺失。");
                result.issues.append(issue);
                messageLines.append(cacheMessageFor(clip.id));
                continue;
            }
            if (!layer->enabled)
                continue;

            // When the feature is disabled the real problem is the missing
            // model/approval, not an "expired cache". Report the gate's own
            // reason so a model install failure is never misdiagnosed.
            if (!enabled) {
                addIssue(clip.id, *layer, clip, gateReason);
                messageLines.append(QStringLiteral("片段“%1”：%2")
                                        .arg(clip.id, gateReason));
                continue;
            }

            if (layer->trackingState == RemovalTrackingState::Tracking
                || layer->trackingState == RemovalTrackingState::Partial
                || layer->trackingState == RemovalTrackingState::Failed) {
                addIssue(clip.id, *layer, clip,
                         QStringLiteral("AI 跟踪尚未完成。"));
                messageLines.append(
                    QStringLiteral("片段“%1”的 AI 跟踪尚未完成。"
                                   "请先生成修复，再导出。")
                        .arg(clip.id));
                continue;
            }

            const Asset *asset = project.assetById(clip.assetId);
            if (!asset) {
                addIssue(clip.id, *layer, clip,
                         QStringLiteral("素材缺失"));
                messageLines.append(cacheMessageFor(clip.id));
                continue;
            }

            const RemovalKeyInput input =
                keyInputFor(project, clip, *layer, models);
            const QString key = computeRemovalCacheKey(input);
            if (key != layer->cacheKey) {
                addIssue(clip.id, *layer, clip,
                         QStringLiteral(
                             "AI 修复结果已过期（素材、遮罩、参数或模型已变化）。"));
                messageLines.append(cacheMessageFor(clip.id));
                continue;
            }

            QString verifyError;
            if (!cache.verify(key, &verifyError)) {
                addIssue(clip.id, *layer, clip,
                         QStringLiteral("AI 修复结果缺失或已损坏。"));
                messageLines.append(cacheMessageFor(clip.id));
                continue;
            }

            if (layer->trackingState == RemovalTrackingState::LowConfidence) {
                // Low confidence is actionable but does not block export.
                messageLines.append(
                    QStringLiteral("片段“%1”的跟踪置信度较低，"
                                   "导出前建议检查修复结果。")
                        .arg(clip.id));
            }
        }
    }

    result.ok = result.issues.isEmpty();
    result.message = messageLines.join(QLatin1Char('\n'));
    return result;
}

} // namespace VideoEditor
