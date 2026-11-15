#include "RemovalPreflight.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QSet>
#include <QStringList>

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

struct RemovalTarget {
    const Clip *clip = nullptr;
    const RemovalLayer *layer = nullptr;
};

// Enabled removal layers that can actually affect an enabled clip. A layer
// whose source range does not overlap the clip's source range (a trimmed
// clip) never becomes active in the evaluator and must not block export.
QList<RemovalTarget> activeRemovalTargets(const Project &project)
{
    QList<RemovalTarget> targets;
    for (const Track &track : project.tracks) {
        if (track.type != TrackType::Video || !track.enabled)
            continue;
        for (const Clip &clip : track.clips) {
            if (!clip.enabled || clip.removalLayerId.isEmpty())
                continue;
            const RemovalLayer *layer =
                project.removalLayerById(clip.removalLayerId);
            if (layer) {
                if (!layer->enabled)
                    continue;
                if (layer->sourceOutUs <= clip.sourceInUs
                    || layer->sourceInUs >= clip.sourceOutUs) {
                    continue;
                }
            }
            targets.append(RemovalTarget{&clip, layer});
        }
    }
    return targets;
}

void addIssue(RemovalPreflightResult *result, const Clip &clip,
              const RemovalLayer *layer, const QString &reason)
{
    RemovalPreflightIssue issue;
    issue.clipId = clip.id;
    issue.layerId = layer ? layer->id : QString();
    issue.startUs = clip.timelineStartUs;
    issue.endUs = clip.timelineEndUs();
    issue.reason = reason;
    result->issues.append(issue);
}

RemovalPreflightResult gateFailure(const QList<RemovalTarget> &targets,
                                   const QString &gateReason)
{
    RemovalPreflightResult result;
    QStringList lines;
    for (const RemovalTarget &target : targets) {
        if (!target.layer) {
            const QString reason = QStringLiteral("AI 修复层缺失。");
            addIssue(&result, *target.clip, nullptr, reason);
            lines.append(cacheMessageFor(target.clip->id));
            continue;
        }
        addIssue(&result, *target.clip, target.layer, gateReason);
        lines.append(
            QStringLiteral("片段“%1”：%2").arg(target.clip->id, gateReason));
    }
    result.ok = result.issues.isEmpty();
    result.message = lines.join(QLatin1Char('\n'));
    return result;
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
    const AiModelRecord *inpaint =
        models.preferredInpaintModel(layer.parameters.mode);
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
                                               const ModelRegistry &models,
                                               bool modelsAlreadyVerified,
                                               bool technicalPreview)
{
    RemovalPreflightResult result;
    // Cheap first pass: export dialogs call this on every setting change.
    // Without an active removal layer there is nothing to verify and no need
    // to hash model bundles or cache chunks.
    const QList<RemovalTarget> targets = activeRemovalTargets(project);
    if (targets.isEmpty())
        return result;

    if (technicalPreview) {
        // The internal technical run intentionally ignores commercial approval
        // and legal sign-off, but it still needs the roles the engine loads.
        const AiModelRecord *tracking = models.defaultTrackingModel();
        if (!tracking) {
            return gateFailure(
                targets,
                QStringLiteral(
                    "内部技术预览不可用：模型清单中没有可用的跟踪模型。"));
        }
        if (!modelsAlreadyVerified) {
            QStringList verifyIds{tracking->id};
            QSet<QString> seen{tracking->id};
            for (const RemovalTarget &target : targets) {
                if (!target.layer)
                    continue;
                const AiModelRecord *inpaint =
                    models.preferredInpaintModel(target.layer->parameters.mode);
                if (!inpaint || seen.contains(inpaint->id))
                    continue;
                seen.insert(inpaint->id);
                verifyIds.append(inpaint->id);
            }
            QString verifyError;
            if (!models.verifyFiles(verifyIds, &verifyError)) {
                return gateFailure(
                    targets,
                    QStringLiteral(
                        "内部技术预览不可用：模型文件缺失或校验失败（%1）。")
                        .arg(verifyError));
            }
        }
    } else {
        QString gateReason;
        const bool enabled =
            models.featureEnabled(&gateReason, !modelsAlreadyVerified);
        if (!enabled)
            return gateFailure(targets, gateReason);
    }

    QStringList messageLines;
    for (const RemovalTarget &target : targets) {
        const Clip &clip = *target.clip;
        const RemovalLayer *layer = target.layer;
        if (!layer) {
            addIssue(&result, clip, nullptr, QStringLiteral("AI 修复层缺失。"));
            messageLines.append(cacheMessageFor(clip.id));
            continue;
        }

        if (layer->trackingState == RemovalTrackingState::Tracking
            || layer->trackingState == RemovalTrackingState::Partial
            || layer->trackingState == RemovalTrackingState::Failed) {
            addIssue(&result, clip, layer, QStringLiteral("AI 跟踪尚未完成。"));
            messageLines.append(
                QStringLiteral("片段“%1”的 AI 跟踪尚未完成。"
                               "请先生成修复，再导出。")
                    .arg(clip.id));
            continue;
        }

        const Asset *asset = project.assetById(clip.assetId);
        if (!asset) {
            addIssue(&result, clip, layer, QStringLiteral("素材缺失"));
            messageLines.append(cacheMessageFor(clip.id));
            continue;
        }

        // Quality must never silently fall back to MI-GAN (or an fp16 LaMa).
        // A mode with no appropriate model cannot have a valid cache.
        if (!models.preferredInpaintModel(layer->parameters.mode)) {
            addIssue(&result, clip, layer,
                     QStringLiteral("没有可用的修复模型。"));
            messageLines.append(
                QStringLiteral("片段“%1”没有可用的修复模型，"
                               "请安装对应的模型后再导出。")
                    .arg(clip.id));
            continue;
        }

        const RemovalKeyInput input = keyInputFor(project, clip, *layer, models);
        const QString key = computeRemovalCacheKey(input);
        if (key != layer->cacheKey) {
            addIssue(&result, clip, layer,
                     QStringLiteral(
                         "AI 修复结果已过期（素材、遮罩、参数或模型已变化）。"));
            messageLines.append(cacheMessageFor(clip.id));
            continue;
        }

        QString verifyError;
        if (!cache.verify(key, &verifyError)) {
            addIssue(&result, clip, layer,
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

    result.ok = result.issues.isEmpty();
    result.message = messageLines.join(QLatin1Char('\n'));
    return result;
}

RemovalPreflightResult RemovalPreflight::checkWithoutModels(
    const Project &project, const QString &reason)
{
    const QList<RemovalTarget> targets = activeRemovalTargets(project);
    if (targets.isEmpty())
        return RemovalPreflightResult{};
    return gateFailure(targets, reason);
}

} // namespace VideoEditor
