#pragma once

#include <functional>

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVariantList>

#include "MaskCodec.h"
#include "MaskSidecarStore.h"
#include "Project.h"
#include "ProjectDocument.h"
#include "Removal.h"

namespace VideoEditor {

class AiFeatureGate;
class AiJobManager;
class MaskImageProvider;
class RemovalCache;

// Owns the transient mask-editor state for one clip: which keyframe is being
// authored, the in-progress binary mask and the command that commits it. The
// document remains the source of truth; every signal emitted while mask mode
// is active is re-derived from ProjectDocument::changed, so undo/redo update
// the preview overlay without the controller keeping a stale layer copy.
class RemovalController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool maskMode READ maskMode NOTIFY maskModeChanged)
    Q_PROPERTY(QString activeClipId READ activeClipId NOTIFY activeClipChanged)
    Q_PROPERTY(int brushSize READ brushSize WRITE setBrushSize
                   NOTIFY brushSizeChanged)
    Q_PROPERTY(int featherPx READ featherPx NOTIFY featherChanged)
    Q_PROPERTY(QString currentMaskHash READ currentMaskHash NOTIFY maskChanged)
    Q_PROPERTY(int maskRevision READ maskRevision NOTIFY maskChanged)
    Q_PROPERTY(QVariantList maskKeyframes READ maskKeyframes NOTIFY layerChanged)
    Q_PROPERTY(bool hasLayer READ hasLayer NOTIFY layerChanged)
    Q_PROPERTY(bool layerEnabled READ layerEnabled NOTIFY layerChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // Editor tool selection shared by RemovalPanel and MaskEditorOverlay.
    Q_PROPERTY(bool boxMode READ boxMode WRITE setBoxMode NOTIFY toolChanged)
    Q_PROPERTY(bool eraseMode READ eraseMode WRITE setEraseMode
                   NOTIFY toolChanged)

public:
    explicit RemovalController(ProjectDocument &document,
                               MaskSidecarStore &sidecars,
                               QObject *parent = nullptr);

    bool maskMode() const { return m_maskMode; }
    QString activeClipId() const { return m_activeClipId; }
    int brushSize() const { return m_brushSize; }
    void setBrushSize(int px);
    int featherPx() const { return m_featherPx; }
    QString currentMaskHash() const;
    int maskRevision() const { return m_maskRevision; }
    QVariantList maskKeyframes() const;
    bool hasLayer() const { return m_hasLayer; }
    bool layerEnabled() const { return m_layerEnabled; }
    QString error() const { return m_error; }
    bool boxMode() const { return m_boxMode; }
    void setBoxMode(bool enabled);
    bool eraseMode() const { return m_eraseMode; }
    void setEraseMode(bool enabled);

    // Injected by AppController: Preview.positionMs * 1000 in timeline mode,
    // with the timeline playhead as the authoritative fallback. Tests inject a
    // fixed value so they do not depend on QML.
    void setPlayheadProvider(std::function<qint64()> provider);
    // Not owned: QQmlEngine::addImageProvider takes ownership.
    void setMaskImageProvider(MaskImageProvider *provider);
    MaskImageProvider *maskImageProvider() const { return m_provider; }

    // Injected by AppController; not owned. Until the job manager is wired the
    // generate/track entry points report a Chinese error but mask editing keeps
    // working. Task 8 replaces the placeholders with the real flow.
    void setJobManager(AiJobManager *manager);
    void setCache(RemovalCache *cache);
    void setFeatureGate(AiFeatureGate *gate);
    AiJobManager *jobManager() const { return m_jobManager; }

    Q_INVOKABLE void beginMask(const QString &clipId);
    Q_INVOKABLE void endMask();
    Q_INVOKABLE void beginStroke(int radiusSourcePx, bool erase);
    Q_INVOKABLE void continueStroke(QPointF fromNorm, QPointF toNorm);
    Q_INVOKABLE void endStroke();
    Q_INVOKABLE void paintBox(QRectF normalized, bool erase);
    Q_INVOKABLE void setFeatherPx(int px);
    Q_INVOKABLE void addMaskKeyframeAtPlayhead();
    Q_INVOKABLE void removeMaskKeyframe(int index);
    Q_INVOKABLE void setLayerEnabled(bool enabled);
    Q_INVOKABLE void deleteLayer();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool track();
    Q_INVOKABLE bool generate();

    // Test/timeline-delegate entry points. sourceTimeUs is relative to the
    // clip's source media, not the timeline.
    Q_INVOKABLE void addMaskKeyframeAtSourceTime(qint64 sourceTimeUs,
                                                 const QString &id = {});
    Q_INVOKABLE QVariantList maskKeyframesForClip(const QString &clipId) const;

signals:
    void maskModeChanged();
    void activeClipChanged();
    void brushSizeChanged();
    void toolChanged();
    void featherChanged();
    void maskChanged();
    void layerChanged();
    void errorChanged();
    // Connects to MaskImageProvider when one is installed.
    void overlayChanged(QImage image, QRectF contentRect);

private:
    struct WorkingState {
        QString clipId;
        QString layerId;
        QString keyframeId;
        qint64 sourceTimeUs = 0;
        BinaryMask mask;
        BinaryMask baseMask;
        QString beforeMaskHash;
        bool strokeActive = false;
        bool erase = false;
        int radius = 1;
    };

    const Clip *findClip(const QString &clipId) const;
    const Clip *activeClip() const;
    const Asset *assetForClip(const Clip &clip) const;
    const RemovalLayer *currentLayer() const;
    qint64 playheadUs() const;
    QSize displaySizeForClip(const Clip &clip) const;
    BinaryMask emptyMaskForClip(const Clip &clip) const;
    static const MaskKeyframe *keyframeAt(const RemovalLayer &layer,
                                          qint64 sourceTimeUs);
    static const MaskKeyframe *nearestKeyframe(const RemovalLayer &layer,
                                               qint64 sourceTimeUs);
    static int keyframeIndexAt(const RemovalLayer &layer, qint64 sourceTimeUs);
    bool loadMask(BinaryMask &target, const QString &maskHash);
    bool prepareMaskEditAtPlayhead();
    bool commitMaskChange(const QString &text);
    void updateOverlay();
    void refreshFromDocument();
    void resetWorkingState();
    void setError(const QString &message);
    QVariantList keyframesToList(const RemovalLayer *layer) const;

    ProjectDocument &m_document;
    MaskSidecarStore &m_sidecars;
    AiJobManager *m_jobManager = nullptr;
    RemovalCache *m_cache = nullptr;
    AiFeatureGate *m_featureGate = nullptr;
    MaskImageProvider *m_provider = nullptr;
    std::function<qint64()> m_playheadProvider;
    WorkingState m_working;
    bool m_maskMode = false;
    QString m_activeClipId;
    int m_brushSize = 24;
    int m_featherPx = 4;
    int m_maskRevision = 0;
    bool m_boxMode = false;
    bool m_eraseMode = false;
    bool m_hasLayer = false;
    bool m_layerEnabled = true;
    QString m_error;
};

} // namespace VideoEditor
