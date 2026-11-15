#include "RemovalController.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <QPoint>
#include <QRect>
#include <QUuid>
#include <QVariantMap>
#include <QtGlobal>

#include "AiFeatureGate.h"
#include "AiJobManager.h"
#include "MaskEditor.h"
#include "MaskImageProvider.h"
#include "RemovalCache.h"

namespace VideoEditor {

namespace {

QString newId(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
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
}

void RemovalController::setPlayheadProvider(std::function<qint64()> provider)
{
    m_playheadProvider = std::move(provider);
}

void RemovalController::setMaskImageProvider(MaskImageProvider *provider)
{
    m_provider = provider;
    if (m_provider && m_working.mask.isValid())
        m_provider->setMask(MaskEditor::toOverlay(m_working.mask));
}

void RemovalController::setJobManager(AiJobManager *manager)
{
    m_jobManager = manager;
}

void RemovalController::setCache(RemovalCache *cache)
{
    m_cache = cache;
}

void RemovalController::setFeatureGate(AiFeatureGate *gate)
{
    m_featureGate = gate;
}

bool RemovalController::track()
{
    if (!m_jobManager) {
        setError(QStringLiteral("AI 任务管理器未初始化。"));
        return false;
    }
    // Task 8 builds the tracking AiJobSpec and listens for jobFinished.
    setError(QStringLiteral("AI 消除任务尚未启用。"));
    return false;
}

bool RemovalController::generate()
{
    if (!m_jobManager) {
        setError(QStringLiteral("AI 任务管理器未初始化。"));
        return false;
    }
    // Task 8 builds the inpaint AiJobSpec and listens for jobFinished.
    setError(QStringLiteral("AI 消除任务尚未启用。"));
    return false;
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
    return m_activeClipId.isEmpty() ? nullptr : findClip(m_activeClipId);
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

} // namespace VideoEditor
