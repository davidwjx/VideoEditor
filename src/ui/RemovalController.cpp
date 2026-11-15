#include "RemovalController.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QUuid>
#include <QVariantMap>
#include <QtGlobal>

#include "AiFeatureGate.h"
#include "AiJobManager.h"
#include "AiProtocol.h"
#include "MaskEditor.h"
#include "MaskImageProvider.h"
#include "ModelRegistry.h"
#include "RemovalCache.h"
#include "RemovalCacheKey.h"
#include "RemovalErrors.h"
#include "RemovalPreflight.h"

namespace VideoEditor {

namespace {

QString newId(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString confidenceColorFor(const QString &state)
{
    if (state == QLatin1String("good"))
        return QStringLiteral("#4CAF50");
    if (state == QLatin1String("fair"))
        return QStringLiteral("#FFC107");
    if (state == QLatin1String("low"))
        return QStringLiteral("#F44336");
    return QStringLiteral("#9E9E9E");
}

bool isLowConfidenceState(const QString &state)
{
    return state == QLatin1String("low") || state == QLatin1String("missing");
}

bool readMask(const MaskSidecarStore &sidecars, const QString &hash,
              BinaryMask &target, QString *error)
{
    if (hash.isEmpty())
        return false;
    const auto loaded = sidecars.readMask(hash, error);
    if (!loaded)
        return false;
    target = *loaded;
    return true;
}

} // namespace

RemovalController::RemovalController(ProjectDocument &document,
                                     MaskSidecarStore &sidecars,
                                     QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_sidecars(sidecars)
    , m_playheadProvider([] { return qint64(0); })
{
    connect(&m_document, &ProjectDocument::changed, this,
            &RemovalController::refreshFromDocument);
    connect(&m_document, &ProjectDocument::projectReplaced, this,
            &RemovalController::endMask);
    // Timeline keyframe diamonds subscribe to layerChanged. Removal edits also
    // change the document while mask mode is off (undo/redo), so they must
    // notify independently of refreshFromDocument's editor-state early return.
    connect(&m_document, &ProjectDocument::removalLayersChanged, this,
            &RemovalController::layerChanged);
    connect(&m_document, &ProjectDocument::removalLayersChanged, this,
            [this] { notifyAnalysisChanged(); });
    connect(&m_document, &ProjectDocument::projectReplaced, this,
            [this] { notifyAnalysisChanged(); });
}

void RemovalController::setPlayheadProvider(std::function<qint64()> provider)
{
    m_playheadProvider = std::move(provider);
}

void RemovalController::setSeekCallback(std::function<void(qint64)> callback)
{
    m_seekCallback = std::move(callback);
}

void RemovalController::setSelectedClipProvider(
    std::function<QString()> provider)
{
    m_selectedClipProvider = std::move(provider);
}

void RemovalController::setCompareSink(
    std::function<void(const QString &, int)> sink)
{
    m_compareSink = std::move(sink);
    applyCompareMode();
}

void RemovalController::applyCompareMode()
{
    if (m_compareSink)
        m_compareSink(m_compareMode, m_splitPermille);
}

void RemovalController::notifySelectionChanged()
{
    emit layerChanged();
    notifyAnalysisChanged();
}

void RemovalController::setMaskImageProvider(MaskImageProvider *provider)
{
    m_provider = provider;
    if (m_provider && m_working.mask.isValid())
        m_provider->setMask(MaskEditor::toOverlay(m_working.mask));
}

void RemovalController::setJobManager(AiJobManager *manager)
{
    if (m_jobManager == manager)
        return;
    if (m_jobManager)
        QObject::disconnect(m_jobManager, nullptr, this, nullptr);
    m_jobManager = manager;
    if (!m_jobManager)
        return;
    connect(m_jobManager, &AiJobManager::progressChanged, this,
            &RemovalController::progressChanged);
    connect(m_jobManager, &AiJobManager::pausedChanged, this,
            &RemovalController::pausedChanged);
    connect(m_jobManager, &AiJobManager::stageTextChanged, this,
            &RemovalController::stageTextChanged);
    connect(m_jobManager, &AiJobManager::etaTextChanged, this,
            &RemovalController::etaTextChanged);
    connect(m_jobManager, &AiJobManager::jobFinished, this,
            &RemovalController::handleJobFinished);
}

void RemovalController::setCache(RemovalCache *cache)
{
    m_cache = cache;
    emit cacheStatusChanged();
}

void RemovalController::setFeatureGate(AiFeatureGate *gate)
{
    m_featureGate = gate;
    emit featureEnabledChanged();
    emit cacheStatusChanged();
}

bool RemovalController::featureEnabled() const
{
    return m_featureGate && m_featureGate->enabled();
}

QString RemovalController::featureDisabledReason() const
{
    if (!m_featureGate)
        return QStringLiteral("AI 消除功能未启用：未加载模型清单。");
    return m_featureGate->disabledReason();
}

bool RemovalController::technicalPreview() const
{
    return m_featureGate && m_featureGate->technicalPreview();
}

bool RemovalController::paused() const
{
    return m_jobManager && m_jobManager->paused();
}

double RemovalController::progress() const
{
    return m_jobManager ? m_jobManager->progress() : 0.0;
}

QString RemovalController::stageText() const
{
    return m_jobManager ? mapStageText(m_jobManager->stageText()) : QString();
}

QString RemovalController::etaText() const
{
    return m_jobManager ? m_jobManager->etaText() : QString();
}

void RemovalController::setAdvancedOpen(bool open)
{
    if (open == m_advancedOpen)
        return;
    m_advancedOpen = open;
    emit advancedOpenChanged();
}

QString RemovalController::advancedMode() const
{
    const RemovalLayer *layer = activeRemovalLayer();
    return repairModeToString(layer ? layer->parameters.mode : RepairMode::Fast);
}

int RemovalController::processingHeight() const
{
    const RemovalLayer *layer = activeRemovalLayer();
    return layer ? layer->parameters.processingHeight : 1080;
}

int RemovalController::stabilizationStrength() const
{
    const RemovalLayer *layer = activeRemovalLayer();
    return layer ? layer->parameters.stabilizationStrength : 50;
}

bool RemovalController::track()
{
    clearError();
    QString error;
    if (!prepareForJob(&error)) {
        setError(error);
        return false;
    }
    const RemovalLayer *layer = activeRemovalLayer();
    const Clip *clip = layer ? findClip(layer->clipId) : nullptr;
    if (!layer || !clip) {
        setError(QStringLiteral("请先绘制消除遮罩。"));
        return false;
    }

    AiJobSpec spec;
    if (!buildJobSpec(PendingJobKind::Track, &spec, &error)) {
        setError(error);
        return false;
    }

    // The tracking cacheDir is derived from the same key generation will use,
    // so tracking intermediates and repair chunks share one directory. The
    // layer key is written only in the single completion command; a failed
    // start leaves the project untouched.
    m_pendingLayerId = layer->id;
    if (!startPendingJob(PendingJobKind::Track, spec, &error)) {
        setError(error);
        return false;
    }
    return true;
}

bool RemovalController::generate()
{
    clearError();
    QString error;
    if (!prepareForJob(&error)) {
        setError(error);
        return false;
    }
    const RemovalLayer *layer = activeRemovalLayer();
    const Clip *clip = layer ? findClip(layer->clipId) : nullptr;
    if (!layer || !clip) {
        setError(QStringLiteral("请先绘制消除遮罩。"));
        return false;
    }

    AiJobSpec spec;
    if (!buildJobSpec(PendingJobKind::Generate, &spec, &error)) {
        setError(error);
        return false;
    }

    // Export preflight and generation use the same computed key. The layer is
    // updated exactly once when the job finishes (success, cancel or error),
    // so a failed start cannot leave a half-applied project command.
    m_pendingLayerId = layer->id;
    if (!startPendingJob(PendingJobKind::Generate, spec, &error)) {
        setError(error);
        return false;
    }
    return true;
}

void RemovalController::generateForExport()
{
    m_exportAfterGeneration = true;
    if (!generate()) {
        m_exportAfterGeneration = false;
        setExportGenerationPending(false);
        return;
    }
    setExportGenerationPending(true);
}

void RemovalController::cancelExportGeneration()
{
    m_exportAfterGeneration = false;
    setExportGenerationPending(false);
    if (m_generating)
        cancel();
}

void RemovalController::setExportGenerationPending(bool pending)
{
    if (pending == m_exportGenerationPending)
        return;
    m_exportGenerationPending = pending;
    emit exportGenerationPendingChanged();
}

void RemovalController::setExportRequiredMessage(const QString &message)
{
    if (message.isEmpty())
        return;
    if (m_error.isEmpty())
        setError(message);
}

void RemovalController::cancel()
{
    if (m_jobManager)
        m_jobManager->cancel();
}

void RemovalController::pause()
{
    if (m_jobManager)
        m_jobManager->pause();
}

void RemovalController::resume()
{
    if (m_jobManager)
        m_jobManager->resume();
}

void RemovalController::repropagateFromCurrentKeyframe()
{
    const RemovalLayer *layer = activeRemovalLayer();
    const Clip *clip = layer ? findClip(layer->clipId) : nullptr;
    if (!layer || !clip || layer->maskKeyframes.isEmpty()) {
        setError(QStringLiteral("请先添加遮罩关键帧。"));
        return;
    }
    const qint64 sourceUs = sourceTimeAt(*clip, playheadUs());
    const MaskKeyframe *keyframe = nearestKeyframe(*layer, sourceUs);
    if (!keyframe) {
        setError(QStringLiteral("请先添加遮罩关键帧。"));
        return;
    }
    redoRange(keyframe->sourceTimeUs, layer->sourceOutUs);
}

void RemovalController::redoRange(qint64 sourceStartUs, qint64 sourceEndUs)
{
    clearError();
    QString error;
    if (!prepareForJob(&error)) {
        setError(error);
        return;
    }
    if (m_jobManager && m_jobManager->running()) {
        setError(QStringLiteral("已有 AI 任务在运行。"));
        return;
    }
    const RemovalLayer *layer = activeRemovalLayer();
    if (!layer || !m_cache) {
        setError(QStringLiteral("AI 缓存未初始化。"));
        return;
    }
    if (layer->cacheKey.isEmpty()) {
        setError(removalErrorMessage(QStringLiteral("cache_missing"), {}));
        return;
    }

    int firstFrame = 0;
    int lastFrame = 0;
    if (!computeRedoFrames(*layer, sourceStartUs, sourceEndUs, &firstFrame,
                           &lastFrame)) {
        return;
    }

    AiJobSpec spec;
    if (!buildJobSpec(PendingJobKind::Redo, &spec, &error)) {
        setError(error);
        return;
    }
    if (spec.cacheKey != layer->cacheKey) {
        setError(removalErrorMessage(QStringLiteral("cache_missing"), {}));
        return;
    }

    QString invalidateError;
    if (!m_cache->invalidateFrameRange(layer->cacheKey, firstFrame, lastFrame,
                                       &invalidateError)) {
        setError(removalErrorMessage(QStringLiteral("cache_missing"),
                                     invalidateError));
        emit cacheStatusChanged();
        return;
    }
    emit cacheStatusChanged();

    spec.resume = true;
    spec.redoFirstFrame = firstFrame;
    spec.redoLastFrame = lastFrame;

    m_pendingLayerId = layer->id;
    if (!startPendingJob(PendingJobKind::Redo, spec, &error))
        setError(error);
}

void RemovalController::setCompareMode(const QString &mode)
{
    const QString value = mode.trimmed().toLower();
    if (value != QStringLiteral("original")
        && value != QStringLiteral("repair")
        && value != QStringLiteral("split")) {
        return;
    }
    if (value == m_compareMode)
        return;
    m_compareMode = value;
    emit compareModeChanged();
    applyCompareMode();
}

void RemovalController::setSplitPermille(int permille)
{
    const int bounded = qBound(0, permille, 1000);
    if (bounded == m_splitPermille)
        return;
    m_splitPermille = bounded;
    emit splitPermilleChanged();
    applyCompareMode();
}

void RemovalController::setAdvancedMode(const QString &mode)
{
    if (m_generating || (m_jobManager && m_jobManager->running())) {
        setError(QStringLiteral("AI 任务运行中，暂时无法修改高级设置。"));
        return;
    }
    bool ok = false;
    const RepairMode parsed = repairModeFromString(mode, &ok);
    if (!ok)
        return;
    updateParameters([parsed](RemovalParameters &parameters) {
        parameters.mode = parsed;
    }, QStringLiteral("AI 修复模式"));
}

void RemovalController::setStabilizationStrength(int strength)
{
    if (m_generating || (m_jobManager && m_jobManager->running())) {
        setError(QStringLiteral("AI 任务运行中，暂时无法修改高级设置。"));
        return;
    }
    const int bounded = qBound(0, strength, 100);
    updateParameters([bounded](RemovalParameters &parameters) {
        parameters.stabilizationStrength = bounded;
        parameters.temporalStabilization = bounded > 0;
    }, QStringLiteral("AI 时域稳定化"));
}

void RemovalController::setProcessingHeight(int height)
{
    if (m_generating || (m_jobManager && m_jobManager->running())) {
        setError(QStringLiteral("AI 任务运行中，暂时无法修改高级设置。"));
        return;
    }
    const int bounded = qBound(144, height, 2160);
    updateParameters([this, bounded](RemovalParameters &parameters) {
        parameters.processingHeight = bounded;
        int sourceWidth = 16;
        int sourceHeight = 9;
        if (const Clip *clip = activeClip()) {
            const QSize display = displaySizeForClip(*clip);
            if (display.isValid() && !display.isEmpty()) {
                sourceWidth = display.width();
                sourceHeight = display.height();
            }
        }
        const int width = qBound(
            1, qRound(qreal(bounded) * sourceWidth / qMax(1, sourceHeight)),
            8192);
        parameters.processingWidth = width;
    }, QStringLiteral("AI 处理分辨率"));
}

void RemovalController::setBrushSize(int px)
{
    const int bounded = qBound(1, px, 256);
    if (bounded == m_brushSize)
        return;
    m_brushSize = bounded;
    emit brushSizeChanged();
}

void RemovalController::setBoxMode(bool enabled)
{
    if (enabled == m_boxMode)
        return;
    m_boxMode = enabled;
    emit toolChanged();
}

void RemovalController::setEraseMode(bool enabled)
{
    if (enabled == m_eraseMode)
        return;
    m_eraseMode = enabled;
    emit toolChanged();
}

QString RemovalController::currentMaskHash() const
{
    return m_working.mask.isValid() ? MaskCodec::hash(m_working.mask)
                                    : QString();
}

const Clip *RemovalController::findClip(const QString &clipId) const
{
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &clip : track.clips) {
            if (clip.id == clipId)
                return &clip;
        }
    }
    return nullptr;
}

const Clip *RemovalController::activeClip() const
{
    if (!m_activeClipId.isEmpty())
        return findClip(m_activeClipId);
    // Export-driven generation and the confidence strip must work for the
    // selected clip without requiring mask mode first.
    if (m_selectedClipProvider) {
        const QString clipId = m_selectedClipProvider();
        if (!clipId.isEmpty())
            return findClip(clipId);
    }
    return nullptr;
}

const Asset *RemovalController::assetForClip(const Clip &clip) const
{
    return m_document.project().assetById(clip.assetId);
}

const RemovalLayer *RemovalController::currentLayer() const
{
    if (m_working.layerId.isEmpty())
        return nullptr;
    return m_document.removalLayerById(m_working.layerId);
}

qint64 RemovalController::playheadUs() const
{
    return m_playheadProvider ? m_playheadProvider() : qint64(0);
}

QSize RemovalController::displaySizeForClip(const Clip &clip) const
{
    const Asset *asset = assetForClip(clip);
    if (asset) {
        const QSize size = displaySizeForAsset(*asset);
        if (size.isValid() && !size.isEmpty())
            return size;
    }
    return QSize(1920, 1080);
}

BinaryMask RemovalController::emptyMaskForClip(const Clip &clip) const
{
    const QSize size = displaySizeForClip(clip);
    BinaryMask mask;
    mask.width = size.width();
    mask.height = size.height();
    mask.bits.assign(size_t(mask.width) * size_t(mask.height), 0);
    return mask;
}

const MaskKeyframe *RemovalController::keyframeAt(const RemovalLayer &layer,
                                                   qint64 sourceTimeUs)
{
    for (const MaskKeyframe &keyframe : layer.maskKeyframes) {
        if (keyframe.sourceTimeUs == sourceTimeUs)
            return &keyframe;
    }
    return nullptr;
}

const MaskKeyframe *RemovalController::nearestKeyframe(
    const RemovalLayer &layer, qint64 sourceTimeUs)
{
    const MaskKeyframe *best = nullptr;
    qint64 bestDistance = 0;
    for (const MaskKeyframe &keyframe : layer.maskKeyframes) {
        const qint64 distance = std::llabs(keyframe.sourceTimeUs - sourceTimeUs);
        if (!best || distance < bestDistance) {
            best = &keyframe;
            bestDistance = distance;
        }
    }
    return best;
}

int RemovalController::keyframeIndexAt(const RemovalLayer &layer,
                                       qint64 sourceTimeUs)
{
    for (int i = 0; i < layer.maskKeyframes.size(); ++i) {
        if (layer.maskKeyframes.at(i).sourceTimeUs == sourceTimeUs)
            return i;
    }
    return -1;
}

bool RemovalController::loadMask(BinaryMask &target, const QString &maskHash)
{
    QString error;
    if (!readMask(m_sidecars, maskHash, target, &error)) {
        if (!error.isEmpty())
            setError(error);
        return false;
    }
    return true;
}

void RemovalController::setError(const QString &message)
{
    if (message == m_error)
        return;
    m_error = message;
    emit errorChanged();
}

void RemovalController::clearError()
{
    if (m_error.isEmpty())
        return;
    m_error.clear();
    emit errorChanged();
}

void RemovalController::beginMask(const QString &clipId)
{
    clearError();

    const Clip *clip = nullptr;
    bool onVideoTrack = false;
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &candidate : track.clips) {
            if (candidate.id != clipId)
                continue;
            clip = &candidate;
            onVideoTrack = track.type == TrackType::Video;
            break;
        }
        if (clip)
            break;
    }

    const Asset *asset = clip ? assetForClip(*clip) : nullptr;
    if (!clip || !onVideoTrack || !asset || !asset->info.hasVideo) {
        setError(QStringLiteral("只能对视频片段使用 AI 消除。"));
        return;
    }

    resetWorkingState();
    m_maskMode = true;
    m_activeClipId = clipId;
    m_working.clipId = clipId;
    m_working.sourceTimeUs = sourceTimeAt(*clip, playheadUs());
    m_working.baseMask = emptyMaskForClip(*clip);
    m_working.mask = m_working.baseMask;

    const RemovalLayer *layer =
        clip->removalLayerId.isEmpty()
            ? nullptr
            : m_document.removalLayerById(clip->removalLayerId);
    if (layer) {
        m_working.layerId = layer->id;
        m_hasLayer = true;
        m_layerEnabled = layer->enabled;
        const MaskKeyframe *exact =
            keyframeAt(*layer, m_working.sourceTimeUs);
        const MaskKeyframe *source =
            exact ? exact : nearestKeyframe(*layer, m_working.sourceTimeUs);
        if (source) {
            if (exact)
                m_working.keyframeId = exact->id;
            m_featherPx = source->featherPx;
            BinaryMask loaded;
            if (loadMask(loaded, source->maskHash))
                m_working.mask = loaded;
            m_working.baseMask = m_working.mask;
        }
    } else {
        m_working.layerId = newId(QStringLiteral("rl-"));
        m_hasLayer = false;
        m_layerEnabled = true;
        m_featherPx = 4;
    }

    m_working.beforeMaskHash = MaskCodec::hash(m_working.mask);
    emit maskModeChanged();
    emit activeClipChanged();
    emit layerChanged();
    emit featherChanged();
    notifyAnalysisChanged();
    updateOverlay();
}

void RemovalController::endMask()
{
    const bool wasActive = m_maskMode;
    const bool toolsChanged = m_boxMode || m_eraseMode;
    resetWorkingState();
    m_maskMode = false;
    m_activeClipId.clear();
    m_hasLayer = false;
    m_layerEnabled = true;
    m_boxMode = false;
    m_eraseMode = false;

    ++m_maskRevision;
    if (m_provider)
        m_provider->setMask(QImage());
    if (toolsChanged)
        emit toolChanged();
    if (wasActive) {
        emit maskModeChanged();
        emit activeClipChanged();
        emit layerChanged();
        emit maskChanged();
        emit overlayChanged(QImage(), QRectF());
    }
}

void RemovalController::resetWorkingState()
{
    m_working = WorkingState{};
}

bool RemovalController::prepareMaskEditAtPlayhead()
{
    const Clip *clip = activeClip();
    if (!clip)
        return false;

    m_working.sourceTimeUs = sourceTimeAt(*clip, playheadUs());
    m_working.keyframeId.clear();

    const RemovalLayer *layer = currentLayer();
    if (layer) {
        const MaskKeyframe *exact =
            keyframeAt(*layer, m_working.sourceTimeUs);
        const MaskKeyframe *source =
            exact ? exact : nearestKeyframe(*layer, m_working.sourceTimeUs);
        if (source) {
            if (exact)
                m_working.keyframeId = exact->id;
            BinaryMask loaded;
            if (loadMask(loaded, source->maskHash))
                m_working.mask = loaded;
            if (source->featherPx != m_featherPx) {
                m_featherPx = source->featherPx;
                emit featherChanged();
            }
        } else {
            m_working.mask = emptyMaskForClip(*clip);
        }
    } else if (!m_working.mask.isValid()) {
        m_working.mask = emptyMaskForClip(*clip);
    }

    if (!m_working.mask.isValid())
        return false;
    m_working.beforeMaskHash = MaskCodec::hash(m_working.mask);
    return true;
}

void RemovalController::beginStroke(int radiusSourcePx, bool erase)
{
    if (!m_maskMode || m_working.layerId.isEmpty())
        return;
    if (!prepareMaskEditAtPlayhead())
        return;
    m_working.radius = qBound(1, radiusSourcePx, 4096);
    m_working.erase = erase;
    m_working.strokeActive = true;
}

void RemovalController::continueStroke(QPointF fromNorm, QPointF toNorm)
{
    if (!m_working.strokeActive || !m_working.mask.isValid())
        return;

    const BinaryMask &mask = m_working.mask;
    const auto toPoint = [&mask](const QPointF &norm) {
        return QPoint(qRound(norm.x() * mask.width),
                      qRound(norm.y() * mask.height));
    };
    MaskEditor::paintStroke(m_working.mask, toPoint(fromNorm), toPoint(toNorm),
                            m_working.radius, m_working.erase);
    updateOverlay();
}

void RemovalController::endStroke()
{
    if (!m_working.strokeActive)
        return;
    m_working.strokeActive = false;
    commitMaskChange(QStringLiteral("AI 消除遮罩"));
}

void RemovalController::paintBox(QRectF normalized, bool erase)
{
    if (!m_maskMode || m_working.layerId.isEmpty())
        return;
    if (!prepareMaskEditAtPlayhead())
        return;

    const QRectF rect = normalized.normalized();
    const BinaryMask &mask = m_working.mask;
    const QRect pixels(QPoint(qRound(rect.left() * mask.width),
                              qRound(rect.top() * mask.height)),
                       QPoint(qRound(rect.right() * mask.width),
                              qRound(rect.bottom() * mask.height)));
    MaskEditor::paintBox(m_working.mask, pixels, erase);
    commitMaskChange(QStringLiteral("AI 消除遮罩"));
}

bool RemovalController::commitMaskChange(const QString &text)
{
    if (!m_working.mask.isValid())
        return false;
    const QString newHash = MaskCodec::hash(m_working.mask);
    if (newHash == m_working.beforeMaskHash)
        return false;

    const Clip *clip = activeClip();
    if (!clip)
        return false;

    QString storedHash;
    QString error;
    if (!m_sidecars.writeMask(m_working.mask, &storedHash, &error)) {
        setError(error);
        return false;
    }

    const RemovalLayer *existing = currentLayer();
    RemovalLayer after;
    if (existing)
        after = *existing;
    else {
        after.id = m_working.layerId;
        after.clipId = clip->id;
        after.parameters.processingWidth = m_working.mask.width;
        after.parameters.processingHeight = m_working.mask.height;
    }
    after.id = m_working.layerId;
    after.clipId = clip->id;
    after.sourceInUs = clip->sourceInUs;
    after.sourceOutUs = clip->sourceOutUs;

    const int index = keyframeIndexAt(after, m_working.sourceTimeUs);
    if (index >= 0) {
        MaskKeyframe &keyframe = after.maskKeyframes[index];
        keyframe.maskHash = storedHash;
        keyframe.revision += 1;
        m_working.keyframeId = keyframe.id;
    } else {
        MaskKeyframe keyframe;
        keyframe.id = newId(QStringLiteral("mk-"));
        keyframe.sourceTimeUs = m_working.sourceTimeUs;
        keyframe.maskHash = storedHash;
        keyframe.featherPx = m_featherPx;
        keyframe.revision = 1;
        after.maskKeyframes.append(keyframe);
        std::sort(after.maskKeyframes.begin(), after.maskKeyframes.end(),
                  [](const MaskKeyframe &a, const MaskKeyframe &b) {
                      return a.sourceTimeUs < b.sourceTimeUs;
                  });
        m_working.keyframeId = keyframe.id;
    }
    after.revision += 1;

    RemovalLayerEdit edit;
    edit.clipId = clip->id;
    if (existing)
        edit.before = *existing;
    edit.after = after;

    m_working.beforeMaskHash = storedHash;
    m_document.applyRemovalLayerEdit(edit, text);

    m_hasLayer = true;
    m_layerEnabled = after.enabled;
    emit layerChanged();
    updateOverlay();
    return true;
}

void RemovalController::setFeatherPx(int px)
{
    const int bounded = qBound(0, px, 64);
    if (bounded == m_featherPx)
        return;
    m_featherPx = bounded;
    emit featherChanged();

    if (!m_maskMode || m_working.layerId.isEmpty())
        return;
    const RemovalLayer *layer = currentLayer();
    if (!layer)
        return;

    // Prefer the keyframe under the current playhead; the user may have
    // scrubbed since the last mask edit.
    if (const Clip *clip = activeClip())
        m_working.sourceTimeUs = sourceTimeAt(*clip, playheadUs());
    int index = keyframeIndexAt(*layer, m_working.sourceTimeUs);
    if (index < 0 && !m_working.keyframeId.isEmpty()) {
        for (int i = 0; i < layer->maskKeyframes.size(); ++i) {
            if (layer->maskKeyframes.at(i).id == m_working.keyframeId) {
                index = i;
                break;
            }
        }
    }
    if (index < 0)
        return;

    RemovalLayer after = *layer;
    if (after.maskKeyframes[index].featherPx == bounded)
        return;
    after.maskKeyframes[index].featherPx = bounded;
    after.revision += 1;

    RemovalLayerEdit edit;
    edit.clipId = layer->clipId;
    edit.before = *layer;
    edit.after = after;
    m_document.applyRemovalLayerEdit(edit,
                                     QStringLiteral("AI 消除羽化"));
}

void RemovalController::addMaskKeyframeAtPlayhead()
{
    if (!m_maskMode)
        return;
    const Clip *clip = activeClip();
    if (!clip)
        return;
    addMaskKeyframeAtSourceTime(sourceTimeAt(*clip, playheadUs()));
}

void RemovalController::addMaskKeyframeAtSourceTime(qint64 sourceTimeUs,
                                                     const QString &id)
{
    if (!m_maskMode || m_working.layerId.isEmpty())
        return;
    const Clip *clip = activeClip();
    if (!clip)
        return;

    const RemovalLayer *existing = currentLayer();
    RemovalLayer after;
    if (existing)
        after = *existing;
    else {
        after.id = m_working.layerId;
        after.clipId = clip->id;
        after.sourceInUs = clip->sourceInUs;
        after.sourceOutUs = clip->sourceOutUs;
        after.parameters.processingWidth = m_working.mask.isValid()
                                               ? m_working.mask.width
                                               : displaySizeForClip(*clip).width();
        after.parameters.processingHeight =
            m_working.mask.isValid()
                ? m_working.mask.height
                : displaySizeForClip(*clip).height();
    }

    const int index = keyframeIndexAt(after, sourceTimeUs);
    if (index >= 0) {
        if (id.isEmpty() || after.maskKeyframes.at(index).id == id)
            return;
        after.maskKeyframes[index].id = id;
    } else {
        BinaryMask maskForNew;
        const MaskKeyframe *nearest = nearestKeyframe(after, sourceTimeUs);
        int feather = m_featherPx;
        if (nearest) {
            BinaryMask loaded;
            if (readMask(m_sidecars, nearest->maskHash, loaded, nullptr))
                maskForNew = loaded;
            feather = nearest->featherPx;
        }
        if (!maskForNew.isValid())
            maskForNew = m_working.mask.isValid() ? m_working.mask
                                                  : emptyMaskForClip(*clip);

        QString storedHash;
        QString error;
        if (!m_sidecars.writeMask(maskForNew, &storedHash, &error)) {
            setError(error);
            return;
        }

        MaskKeyframe keyframe;
        keyframe.id = id.isEmpty() ? newId(QStringLiteral("mk-")) : id;
        keyframe.sourceTimeUs = sourceTimeUs;
        keyframe.maskHash = storedHash;
        keyframe.featherPx = feather;
        keyframe.revision = 1;
        after.maskKeyframes.append(keyframe);
        std::sort(after.maskKeyframes.begin(), after.maskKeyframes.end(),
                  [](const MaskKeyframe &a, const MaskKeyframe &b) {
                      return a.sourceTimeUs < b.sourceTimeUs;
                  });
    }
    after.revision += 1;

    RemovalLayerEdit edit;
    edit.clipId = clip->id;
    if (existing)
        edit.before = *existing;
    edit.after = after;
    m_document.applyRemovalLayerEdit(edit,
                                     QStringLiteral("AI 消除关键帧"));
}

void RemovalController::removeMaskKeyframe(int index)
{
    if (!m_maskMode)
        return;
    const RemovalLayer *layer = currentLayer();
    if (!layer)
        return;
    if (layer->maskKeyframes.size() <= 1) {
        setError(QStringLiteral("至少保留一个遮罩关键帧。"));
        return;
    }
    if (index < 0 || index >= layer->maskKeyframes.size())
        return;

    RemovalLayer after = *layer;
    after.maskKeyframes.removeAt(index);
    after.revision += 1;

    RemovalLayerEdit edit;
    edit.clipId = layer->clipId;
    edit.before = *layer;
    edit.after = after;
    m_document.applyRemovalLayerEdit(edit,
                                     QStringLiteral("AI 删除遮罩关键帧"));
}

void RemovalController::setLayerEnabled(bool enabled)
{
    const RemovalLayer *layer = currentLayer();
    if (!layer || layer->enabled == enabled)
        return;

    RemovalLayer after = *layer;
    after.enabled = enabled;
    after.revision += 1;

    RemovalLayerEdit edit;
    edit.clipId = layer->clipId;
    edit.before = *layer;
    edit.after = after;
    m_document.applyRemovalLayerEdit(edit,
                                     QStringLiteral("AI 消除层开关"));
    m_layerEnabled = enabled;
    emit layerChanged();
}

void RemovalController::deleteLayer()
{
    const RemovalLayer *layer = currentLayer();
    if (layer) {
        RemovalLayerEdit edit;
        edit.clipId = layer->clipId;
        edit.before = *layer;
        edit.after = std::nullopt;
        m_document.applyRemovalLayerEdit(edit,
                                         QStringLiteral("AI 删除消除层"));
    }
    endMask();
}

QVariantList RemovalController::keyframesToList(
    const RemovalLayer *layer) const
{
    QVariantList list;
    if (!layer)
        return list;
    for (const MaskKeyframe &keyframe : layer->maskKeyframes) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), keyframe.id);
        entry.insert(QStringLiteral("timeUs"),
                     QVariant::fromValue<qint64>(keyframe.sourceTimeUs));
        entry.insert(QStringLiteral("maskHash"), keyframe.maskHash);
        entry.insert(QStringLiteral("featherPx"), keyframe.featherPx);
        entry.insert(QStringLiteral("revision"), keyframe.revision);
        list.append(entry);
    }
    return list;
}

QVariantList RemovalController::maskKeyframes() const
{
    return keyframesToList(currentLayer());
}

QVariantList RemovalController::maskKeyframesForClip(
    const QString &clipId) const
{
    const Clip *clip = findClip(clipId);
    if (!clip || clip->removalLayerId.isEmpty())
        return {};
    return keyframesToList(
        m_document.removalLayerById(clip->removalLayerId));
}

void RemovalController::updateOverlay()
{
    const QImage image = MaskEditor::toOverlay(m_working.mask);
    ++m_maskRevision;
    if (m_provider)
        m_provider->setMask(image);
    emit maskChanged();
    emit overlayChanged(image, QRectF());
}

void RemovalController::refreshFromDocument()
{
    if (!m_maskMode)
        return;
    // The masked clip can disappear through delete/split/ripple edits. Leaving
    // mask mode on would keep a full-preview click blocker with no panel.
    if (!activeClip()) {
        endMask();
        return;
    }
    if (m_working.layerId.isEmpty())
        return;

    const RemovalLayer *layer = currentLayer();
    m_hasLayer = layer != nullptr;

    if (layer) {
        m_layerEnabled = layer->enabled;
        const MaskKeyframe *exact =
            keyframeAt(*layer, m_working.sourceTimeUs);
        const MaskKeyframe *keyframe =
            exact ? exact : nearestKeyframe(*layer, m_working.sourceTimeUs);
        if (keyframe) {
            m_working.keyframeId = exact ? exact->id : QString();
            const bool featherDidChange = keyframe->featherPx != m_featherPx;
            m_featherPx = keyframe->featherPx;
            if (keyframe->maskHash != m_working.beforeMaskHash) {
                BinaryMask loaded;
                if (loadMask(loaded, keyframe->maskHash)) {
                    m_working.mask = loaded;
                    m_working.beforeMaskHash = keyframe->maskHash;
                }
            }
            if (featherDidChange)
                emit featherChanged();
        } else {
            m_working.keyframeId.clear();
        }
    } else {
        // Undo removed the layer entirely (this happens when the first stroke
        // of a brand-new layer is undone). Restore the mask the editor opened
        // with so currentMaskHash matches the pre-command state again.
        m_working.keyframeId.clear();
        if (m_working.baseMask.isValid())
            m_working.mask = m_working.baseMask;
        m_working.beforeMaskHash = MaskCodec::hash(m_working.mask);
    }

    emit layerChanged();
    updateOverlay();
}

const RemovalLayer *RemovalController::activeRemovalLayer() const
{
    if (const RemovalLayer *layer = currentLayer())
        return layer;
    const Clip *clip = activeClip();
    if (!clip || clip->removalLayerId.isEmpty())
        return nullptr;
    return m_document.removalLayerById(clip->removalLayerId);
}

const Asset *RemovalController::activeAsset() const
{
    const Clip *clip = activeClip();
    return clip ? assetForClip(*clip) : nullptr;
}

bool RemovalController::prepareForJob(QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!m_jobManager)
        return fail(QStringLiteral("AI 任务管理器未初始化。"));
    if (!m_featureGate)
        return fail(QStringLiteral("AI 消除功能未启用：未加载模型清单。"));
    if (!m_featureGate->enabled())
        return fail(m_featureGate->disabledReason());
    if (!m_cache)
        return fail(QStringLiteral("AI 缓存未初始化。"));
    const RemovalLayer *layer = activeRemovalLayer();
    if (!layer)
        return fail(QStringLiteral("请先绘制消除遮罩。"));
    if (layer->maskKeyframes.isEmpty())
        return fail(QStringLiteral("请先添加遮罩关键帧。"));
    const Clip *clip = findClip(layer->clipId);
    if (!clip)
        return fail(QStringLiteral("未找到消除层对应的片段。"));
    const Asset *asset = assetForClip(*clip);
    if (!asset || !asset->info.hasVideo)
        return fail(QStringLiteral("只能对视频片段使用 AI 消除。"));
    if (m_sidecars.sidecarRoot().isEmpty())
        return fail(QStringLiteral("无法确定工程旁车目录。"));
    if (!m_featureGate->registry())
        return fail(QStringLiteral("AI 消除功能未启用：未加载模型清单。"));
    if (layer->sourceOutUs <= layer->sourceInUs)
        return fail(QStringLiteral("消除层的源区间无效。"));
    return true;
}

QString RemovalController::cacheKeyFor(const Clip &clip,
                                       const RemovalLayer &layer,
                                       const ModelRegistry &models) const
{
    return computeRemovalCacheKey(
        RemovalPreflight::keyInputFor(m_document.project(), clip, layer, models));
}

QString RemovalController::manifestPath() const
{
    if (!m_featureGate || !m_featureGate->registry())
        return QString();
    const ModelRegistry *registry = m_featureGate->registry();
    if (!registry->manifestPath().isEmpty())
        return registry->manifestPath();
    return QDir(registry->rootDir())
        .filePath(QStringLiteral("models.manifest.json"));
}

bool RemovalController::buildJobSpec(PendingJobKind kind, AiJobSpec *spec,
                                     QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const RemovalLayer *layer = activeRemovalLayer();
    const Clip *clip = layer ? findClip(layer->clipId) : nullptr;
    const Asset *asset = clip ? assetForClip(*clip) : nullptr;
    const ModelRegistry *registry =
        m_featureGate ? m_featureGate->registry() : nullptr;
    if (!layer || !clip || !asset || !registry)
        return fail(QStringLiteral("AI 消除功能未启用：未加载模型清单。"));

    const AiModelRecord *tracking = registry->defaultTrackingModel();
    if (!tracking)
        return fail(QStringLiteral("没有可用的跟踪模型。"));
    const AiModelRecord *inpaint =
        kind == PendingJobKind::Track
            ? nullptr
            : registry->preferredInpaintModel(layer->parameters.mode);
    if (kind != PendingJobKind::Track && !inpaint)
        return fail(QStringLiteral("没有可用的修复模型。"));

    const QString key = cacheKeyFor(*clip, *layer, *registry);
    if (key.isEmpty())
        return fail(QStringLiteral("无法计算 AI 修复缓存键。"));

    spec->jobId = newId(QStringLiteral("ai-"));
    spec->clipId = clip->id;
    spec->assetPath = asset->path;
    spec->sidecarRoot = m_sidecars.sidecarRoot();
    spec->cacheDir = m_cache ? m_cache->cacheDir(key) : QString();
    spec->cacheKey = key;
    spec->modelsManifestPath = manifestPath();
    spec->trackingModelId = tracking->id;
    spec->inpaintModelId = inpaint ? inpaint->id : QString();
    // Tracking-only runs must not require an inpaint model; their cacheDir
    // still uses the same key so the later repair run reuses the directory.
    spec->trackingOnly = kind == PendingJobKind::Track;
    spec->sourceInUs = layer->sourceInUs;
    spec->sourceOutUs = layer->sourceOutUs;

    const QSize display = displaySizeForAsset(*asset);
    spec->width = display.width() > 0 ? display.width() : 1920;
    spec->height = display.height() > 0 ? display.height() : 1080;

    const Rational assetFps = asset->info.frameRate;
    spec->fps = assetFps.isValid() ? assetFps : m_document.project().timeline.fps;
    if (!spec->fps.isValid())
        spec->fps = Rational{30, 1};

    spec->maskKeyframes = layer->maskKeyframes;
    RemovalParameters parameters = layer->parameters;
    if (parameters.processingWidth <= 0 || parameters.processingWidth > 8192)
        parameters.processingWidth = spec->width;
    if (parameters.processingHeight <= 0 || parameters.processingHeight > 8192)
        parameters.processingHeight = spec->height;
    parameters.stabilizationStrength =
        qBound(0, parameters.stabilizationStrength, 100);
    spec->parameters = parameters;

    if (spec->cacheDir.isEmpty() || spec->modelsManifestPath.isEmpty())
        return fail(QStringLiteral("AI 任务参数无效。"));
    return true;
}

bool RemovalController::startPendingJob(PendingJobKind kind,
                                        const AiJobSpec &spec, QString *error)
{
    if (!m_jobManager) {
        if (error)
            *error = QStringLiteral("AI 任务管理器未初始化。");
        return false;
    }
    m_pendingKind = kind;
    m_pendingCacheKey = spec.cacheKey;
    m_pendingExportAfter = m_exportAfterGeneration;
    QString startError;
    if (!m_jobManager->start(spec, &startError)) {
        m_pendingKind = PendingJobKind::None;
        m_pendingLayerId.clear();
        m_pendingExportAfter = false;
        if (error)
            *error = startError.isEmpty()
                         ? QStringLiteral("AI 任务参数无效。")
                         : startError;
        return false;
    }
    m_generating = true;
    emit generatingChanged();
    emit pausedChanged();
    emit progressChanged();
    emit stageTextChanged();
    emit etaTextChanged();
    emit cacheStatusChanged();
    return true;
}

void RemovalController::handleJobFinished(const AiJobResult &result)
{
    const PendingJobKind kind = m_pendingKind;
    const QString layerId = m_pendingLayerId;
    const bool exportAfter = m_pendingExportAfter;
    const bool wasTracking = kind == PendingJobKind::Track;
    m_pendingKind = PendingJobKind::None;
    m_pendingLayerId.clear();
    m_pendingCacheKey.clear();
    m_pendingExportAfter = false;
    m_generating = false;
    m_exportAfterGeneration = false;
    setExportGenerationPending(false);
    emit generatingChanged();
    emit pausedChanged();
    emit progressChanged();
    emit stageTextChanged();

    RemovalLayer *layer = layerId.isEmpty()
                              ? nullptr
                              : m_document.removalLayerById(layerId);
    if (!layer) {
        notifyAnalysisChanged();
        return;
    }

    RemovalLayer after = *layer;
    // Tracking-only jobs also own the same content-addressed directory, so the
    // single completion command records the key/state for every outcome.
    if (!result.cacheKey.isEmpty()) {
        after.cacheKey = result.cacheKey;
        after.cachePath = QStringLiteral("removal/") + result.cacheKey;
    }
    const bool confidenceChanged =
        !result.confidenceHash.isEmpty()
        && result.confidenceHash != after.trackingConfidenceHash;
    if (confidenceChanged)
        after.trackingConfidenceHash = result.confidenceHash;

    if (result.ok) {
        RemovalTrackingState state = result.trackingState;
        if (wasTracking) {
            if (state == RemovalTrackingState::NotTracked)
                state = RemovalTrackingState::Complete;
        } else if (m_cache && !after.cacheKey.isEmpty()) {
            const RemovalCacheState cacheState = m_cache->state(after.cacheKey);
            if (cacheState == RemovalCacheState::Complete) {
                // Low confidence is actionable information even after a
                // successful repair; keep it instead of flattening to Complete.
                if (state != RemovalTrackingState::LowConfidence)
                    state = RemovalTrackingState::Complete;
            } else if (cacheState == RemovalCacheState::Partial) {
                state = RemovalTrackingState::Partial;
            } else if (state == RemovalTrackingState::NotTracked) {
                state = RemovalTrackingState::Failed;
            }
        }
        after.trackingState = state;
        after.revision += 1;
        applyLayerUpdate(after, wasTracking ? QStringLiteral("AI 跟踪完成")
                                            : QStringLiteral("AI 修复完成"));
        notifyAnalysisChanged();
        if (exportAfter && !wasTracking)
            emit generationFinishedForExport();
        return;
    }

    if (result.cancelled) {
        after.trackingState =
            result.trackingState == RemovalTrackingState::NotTracked
                ? RemovalTrackingState::Partial
                : result.trackingState;
        after.revision += 1;
        applyLayerUpdate(after, QStringLiteral("AI 任务取消"));
        setError(removalErrorMessage(QStringLiteral("cancelled"),
                                     result.errorMessage));
    } else {
        after.trackingState = RemovalTrackingState::Failed;
        after.revision += 1;
        applyLayerUpdate(after, QStringLiteral("AI 任务失败"));
        setError(removalErrorMessage(result.errorCode, result.errorMessage));
    }
    notifyAnalysisChanged();
}

void RemovalController::applyLayerUpdate(const RemovalLayer &after,
                                         const QString &text)
{
    const RemovalLayer *before = m_document.removalLayerById(after.id);
    RemovalLayerEdit edit;
    edit.clipId = after.clipId;
    if (before)
        edit.before = *before;
    edit.after = after;
    m_document.applyRemovalLayerEdit(edit, text);
    m_hasLayer = true;
    m_layerEnabled = after.enabled;
    emit layerChanged();
    notifyAnalysisChanged();
}

bool RemovalController::updateParameters(
    const std::function<void(RemovalParameters &)> &mutate, const QString &text)
{
    const RemovalLayer *layer = activeRemovalLayer();
    if (!layer)
        return false;
    RemovalLayer after = *layer;
    mutate(after.parameters);
    // The cache key includes every parameter, so any change invalidates the
    // previously generated repair. Keep the tracking sidecar (its hash only
    // depends on the mask and tracking model) but drop the stale key.
    if (after.parameters == layer->parameters)
        return false;
    after.revision += 1;
    after.cacheKey.clear();
    after.cachePath.clear();
    applyLayerUpdate(after, text);
    return true;
}

QString RemovalController::mapStageText(const QString &stage) const
{
    const QString value = stage.trimmed();
    if (value.compare(QStringLiteral("tracking"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("正在跟踪遮罩");
    if (value.compare(QStringLiteral("inpainting"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("repair"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("正在修复画面");
    if (value.compare(QStringLiteral("writing"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("正在写入缓存");
    return stage;
}

QString RemovalController::cacheStatusFor(const RemovalLayer *layer) const
{
    if (!layer)
        return QString();
    if (layer->cacheKey.isEmpty())
        return QStringLiteral("缺失");
    if (!m_cache)
        return QStringLiteral("缺失");
    switch (m_cache->state(layer->cacheKey)) {
    case RemovalCacheState::Complete:
        return QStringLiteral("已生成");
    case RemovalCacheState::Partial:
        return QStringLiteral("部分可继续");
    case RemovalCacheState::Corrupt:
        return QStringLiteral("已过期");
    case RemovalCacheState::Missing:
        break;
    }
    return QStringLiteral("缺失");
}

QString RemovalController::cacheStatusText() const
{
    return cacheStatusFor(activeRemovalLayer());
}

bool RemovalController::canContinueGeneration() const
{
    return cacheStatusText() == QStringLiteral("部分可继续");
}

QVariantList RemovalController::confidenceBand() const
{
    QVariantList band;
    const RemovalLayer *layer = activeRemovalLayer();
    if (!layer || layer->trackingConfidenceHash.isEmpty())
        return band;
    const auto tracking =
        m_sidecars.readTracking(layer->trackingConfidenceHash, nullptr);
    if (!tracking)
        return band;

    const Clip *clip = findClip(layer->clipId);
    const QJsonArray frames =
        tracking->value(QStringLiteral("frames")).toArray();
    band.reserve(frames.size());
    for (const QJsonValue &value : frames) {
        const QJsonObject frame = value.toObject();
        const qint64 sourceUs =
            qint64(frame.value(QStringLiteral("timeUs")).toDouble());
        qint64 timelineUs = sourceUs;
        if (clip)
            timelineUs = clip->timelineStartUs
                         + unscaleBySpeed(sourceUs - clip->sourceInUs,
                                          clip->speedMilli);
        const QString state =
            frame.value(QStringLiteral("state")).toString();
        QVariantMap entry;
        entry.insert(QStringLiteral("timeUs"),
                     QVariant::fromValue<qint64>(sourceUs));
        entry.insert(QStringLiteral("timelineUs"),
                     QVariant::fromValue<qint64>(timelineUs));
        entry.insert(QStringLiteral("state"), state);
        entry.insert(QStringLiteral("color"), confidenceColorFor(state));
        band.append(entry);
    }
    return band;
}

int RemovalController::lowConfidenceCount() const
{
    int count = 0;
    for (const QVariant &entry : confidenceBand()) {
        if (isLowConfidenceState(
                entry.toMap().value(QStringLiteral("state")).toString()))
            ++count;
    }
    return count;
}

void RemovalController::jumpToConfidence(int index)
{
    const QVariantList band = confidenceBand();
    if (index < 0 || index >= band.size())
        return;
    const QVariantMap entry = band.at(index).toMap();
    const qint64 timelineUs =
        entry.value(QStringLiteral("timelineUs")).toLongLong();
    if (m_seekCallback)
        m_seekCallback(timelineUs);
}

void RemovalController::notifyAnalysisChanged()
{
    emit confidenceChanged();
    emit cacheStatusChanged();
}

bool RemovalController::computeRedoFrames(const RemovalLayer &layer,
                                          qint64 sourceStartUs,
                                          qint64 sourceEndUs, int *firstFrame,
                                          int *lastFrame)
{
    const Clip *clip = findClip(layer.clipId);
    const Asset *asset = clip ? assetForClip(*clip) : nullptr;
    if (!clip || !asset) {
        setError(QStringLiteral("未找到消除层对应的片段。"));
        return false;
    }
    const qint64 start = qBound(layer.sourceInUs, sourceStartUs,
                                layer.sourceOutUs);
    const qint64 end = qBound(layer.sourceInUs, sourceEndUs, layer.sourceOutUs);
    if (end <= start) {
        setError(QStringLiteral("重做范围为空。"));
        return false;
    }

    Rational fps = asset->info.frameRate;
    if (!fps.isValid())
        fps = m_document.project().timeline.fps;
    if (!fps.isValid())
        fps = Rational{30, 1};

    const auto frameAt = [&](qint64 us, bool roundUp) {
        const qint64 delta = us - layer.sourceInUs;
        const qint64 scaled = delta * fps.num;
        const qint64 divisor = 1'000'000LL * fps.den;
        if (roundUp)
            return int((scaled + divisor - 1) / divisor);
        return int(scaled / divisor);
    };

    const int totalFrames = qMax(
        1, frameAt(layer.sourceOutUs, true));
    const int first = qBound(0, frameAt(start, false), totalFrames - 1);
    const int last = qBound(first, frameAt(end, true) - 1, totalFrames - 1);
    const int chunkCount = (totalFrames + 29) / 30;
    const int firstChunk = qMax(0, first / 30 - 1);
    const int lastChunk = qMin(chunkCount - 1, last / 30 + 1);
    *firstFrame = firstChunk * 30;
    *lastFrame = (lastChunk + 1) * 30 - 1;
    return true;
}

} // namespace VideoEditor
