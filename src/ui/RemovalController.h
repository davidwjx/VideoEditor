#pragma once

#include <cstdint>
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
class ModelRegistry;
class RemovalCache;
struct AiJobResult;
struct AiJobSpec;

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
    // Task 8: feature gate, job progress and analysis state.
    Q_PROPERTY(bool featureEnabled READ featureEnabled
                   NOTIFY featureEnabledChanged)
    Q_PROPERTY(QString featureDisabledReason READ featureDisabledReason
                   NOTIFY featureEnabledChanged)
    Q_PROPERTY(bool technicalPreview READ technicalPreview
                   NOTIFY featureEnabledChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stageText READ stageText NOTIFY stageTextChanged)
    Q_PROPERTY(QString etaText READ etaText NOTIFY etaTextChanged)
    Q_PROPERTY(QVariantList confidenceBand READ confidenceBand
                   NOTIFY confidenceChanged)
    Q_PROPERTY(int lowConfidenceCount READ lowConfidenceCount
                   NOTIFY confidenceChanged)
    Q_PROPERTY(QString cacheStatusText READ cacheStatusText
                   NOTIFY cacheStatusChanged)
    Q_PROPERTY(bool canContinueGeneration READ canContinueGeneration
                   NOTIFY cacheStatusChanged)
    Q_PROPERTY(bool advancedOpen READ advancedOpen WRITE setAdvancedOpen
                   NOTIFY advancedOpenChanged)
    Q_PROPERTY(QString advancedMode READ advancedMode NOTIFY layerChanged)
    Q_PROPERTY(int processingHeight READ processingHeight NOTIFY layerChanged)
    Q_PROPERTY(int stabilizationStrength READ stabilizationStrength
                   NOTIFY layerChanged)
    Q_PROPERTY(QString compareMode READ compareMode
                   NOTIFY compareModeChanged)
    Q_PROPERTY(int splitPermille READ splitPermille
                   NOTIFY splitPermilleChanged)
    // True from generateForExport() until the job succeeds/fails or the user
    // cancels the export-driven generation.
    Q_PROPERTY(bool exportGenerationPending READ exportGenerationPending
                   NOTIFY exportGenerationPendingChanged)

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
    bool hasLayer() const { return activeRemovalLayer() != nullptr; }
    bool layerEnabled() const { return m_layerEnabled; }
    QString error() const { return m_error; }
    bool boxMode() const { return m_boxMode; }
    void setBoxMode(bool enabled);
    bool eraseMode() const { return m_eraseMode; }
    void setEraseMode(bool enabled);

    bool featureEnabled() const;
    QString featureDisabledReason() const;
    bool technicalPreview() const;
    bool generating() const { return m_generating; }
    bool paused() const;
    double progress() const;
    QString stageText() const;
    QString etaText() const;
    QVariantList confidenceBand() const;
    int lowConfidenceCount() const;
    QString cacheStatusText() const;
    bool canContinueGeneration() const;
    bool advancedOpen() const { return m_advancedOpen; }
    void setAdvancedOpen(bool open);
    QString advancedMode() const;
    int processingHeight() const;
    int stabilizationStrength() const;
    QString compareMode() const { return m_compareMode; }
    int splitPermille() const { return m_splitPermille; }
    bool exportGenerationPending() const { return m_exportGenerationPending; }

    // Injected by AppController: Preview.positionMs * 1000 in timeline mode,
    // with the timeline playhead as the authoritative fallback. Tests inject a
    // fixed value so they do not depend on QML.
    void setPlayheadProvider(std::function<qint64()> provider);
    // Maps a timeline position and pauses playback. Injected by AppController;
    // confidence-band clicks use it.
    void setSeekCallback(std::function<void(qint64)> callback);
    // Selected clip id provider so export-driven generation and the confidence
    // strip work without entering mask mode first.
    void setSelectedClipProvider(std::function<QString()> provider);
    // Forwards compare mode/split to the preview renderer. AppController wires
    // this to PreviewController::setRemovalCompareMode; tests capture it.
    void setCompareSink(std::function<void(const QString &, int)> sink);
    // Not owned: QQmlEngine::addImageProvider takes ownership.
    void setMaskImageProvider(MaskImageProvider *provider);
    MaskImageProvider *maskImageProvider() const { return m_provider; }

    // Injected by AppController; not owned. Until the job manager is wired the
    // generate/track entry points report a Chinese error but mask editing keeps
    // working.
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
    // Re-runs tracking from the keyframe at/just before the playhead to the end
    // of the layer, chunk-padded like redoRange().
    Q_INVOKABLE void repropagateFromCurrentKeyframe();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void jumpToConfidence(int index);
    Q_INVOKABLE void redoRange(qint64 sourceStartUs, qint64 sourceEndUs);
    Q_INVOKABLE void setCompareMode(const QString &mode);
    Q_INVOKABLE void setSplitPermille(int permille);
    Q_INVOKABLE void setAdvancedMode(const QString &mode);
    Q_INVOKABLE void setStabilizationStrength(int strength);
    Q_INVOKABLE void setProcessingHeight(int height);
    // Starts generation with the export-after flag set. Emits
    // generationFinishedForExport() only when generation succeeds.
    Q_INVOKABLE void generateForExport();
    // Receives ExportController::removalGenerationRequired so the panel can
    // explain a blocked export even before the user opens the prompt.
    Q_INVOKABLE void setExportRequiredMessage(const QString &message);
    // Cancels the export-driven generation and disarms the post-success
    // export start (used when the export dialog closes mid-generation).
    Q_INVOKABLE void cancelExportGeneration();
    // Re-emits analysis-derived signals when the timeline selection changes.
    void notifySelectionChanged();

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
    void featureEnabledChanged();
    void generatingChanged();
    void pausedChanged();
    void progressChanged();
    void stageTextChanged();
    void etaTextChanged();
    void confidenceChanged();
    void cacheStatusChanged();
    void advancedOpenChanged();
    void compareModeChanged();
    void splitPermilleChanged();
    void exportGenerationPendingChanged();
    // Emitted after a generation started via generateForExport() succeeds.
    void generationFinishedForExport();

private:
    enum class PendingJobKind { None, Track, Generate, Redo };

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
    // Layer under the active clip, falling back to the clip's own removal layer
    // when mask mode has not loaded it yet.
    const RemovalLayer *activeRemovalLayer() const;
    const Asset *activeAsset() const;
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

    // Task 8 helpers.
    bool prepareForJob(QString *error) const;
    bool buildJobSpec(PendingJobKind kind, AiJobSpec *spec, QString *error);
    QString cacheKeyFor(const Clip &clip, const RemovalLayer &layer,
                        const ModelRegistry &models) const;
    QString manifestPath() const;
    bool startPendingJob(PendingJobKind kind, const AiJobSpec &spec,
                         QString *error);
    void handleJobFinished(const AiJobResult &result);
    void applyLayerUpdate(const RemovalLayer &after, const QString &text);
    bool updateParameters(const std::function<void(RemovalParameters &)> &mutate,
                          const QString &text);
    void applyCompareMode();
    void setExportGenerationPending(bool pending);
    QString mapStageText(const QString &stage) const;
    QString cacheStatusFor(const RemovalLayer *layer) const;
    void notifyAnalysisChanged();
    // Chunk-aligned ±1 chunk invalidation range for a source-time span.
    bool computeRedoFrames(const RemovalLayer &layer, qint64 sourceStartUs,
                           qint64 sourceEndUs, int *firstFrame,
                           int *lastFrame);

    ProjectDocument &m_document;
    MaskSidecarStore &m_sidecars;
    AiJobManager *m_jobManager = nullptr;
    RemovalCache *m_cache = nullptr;
    AiFeatureGate *m_featureGate = nullptr;
    MaskImageProvider *m_provider = nullptr;
    std::function<qint64()> m_playheadProvider;
    std::function<void(qint64)> m_seekCallback;
    std::function<QString()> m_selectedClipProvider;
    std::function<void(const QString &, int)> m_compareSink;
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

    PendingJobKind m_pendingKind = PendingJobKind::None;
    QString m_pendingLayerId;
    QString m_pendingCacheKey;
    bool m_pendingExportAfter = false;
    bool m_generating = false;
    bool m_exportAfterGeneration = false;
    bool m_exportGenerationPending = false;
    bool m_advancedOpen = false;
    QString m_compareMode = QStringLiteral("repair");
    int m_splitPermille = 500;
};

} // namespace VideoEditor
