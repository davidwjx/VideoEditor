#include "AppController.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <utility>

#include <objbase.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

#include "AiFeatureGate.h"
#include "AiJobManager.h"
#include "AudioEngine.h"
#include "CodecSupport.h"
#include "ExportController.h"
#include "ExportEncoderCatalog.h"
#include "ExportPipeline.h"
#include "FrameReader.h"
#include "ImageMetrics.h"
#include "LiveRepairService.h"
#include "MaskCodec.h"
#include "MaskSidecarStore.h"
#include "MediaListModel.h"
#include "MediaProbe.h"
#include "ModelRegistry.h"
#include "PreviewController.h"
#include "ProxyPlanner.h"
#include "ProxyService.h"
#include "RemovalCache.h"
#include "RemovalCacheKey.h"
#include "RemovalPreflight.h"
#include "RepairFrameSource.h"
#include "TimelineController.h"
#include "TimelineEvaluator.h"
#include "WaveformService.h"

#ifndef VIDEOEDITOR_FIXTURE_DIR
#define VIDEOEDITOR_FIXTURE_DIR "tests/fixtures"
#endif

namespace VideoEditor {

namespace {

// Proxy generation runs the FFmpeg CLI. Prefer the executable shipped next to
// the app, then the documented override, then whatever is on PATH.
QString resolveFfmpegPath()
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("ffmpeg.exe"));
    if (QFileInfo::exists(bundled))
        return bundled;
    const QString configured = qEnvironmentVariable("VIDEOEDITOR_FFMPEG");
    if (!configured.isEmpty())
        return configured;
    return QStringLiteral("ffmpeg");
}

} // namespace

AppController::AppController(QObject *parent, QString recoveryPath,
                             AudioEngine::SinkFactory audioSinkFactory,
                             QString sidecarFallback)
    : QObject(parent)
    , m_document(new ProjectDocument(this))
    , m_mediaModel(new MediaListModel(this))
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (recoveryPath.isEmpty()) {
        recoveryPath = QDir(appData).filePath(
            QStringLiteral("recovery/project.autosave.json"));
    }
    m_store = std::make_unique<ProjectStore>(recoveryPath);

    connect(m_document, &ProjectDocument::assetsChanged,
            this, &AppController::refreshModel);
    connect(m_document, &ProjectDocument::undoStateChanged,
            this, &AppController::undoStateChanged);
    connect(m_document, &ProjectDocument::dirtyChanged,
            this, &AppController::projectChanged);

    m_autosaveTimer.setInterval(30'000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &AppController::autosave);
    m_autosaveTimer.start();

    refreshModel();
    // Prompt only once, at startup, for a recovery file left by a prior session.
    // Autosaving during this session must not raise the prompt.
    m_recoveryAvailable = QFileInfo::exists(m_store->recoveryFilePath());

    m_timeline = new TimelineController(*m_document, this);
    connect(m_timeline, &TimelineController::editFailed, this,
            &AppController::setLastError);

    // The export controller reads the document lazily; only an export freezes
    // it. Keep one instance for the lifetime of the window so QML bindings and
    // in-flight exports share the same state.
    m_export = std::make_unique<ExportController>(*m_document, QString());

    m_preview = new PreviewController(this);
    connect(m_preview, &PreviewController::framePresented, this,
            [this](qint64) { emit selftestFramePresented(); });
    connect(m_preview, &PreviewController::timelineFramePresented, this,
            [this](qint64) {
                emit timelineFramePresented();
                if (m_perfClock.isValid())
                    ++m_perfFrames;
            });
    connect(m_preview, &PreviewController::timelineFrameLayersPresented, this,
            [this](int layerCount) {
                emit timelineFrameLayersPresented(layerCount);
                if (!m_perfClock.isValid())
                    return;
                ++m_perfLayerSamples;
                if (layerCount >= m_perfExpectedLayers)
                    ++m_perfFullLayerFrames;
                if (m_perfLayerSamples == 1 || layerCount < m_perfMinLayers)
                    m_perfMinLayers = layerCount;
            });
    connect(m_preview, &PreviewController::error, this,
            &AppController::setLastError);
    // The preview clock is the timeline playhead while playing. seekUs() only
    // updates the controller position, so this never touches the model.
    connect(m_preview, &PreviewController::positionChanged, this, [this] {
        if (!m_preview->timelineMode() || m_syncingPlayhead)
            return;
        m_syncingPlayhead = true;
        m_timeline->seekUs(m_preview->positionMs() * 1000);
        m_syncingPlayhead = false;
    });
    // The live worker repairs the source frame under the playhead first. m_removal
    // is created later but this signal never fires during construction.
    connect(m_preview, &PreviewController::positionChanged, this, [this] {
        if (!m_liveRepair || !m_removal || !m_removal->livePreview()
            || !m_liveRepair->isRunning()) {
            return;
        }
        m_liveRepair->setTarget(
            m_removal->liveSourceTimeUs(m_preview->positionMs() * 1000));
    });

    // Mask sidecars live next to the .veproj. Unsaved projects use a stable
    // fallback so an in-progress mask edit survives until the first save.
    // MaskSidecarStore appends "removal-sidecars/unsaved" to this base, which
    // yields <AppDataLocation>/removal-sidecars/unsaved.
    m_sidecarFallback =
        sidecarFallback.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : sidecarFallback;
    m_maskSidecars = std::make_unique<MaskSidecarStore>();
    m_maskSidecars->setProjectPath(m_document->filePath(), m_sidecarFallback);

    // The AI feature is fail-closed: loading the manifest is not enough, it
    // must carry commercial approval, legal sign-off and verified files.
    m_aiGate = std::make_unique<AiFeatureGate>();
    // Tests and internal technical runs point VIDEOEDITOR_M4_MODELS at an
    // installed model directory; the historical suffix remains supported.
    QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (modelsDir.isEmpty())
        modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS_DIR");
    if (modelsDir.isEmpty()) {
        modelsDir = QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("models"));
    }
    QString modelsError;
    m_aiGate->reload(modelsDir, &modelsError);
    // Approved release builds stop here. The internal technical preview is an
    // explicit, environment-gated escape hatch that never flips the manifest's
    // commercialApproved/releaseBlocked flags.
    if (!m_aiGate->enabled()
        && qEnvironmentVariable("VIDEOEDITOR_M4_TECHNICAL")
               == QLatin1String("1")) {
        QString previewError;
        m_aiGate->enableTechnicalPreview(&previewError);
    }
    m_aiJobs = std::make_unique<AiJobManager>(*m_aiGate);
    m_removalCache = std::make_unique<RemovalCache>(
        QDir(appData).filePath(QStringLiteral("cache/removal")));

    m_removal = std::make_unique<RemovalController>(*m_document,
                                                     *m_maskSidecars);
    m_removal->setJobManager(m_aiJobs.get());
    m_removal->setCache(m_removalCache.get());
    m_removal->setFeatureGate(m_aiGate.get());

    // Export preflight is injected: ExportController must not know about the
    // cache or the model registry. A failed manifest load leaves registry()
    // null; report the gate reason instead of dereferencing it.
    m_export->setRemovalChecker([this](const Project &project) {
        const ModelRegistry *registry =
            m_aiGate ? m_aiGate->registry() : nullptr;
        if (!registry) {
            const QString reason =
                m_aiGate
                    ? m_aiGate->disabledReason()
                    : QStringLiteral("AI 消除功能未启用：未加载模型清单。");
            return RemovalPreflight::checkWithoutModels(project, reason);
        }
        // AiFeatureGate::reload() only enables the feature after hashing the
        // model files, so a loaded enabled gate does not need the preflight to
        // hash them again on every export-dialog edit.
        return RemovalPreflight::check(
            project, *m_removalCache, *registry,
            m_aiGate && m_aiGate->enabled(),
            m_aiGate && m_aiGate->technicalPreview());
    });
    // The preflight result depends on the document; a cache hit must not
    // survive an edit. `changed` covers every command (including removal layer
    // and clip/track edits); projectReplaced covers load/recovery.
    connect(m_document, &ProjectDocument::changed, m_export.get(),
            &ExportController::invalidateRemovalPreflight);
    connect(m_document, &ProjectDocument::projectReplaced, m_export.get(),
            &ExportController::invalidateRemovalPreflight);
    m_export->setRemovalCacheRoot(m_removalCache->root());
    m_preview->setRemovalCacheRoot(m_removalCache->root());
    // Export preflight failure is surfaced by the dialog, but the message is
    // still forwarded so the removal panel can explain what is missing.
    connect(m_export.get(), &ExportController::removalGenerationRequired,
            m_removal.get(), &RemovalController::setExportRequiredMessage);
    // Completed "generate, then export" runs resume the export the user asked
    // for. The flag is only emitted on success, so a failed run keeps the
    // dialog open with the mapped error.
    connect(m_removal.get(), &RemovalController::generationFinishedForExport,
            m_export.get(), &ExportController::startExport);
    // Timeline position is authoritative for mask authoring; the preview clock
    // mirrors it in timeline mode. The fallback keeps the controller useful
    // when only the timeline controller exists (asset-mode preview).
    m_removal->setPlayheadProvider([this]() -> qint64 {
        if (m_preview && m_preview->timelineMode())
            return m_preview->positionMs() * 1000;
        return m_timeline ? m_timeline->positionUs() : 0;
    });
    // Confidence-band clicks jump to a timeline position and pause playback.
    m_removal->setSeekCallback([this](qint64 timelineUs) {
        if (m_preview)
            m_preview->pause();
        seekTimeline(qMax<qint64>(0, timelineUs));
    });
    // The export prompt can generate without mask mode; target the selection.
    m_removal->setSelectedClipProvider([this] {
        return m_timeline ? m_timeline->selectedClipId() : QString();
    });
    // Compare/split must reach the renderer; the preview method invalidates and
    // re-requests the current frame.
    m_removal->setCompareSink([this](const QString &mode, int splitPermille) {
        if (m_preview)
            m_preview->setRemovalCompareMode(mode, splitPermille);
    });

    // Live preview: the controller resolves the layer/spec, this object owns
    // the worker and forwards the provider to the preview compositor only.
    m_liveRepair = std::make_shared<LiveRepairService>();
    m_liveRefreshTimer.setSingleShot(true);
    m_liveRefreshTimer.setInterval(50);
    connect(&m_liveRefreshTimer, &QTimer::timeout, this, [this] {
        if (m_removal && m_removal->livePreview())
            m_preview->refreshTimelineFrame();
    });
    connect(m_liveRepair.get(), &LiveRepairService::error, this,
            [this](const QString &message) {
                // A missing mask (or any repair failure) stops the worker; the
                // panel shows the Chinese error instead of a silent original.
                if (m_liveRepair)
                    m_liveRepair->stop();
                if (m_preview)
                    m_preview->setLiveRepairProvider(LiveRepairProviderPtr{});
                if (m_removal)
                    m_removal->handleLiveRepairError(message);
                setLastError(message);
            });
    connect(m_liveRepair.get(), &LiveRepairService::frameReady, this,
            [this](int) {
                if (!m_removal || !m_removal->livePreview())
                    return;
                m_removal->notifyLivePreviewReady();
                // A paused preview only re-composes when asked; coalesce the
                // burst of prefetch completions into one request.
                if (!m_preview->playing())
                    m_liveRefreshTimer.start();
            });
    m_removal->setLivePreviewStarter(
        [this](const LiveRepairSpec &spec, QString *error) {
            if (!m_liveRepair)
                return false;
            if (!m_liveRepair->configure(spec, error))
                return false;
            m_liveRepair->setTarget(
                m_removal->liveSourceTimeUs(m_preview->positionMs() * 1000));
            m_preview->setLiveRepairProvider(m_liveRepair);
            m_liveRepair->start();
            return true;
        });
    m_removal->setLivePreviewStopper([this] {
        if (m_liveRefreshTimer.isActive())
            m_liveRefreshTimer.stop();
        if (m_liveRepair)
            m_liveRepair->stop();
        if (m_preview)
            m_preview->setLiveRepairProvider(LiveRepairProviderPtr{});
    });

    connect(m_document, &ProjectDocument::projectReplaced, this, [this] {
        if (m_removal && m_removal->livePreview())
            m_removal->setLivePreview(false);
        m_maskSidecars->setProjectPath(m_document->filePath(),
                                       m_sidecarFallback);
    });
    // The inspector hides RemovalPanel when the selection changes away from
    // the masked clip. Leaving mask mode active would let the transparent
    // overlay keep swallowing preview clicks with no visible exit control.
    connect(m_timeline, &TimelineController::selectionChanged, this, [this] {
        // Live preview is bound to one layer and one source range.
        if (m_removal->livePreview())
            m_removal->setLivePreview(false);
        // The selected clip can satisfy activeRemovalLayer()/hasLayer() without
        // mask mode, so refresh the analysis bindings for every selection.
        m_removal->notifySelectionChanged();
        if (!m_removal->maskMode())
            return;
        const QString selected = m_timeline->selectedClipId();
        if (selected.isEmpty() || selected != m_removal->activeClipId())
            m_removal->endMask();
    });

    m_waveforms = new WaveformService(
        QDir(appData).filePath(QStringLiteral("cache")), this);
    m_audio = std::make_unique<AudioEngine>(nullptr, std::move(audioSinkFactory));
    // A missing or unplugged output device must not stop the preview: playback
    // falls back to the wall clock, but the user still needs to know why the
    // timeline is silent.
    connect(m_audio.get(), &AudioEngine::deviceFailed, this,
            [this](const QString &message) {
                setLastError(message.isEmpty()
                                 ? QStringLiteral("无音频输出，静音预览。")
                                 : QStringLiteral("无音频输出，静音预览（%1）")
                                       .arg(message));
            });
    // The device is opened in the AudioEngine constructor, before the signal
    // above can be connected, so surface an initial failure explicitly.
    if (!m_audio->hasOutputDevice()) {
        const QString deviceError = m_audio->lastDeviceError();
        setLastError(deviceError.isEmpty()
                         ? QStringLiteral("无音频输出，静音预览。")
                         : QStringLiteral("无音频输出，静音预览（%1）")
                               .arg(deviceError));
    }
    m_preview->setAudioEngine(m_audio.get());

    // Keep the compositor and audio mixer snapshots in sync with edits without
    // resetting the playhead or playback state. Coalesce command bursts.
    m_timelineUpdateTimer.setSingleShot(true);
    m_timelineUpdateTimer.setInterval(40);
    connect(&m_timelineUpdateTimer, &QTimer::timeout, this, [this] {
        if (m_preview->timelineMode())
            m_preview->updateTimelineProject(m_document->project());
    });
    const auto scheduleTimelineUpdate = [this] {
        m_timelineUpdateTimer.start();
    };
    connect(m_document, &ProjectDocument::timelineChanged, this,
            scheduleTimelineUpdate);
    connect(m_document, &ProjectDocument::tracksChanged, this,
            scheduleTimelineUpdate);
    // Enabling/deleting a removal layer changes which cached patch the
    // compositor should draw. Without this the paused preview keeps the old
    // snapshot until the next unrelated edit.
    connect(m_document, &ProjectDocument::removalLayersChanged, this,
            scheduleTimelineUpdate);
    // Editing parameters, deleting the layer or regenerating invalidates the
    // live provider's spec and tracking masks; stop instead of drawing a stale
    // patch. The user toggles live preview on again after the edit.
    connect(m_document, &ProjectDocument::removalLayersChanged, this, [this] {
        if (m_removal && m_removal->livePreview())
            m_removal->setLivePreview(false);
    });

    const QString cacheRoot = QDir(appData).filePath(QStringLiteral("cache"));
    m_proxyService = new ProxyService(cacheRoot, resolveFfmpegPath(), this);
    connect(m_proxyService, &ProxyService::ready, this,
            [this](const QString &assetId, const QString &proxyPath) {
                m_document->setAssetProxyPath(assetId, proxyPath);
                // Proxy completion does not emit timelineChanged; push the new
                // asset metadata to the compositor so the next frame uses it.
                if (m_preview->timelineMode())
                    m_preview->updateTimelineProject(m_document->project());
                if (!m_currentAssetId.isEmpty() && m_currentAssetId == assetId
                    && !m_preview->timelineMode() && m_useProxy) {
                    openAsset(assetId);
                }
            });
    connect(m_waveforms, &WaveformService::failed, this,
            [this](const QString &, const QString &message) {
                setLastError(message.isEmpty()
                                 ? QStringLiteral("波形生成失败。")
                                 : QStringLiteral("波形生成失败：%1").arg(message));
            });
    connect(m_proxyService, &ProxyService::failed, this,
            [this](const QString &, const QString &message) {
                setLastError(message);
            });
}

AppController::~AppController()
{
    // Join the live preview worker and release the provider before the preview
    // compositor is destroyed. Export never saw this provider.
    m_liveRefreshTimer.stop();
    if (m_removal && m_removal->livePreview())
        m_removal->setLivePreview(false);
    if (m_liveRepair)
        m_liveRepair->stop();
    // The preview's render thread holds a shared reference to the provider.
    // Destroy the preview explicitly while this object's m_liveRepair member
    // is still alive so the provider is released on the GUI thread, not from
    // the render thread during deferred child cleanup. pause() first: it
    // forwards to the audio engine, which must still be alive.
    if (m_preview) {
        m_preview->pause();
        m_preview->setParent(nullptr);
        delete m_preview;
        m_preview = nullptr;
    }
    // AudioEngine owns its own worker thread, so shutdown() is a blocking,
    // idempotent stop that no callback can outlive.
    if (m_audio)
        m_audio->shutdown();
}

bool AppController::aiFeatureEnabled() const
{
    return m_aiGate && m_aiGate->enabled();
}

bool AppController::aiTechnicalPreview() const
{
    return m_aiGate && m_aiGate->technicalPreview();
}

QString AppController::aiFeatureDisabledReason() const
{
    return m_aiGate ? m_aiGate->disabledReason() : QString();
}

bool AppController::canUndo() const
{
    return m_document->canUndo();
}

bool AppController::canRedo() const
{
    return m_document->canRedo();
}

void AppController::importUrls(const QList<QUrl> &urls)
{
    QStringList failures;

    for (const QUrl &url : urls) {
        const QString path = url.toLocalFile();
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        if (!info) {
            failures.append(error);
            continue;
        }
        if (!info->supported) {
            failures.append(info->unsupportedReason);
            continue;
        }

        Asset asset;
        asset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        asset.path = path;
        asset.info = *info;
        m_document->addAsset(asset);

        // Proxies are cache metadata: generated in the background and stored
        // outside the undo stack, so an import never blocks on ffmpeg.
        const ProxyPlan plan = planProxy(asset.info);
        if (plan.needed)
            m_proxyService->request(asset.id, asset.path, plan.height);

        // Waveform peaks are built from the source file on a worker thread;
        // the request itself never blocks the GUI.
        if (asset.info.hasAudio)
            m_waveforms->request(asset.id, asset.path);
    }

    if (failures.isEmpty())
        clearError();
    else
        setLastError(failures.join(QLatin1Char('\n')));
}

void AppController::addAssetToTimeline(const QString &assetId,
                                       const QString &targetTrackId)
{
    const Asset *asset = m_document->project().assetById(assetId);
    if (!asset) {
        setLastError(QStringLiteral("未找到素材。"));
        return;
    }

    const bool wantsVideo =
        asset->info.hasVideo || asset->info.kind == MediaKind::Video;
    const TrackType wanted = wantsVideo ? TrackType::Video : TrackType::Audio;

    // Prefer the user-chosen lane; the controller pairs original audio with
    // the same-ordinal A-track (V2 -> A2). Fall back to the first track of the
    // wanted type when the target is missing or of the wrong type.
    QString trackId = targetTrackId;
    const Track *target = m_document->trackById(trackId);
    if (!target || target->type == TrackType::Subtitle) {
        trackId.clear();
        if (wanted == TrackType::Audio && !wantsVideo) {
            // Standalone music must not land on the lane that already holds a
            // video's original audio: mute/solo and waveforms would collide.
            // Use the first free audio lane, or create a dedicated BGM lane.
            const Project &project = m_document->project();
            QString firstAudioTrack;
            QString freeAudioTrack;
            for (const Track &track : project.tracks) {
                if (track.type != TrackType::Audio)
                    continue;
                if (firstAudioTrack.isEmpty())
                    firstAudioTrack = track.id;
                bool hasOriginalAudio = false;
                for (const Clip &clip : track.clips) {
                    const Asset *clipAsset = project.assetById(clip.assetId);
                    if (clipAsset
                        && (clipAsset->info.hasVideo
                            || clipAsset->info.kind == MediaKind::Video)) {
                        hasOriginalAudio = true;
                        break;
                    }
                }
                if (!hasOriginalAudio) {
                    freeAudioTrack = track.id;
                    break;
                }
            }
            if (!freeAudioTrack.isEmpty()) {
                trackId = freeAudioTrack;
            } else if (!firstAudioTrack.isEmpty()) {
                trackId = m_timeline->addTrack(1);
            } else {
                trackId.clear();
            }
        } else {
            for (const Track &track : m_document->project().tracks) {
                if (track.type == wanted) {
                    trackId = track.id;
                    break;
                }
            }
        }
    }
    if (trackId.isEmpty()) {
        setLastError(QStringLiteral("没有可用的轨道。"));
        return;
    }

    const qint64 positionUs = m_timeline->positionUs();
    const QString clipId =
        m_timeline->addAssetToTimeline(assetId, trackId, positionUs);
    if (clipId.isEmpty())
        return; // TimelineController already reported the failure.
    clearError();
    // Select the inserted clip so the inspector immediately shows its volume,
    // fades and transform controls (CapCut-style "what I just added" focus).
    m_timeline->selectClip(clipId);

    const Project &project = m_document->project();
    const int64_t duration = TimelineEvaluator(project).durationUs();
    if (m_preview->timelineMode()) {
        // An edit must not jump the playhead back to the start or pause.
        m_preview->updateTimelineProject(project);
    } else if (duration > 0) {
        m_preview->openTimeline(project, duration, m_useProxy);
        seekTimeline(positionUs);
    }
}

void AppController::seekTimeline(qint64 positionUs)
{
    m_syncingPlayhead = true;
    m_timeline->seekUs(positionUs);
    if (m_preview->timelineMode()) {
        m_preview->seekMs(qMax<qint64>(0, positionUs) / 1000);
        // seekFrame() snaps to the project frame grid; reflect that exact
        // position back on the playhead so edits and playback agree.
        m_timeline->seekUs(m_preview->positionMs() * 1000);
    }
    m_syncingPlayhead = false;
}

QVariantList AppController::subtitleStyles() const
{
    // Expose the project's merged style table (builtins plus any overrides),
    // which is exactly what the compositor resolves by id. Falling back to the
    // builtin list keeps the ComboBox usable before a document exists.
    const QList<SubtitleStyle> &projectStyles = m_document->project().subtitleStyles;
    const QList<SubtitleStyle> fallback = builtinSubtitleStyles();
    const QList<SubtitleStyle> &styles =
        projectStyles.isEmpty() ? fallback : projectStyles;

    QVariantList entries;
    for (const SubtitleStyle &style : styles) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), style.id);
        entry.insert(QStringLiteral("name"), style.name);
        entries.append(entry);
    }
    return entries;
}

void AppController::refreshTimelinePreview()
{
    const Project &project = m_document->project();
    const int64_t duration = TimelineEvaluator(project).durationUs();
    m_timeline->seekUs(0);
    // An empty timeline must not replace a closed asset preview (e.g. right
    // after opening a project that only contains imported assets).
    if (duration <= 0)
        return;
    m_preview->openTimeline(project, duration, m_useProxy);
}

void AppController::openAsset(const QString &id)
{
    for (const Asset &asset : m_document->project().assets) {
        if (asset.id == id) {
            // Undo/redo, a cleared cache, or a project saved before generation
            // finished can leave the asset without a usable proxy. Re-request
            // it so the preview does not fall back to the source forever.
            if (needsProxyGeneration(asset.info, asset.proxyPath)) {
                m_proxyService->request(asset.id, asset.path,
                                        planProxy(asset.info).height);
            }
            m_currentAssetId = id;
            m_preview->openMedia(asset.path,
                                 m_useProxy ? asset.proxyPath : QString());
            return;
        }
    }
}

void AppController::beginPopup()
{
    ++m_openPopupCount;
    emit openPopupCountChanged();
}

void AppController::endPopup()
{
    // A menu is normally closed before it is destroyed; clamp so a stray
    // close can never leave the editing shortcuts permanently disabled.
    if (m_openPopupCount <= 0)
        return;
    --m_openPopupCount;
    emit openPopupCountChanged();
}

void AppController::setUseProxy(bool enabled)
{
    if (m_useProxy == enabled)
        return;
    m_useProxy = enabled;
    emit useProxyChanged();
    // Re-opening the preview is what actually switches between proxy and
    // source. In timeline mode the render worker must drop its decoders; in
    // asset mode the current file is reopened.
    if (m_preview->timelineMode()) {
        m_preview->setUseProxy(enabled);
    } else if (!m_currentAssetId.isEmpty()) {
        openAsset(m_currentAssetId);
    }
}

void AppController::undo()
{
    m_document->undo();
}

void AppController::redo()
{
    m_document->redo();
}

void AppController::clearError()
{
    if (m_lastError.isEmpty())
        return;
    m_lastError.clear();
    emit lastErrorChanged();
}

void AppController::restoreRecovery()
{
    QString error;
    QString savedPath;
    auto project = m_store->loadRecovery(&error, &savedPath);
    if (!project) {
        setLastError(error);
        return;
    }
    // Restoring the original file path keeps mask sidecars next to the saved
    // project instead of rebinding them to the unsaved fallback root.
    m_document->setProject(std::move(*project), savedPath);
    m_currentAssetId.clear();
    // The restored document replaces whatever was previewed; stop and clear
    // the old preview so its clip does not keep playing behind the new project.
    m_preview->closeMedia();
    m_store->discardRecovery();
    setRecoveryAvailable(false);
    requestWaveforms();
    requestMissingProxies();
    refreshTimelinePreview();
    clearError();
}

void AppController::discardRecovery()
{
    m_store->discardRecovery();
    setRecoveryAvailable(false);
}

void AppController::saveProject(const QUrl &url)
{
    QString path = url.toLocalFile();
    // The Qt Quick save dialog does not expose a default suffix, so add the
    // project extension here when the user did not type one.
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".veproj");

    // Masks authored before the first save live under the fallback root; move
    // them next to the project BEFORE writing the .veproj. A failed copy must
    // abort the save: writing a project whose sidecars stayed behind would
    // make the file unopenable, and pointing the store at an unpopulated root
    // would silently lose later masks.
    const QString previousSidecarPath = m_maskSidecars->projectFilePath();
    QString sidecarError;
    if (!m_maskSidecars->relocateTo(path, &sidecarError)) {
        setLastError(sidecarError.isEmpty()
                         ? QStringLiteral("无法迁移遮罩旁车文件。")
                         : sidecarError);
        return;
    }
    QString error;
    if (!m_document->save(path, &error)) {
        m_maskSidecars->setProjectPath(previousSidecarPath, m_sidecarFallback);
        setLastError(error);
        return;
    }
    // The project on disk now supersedes any autosaved snapshot; keeping the
    // recovery file would let the next launch roll back edits saved after the
    // last autosave.
    m_store->discardRecovery();
    setRecoveryAvailable(false);
    clearError();
}

void AppController::openProject(const QUrl &url)
{
    QString error;
    if (!m_document->load(url.toLocalFile(), &error)) {
        setLastError(error);
        return;
    }
    m_currentAssetId.clear();
    // The loaded project replaces the current document, so the previous
    // asset's preview must stop instead of playing on behind it.
    m_preview->closeMedia();
    requestWaveforms();
    // A project saved before proxy generation finished (or whose cache was
    // cleared) must regenerate proxies; ready() then refreshes the timeline.
    requestMissingProxies();
    refreshTimelinePreview();
    clearError();
}

void AppController::discardRecoveryIfClean()
{
    if (m_document->isDirty() || m_recoveryAvailable)
        return;
    m_store->discardRecovery();
}

void AppController::selftest(const QString &path)
{
    importUrls({QUrl::fromLocalFile(path)});
    if (m_mediaModel->rowCount() <= 0)
        return;
    const Asset &asset = m_document->project().assets.first();
    openAsset(asset.id);
}

void AppController::requestWaveforms()
{
    for (const Asset &asset : m_document->project().assets) {
        if (asset.info.hasAudio)
            m_waveforms->request(asset.id, asset.path);
    }
}

void AppController::requestMissingProxies()
{
    for (const Asset &asset : m_document->project().assets) {
        if (needsProxyGeneration(asset.info, asset.proxyPath)) {
            m_proxyService->request(asset.id, asset.path,
                                    planProxy(asset.info).height);
        }
    }
}

void AppController::buildTimelineSelftestProject()
{
    const QString root = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR);
    // Native 1080p source so the performance gate stresses real decode load,
    // not only 1080p compositing of upscaled 640x360 material.
    importUrls({QUrl::fromLocalFile(
                    QDir(root).filePath(QStringLiteral("perf1080.mp4"))),
                QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("tone.wav"))),
                QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("bgm.mp3")))});
    if (m_document->project().assets.size() < 3) {
        setLastError(QStringLiteral("自检素材缺失。"));
        return;
    }

    const QString videoId = m_document->project().assets.at(0).id;
    const QString toneId = m_document->project().assets.at(1).id;
    const QString bgmId = m_document->project().assets.at(2).id;

    m_timeline->addAssetToTimeline(videoId, QStringLiteral("track-v1"), 0);

    // Two extra video tracks and a second audio track so the self-test covers
    // the spec's 3V/2A/1S 1080p timeline instead of a single-layer project.
    m_timeline->addTrack(0);
    m_timeline->addTrack(0);
    m_timeline->addTrack(1);

    QStringList videoTracks;
    QStringList audioTracks;
    QString subtitleTrackId;
    for (const Track &track : m_document->project().tracks) {
        if (track.type == TrackType::Video)
            videoTracks.append(track.id);
        else if (track.type == TrackType::Audio)
            audioTracks.append(track.id);
        else if (track.type == TrackType::Subtitle)
            subtitleTrackId = track.id;
    }

    // perf1080.mp4 is five seconds long. Place it twice on every video track
    // so all three layers stay active for the whole warm-up + measurement
    // window (1 s + 5 s); a gate that only saw three layers briefly could
    // average a bad three-layer frame rate away with fast empty frames.
    const qint64 videoDurationUs = m_document->project().assets.at(0).info.durationUs;
    const auto addVideoClip = [this, &videoId, videoDurationUs](
                                  const QString &trackId, const QString &id,
                                  int index, int scaleMilli, int offsetX) {
        Clip clip;
        clip.id = id;
        clip.assetId = videoId;
        clip.sourceInUs = 0;
        clip.sourceOutUs = videoDurationUs;
        clip.timelineStartUs = qint64(index) * videoDurationUs;
        clip.transform.scaleMilli = scaleMilli;
        clip.transform.offsetXPermille = offsetX;
        m_timeline->addClipForTest(trackId, clip);
    };

    if (videoTracks.size() >= 3) {
        const QString v1 = videoTracks.at(videoTracks.size() - 3);
        const QString v2 = videoTracks.at(videoTracks.size() - 2);
        const QString v3 = videoTracks.last();
        addVideoClip(v1, QStringLiteral("perf-v1-1"), 1, 1000, 0);
        for (int i = 0; i < 2; ++i) {
            addVideoClip(v2, QStringLiteral("perf-v2-%1").arg(i), i, 500, 300);
            addVideoClip(v3, QStringLiteral("perf-v3-%1").arg(i), i, 500, -300);
        }
    }

    // Two audio tracks: tone on A1, BGM on A2, so both are mixed independently.
    m_timeline->addAssetToTimeline(toneId, QStringLiteral("track-a1"), 0);
    if (audioTracks.size() >= 2)
        m_timeline->addAssetToTimeline(bgmId, audioTracks.last(), 0);
    else
        m_timeline->addAssetToTimeline(bgmId, QStringLiteral("track-a1"), 0);

    for (int i = 0; i < 3; ++i) {
        m_timeline->seekUs(i * 1'000'000);
        m_timeline->addCueAtPlayhead();
        const Track *subtitleTrack = m_document->trackById(subtitleTrackId);
        if (!subtitleTrack || subtitleTrack->cues.isEmpty())
            continue;
        const QString cueId = subtitleTrack->cues.last().id;
        m_timeline->setCueText(
            cueId, QStringLiteral("多轨自检字幕 %1").arg(i + 1));
    }

    int videoClipCount = 0;
    int audioClipCount = 0;
    int cueCount = 0;
    int videoTrackCount = 0;
    int audioTrackCount = 0;
    int subtitleTrackCount = 0;
    for (const Track &track : m_document->project().tracks) {
        switch (track.type) {
        case TrackType::Video:
            ++videoTrackCount;
            videoClipCount += int(track.clips.size());
            break;
        case TrackType::Audio:
            ++audioTrackCount;
            audioClipCount += int(track.clips.size());
            break;
        case TrackType::Subtitle:
            ++subtitleTrackCount;
            cueCount += int(track.cues.size());
            break;
        }
    }
    const TimelineSettings &settings = m_document->project().timeline;
    qInfo("TIMELINE_SELFTEST_PROJECT resolution=%dx%d fps=%d/%d videoTracks=%d "
          "audioTracks=%d subtitleTracks=%d videoClips=%d audioClips=%d cues=%d",
          settings.width, settings.height, settings.fps.num, settings.fps.den,
          videoTrackCount, audioTrackCount, subtitleTrackCount, videoClipCount,
          audioClipCount, cueCount);
}

void AppController::selftestTimeline()
{
    buildTimelineSelftestProject();
    if (m_document->project().assets.size() < 3)
        return;
    // Unit tests build the fixture project without a QML preview item. With
    // no scene-graph target there is nothing to present, so only the command-
    // line self-tests (which load Main.qml first) open the timeline preview.
    if (!m_preview->item())
        return;
    refreshTimelinePreview();
    m_timeline->seekUs(0);
    m_preview->play();
}

void AppController::selftestPerf()
{
    // Same 3V/2A/1S project as --selftest-timeline. The gate counts frames the
    // scene graph actually presented over five seconds and checks that those
    // frames really contain the expected video layers; compositor throughput,
    // decode and audio mixing all run for real.
    m_perfFrames = 0;
    m_perfLayerSamples = 0;
    m_perfFullLayerFrames = 0;
    m_perfMinLayers = 0;
    m_perfExpectedLayers = 0;
    m_perfClock.invalidate();
    selftestTimeline();
    if (m_document->project().assets.size() < 3) {
        qCritical("TIMELINE_PERF_MISSING_FIXTURES");
        QCoreApplication::exit(3);
        return;
    }
    for (const Track &track : m_document->project().tracks) {
        if (track.type == TrackType::Video)
            ++m_perfExpectedLayers;
    }

    // Guard against a stalled event loop or a renderer that never presents:
    // the normal path exits well before this.
    QTimer::singleShot(20000, this, [this] {
        qCritical("TIMELINE_PERF_TIMEOUT");
        QCoreApplication::exit(4);
    });

    // Warm up for one second so decoder pools, the D3D11 device and the
    // presentation timer reach steady state before measurement starts.
    QTimer::singleShot(1000, this, [this] {
        m_perfFrames = 0;
        m_perfLayerSamples = 0;
        m_perfFullLayerFrames = 0;
        m_perfMinLayers = 0;
        m_perfClock.start();
        QTimer::singleShot(5000, this, [this] {
            const qint64 elapsedMs = m_perfClock.elapsed();
            m_perfClock.invalidate();
            const double seconds = elapsedMs > 0 ? elapsedMs / 1000.0 : 0.0;
            const double fps = seconds > 0.0 ? m_perfFrames / seconds : 0.0;
            const bool fpsOk = fps >= 24.0;
            // Every presented frame must carry layer-count evidence, and at
            // least 90% of measured frames must contain all expected layers.
            const bool samplesOk = m_perfFrames > 0
                                   && m_perfLayerSamples == m_perfFrames;
            const bool contentOk =
                samplesOk && m_perfFullLayerFrames * 10 >= m_perfFrames * 9;
            qInfo("TIMELINE_FPS=%.1f FRAMES=%d ELAPSED_MS=%lld "
                  "LAYER_SAMPLES=%d FULL_LAYER_FRAMES=%d EXPECTED_LAYERS=%d "
                  "MIN_LAYERS=%d",
                  fps, m_perfFrames, static_cast<long long>(elapsedMs),
                  m_perfLayerSamples, m_perfFullLayerFrames,
                  m_perfExpectedLayers, m_perfMinLayers);
            qInfo("TIMELINE_PERF_%s fps=%.1f content=%s",
                  fpsOk && contentOk ? "OK" : "FAILED", fps,
                  contentOk ? "ok" : "failed");
            QCoreApplication::exit(fpsOk && contentOk ? 0 : 3);
        });
    });
}

void AppController::selftestExport()
{
    // Reuse the M2 3V/2A/1S project builder; export is deliberately tested
    // against the same timeline the preview performance gate uses.
    buildTimelineSelftestProject();
    Project project = m_document->project();
    if (project.assets.size() < 3) {
        qCritical("EXPORT_SELFTEST_FAIL missing fixtures");
        QCoreApplication::exit(5);
        return;
    }
    // Small enough that the full three-codec matrix stays well inside the
    // CTest timeout even on a software-only machine.
    project.timeline.width = 640;
    project.timeline.height = 360;
    project.timeline.fps = Rational{30, 1};

    // One unique directory per run, removed by QTemporaryDir on every exit
    // path (success, validation failure, missing encoder, timeout unwind).
    QTemporaryDir runDir(
        QDir(QDir::tempPath())
            .filePath(QStringLiteral("videoeditor-export-selftest-XXXXXX")));
    if (!runDir.isValid()) {
        qCritical("EXPORT_SELFTEST_FAIL cannot create %s",
                  qPrintable(runDir.path()));
        QCoreApplication::exit(5);
        return;
    }
    const QString root = runDir.path();
    // Encoder probing and Media Foundation need an MTA apartment. The Qt GUI
    // thread is STA (OLE), where h264_mf refuses to open and leaves the
    // process in a state that crashes the qwindows plugin at shutdown. Run
    // both detection and the export on a dedicated MTA thread, exactly like
    // ExportController + ExportWorker do in the real application.
    bool ok = false;
    std::thread worker([&ok, project, root] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const EncoderCatalog catalog =
            detectEncoders(project.timeline.width, project.timeline.height,
                           project.timeline.fps);
        QStringList done;
        int64_t maxAvDiffUs = 0;
        bool failed = false;
        for (ExportVideoCodec codec : {ExportVideoCodec::Mpeg4Part2,
                                       ExportVideoCodec::H264,
                                       ExportVideoCodec::H265}) {
            bool hardware = false;
            QString reason;
            const QString encoder =
                catalog.selectEncoder(codec, true, &hardware, &reason);
            if (encoder.isEmpty()) {
                qInfo("EXPORT_SELFTEST_SKIP codec=%s reason=%s",
                      qPrintable(exportCodecToString(codec)),
                      qPrintable(reason));
                continue;
            }
            ExportSettings settings;
            settings.codec = codec;
            settings.width = project.timeline.width;
            settings.height = project.timeline.height;
            settings.fps = project.timeline.fps;
            settings.rateControl = ExportRateControl::Bitrate;
            settings.bitrateKbps = 6000;
            settings.outputPath = QDir(root).filePath(
                exportCodecToString(codec) + QStringLiteral(".mp4"));
            settings.segmentTargetUs = 1'000'000;
            ExportPipeline pipeline;
            const ExportResult result =
                pipeline.run(project, settings, catalog, false, {});
            if (!result.ok) {
                qCritical("EXPORT_SELFTEST_FAIL codec=%s error=%s",
                          qPrintable(exportCodecToString(codec)),
                          qPrintable(result.error));
                failed = true;
                break;
            }
            const int64_t frameUs = 1'000'000 / 30;
            const int64_t diff =
                std::llabs(result.validation.audioDurationUs
                           - result.validation.videoDurationUs);
            // FFmpeg's headers pull in windows.h, whose max() macro would
            // break std::max; qMax is unaffected.
            maxAvDiffUs = qMax(maxAvDiffUs, diff);
            if (diff > frameUs + 1) {
                qCritical("EXPORT_SELFTEST_FAIL codec=%s avDiffUs=%lld",
                          qPrintable(exportCodecToString(codec)),
                          static_cast<long long>(diff));
                failed = true;
                break;
            }
            qInfo("EXPORT_SELFTEST_CODEC_OK codec=%s encoder=%s hardware=%d",
                  qPrintable(exportCodecToString(codec)),
                  qPrintable(result.encoderId),
                  result.usedHardware ? 1 : 0);
            done.append(exportCodecToString(codec));
        }
        if (!failed && done.isEmpty()) {
            qCritical("EXPORT_SELFTEST_FAIL no encoder available");
            failed = true;
        }
        if (!failed) {
            qInfo("EXPORT_SELFTEST_OK codecs=%s maxAvDiffUs=%lld",
                  qPrintable(done.join(QLatin1Char(','))),
                  static_cast<long long>(maxAvDiffUs));
        }
        ok = !failed;
        if (SUCCEEDED(com))
            CoUninitialize();
    });
    worker.join();
    QCoreApplication::exit(ok ? 0 : 5);
}

void AppController::selftestRemoval()
{
    const auto fail = [](const QString &reason) {
        qCritical("REMOVAL_SELFTEST_FAIL %s", qPrintable(reason));
        QCoreApplication::exit(4);
    };

    // Fail closed: never fabricate a pass without a verified model bundle. The
    // internal technical preview only exists when VIDEOEDITOR_M4_TECHNICAL=1.
    if (!m_aiGate || !m_aiGate->enabled() || !m_aiGate->registry()) {
        const QString reason =
            m_aiGate ? m_aiGate->disabledReason()
                     : QStringLiteral("AI 消除功能未启用：未加载模型清单。");
        qInfo("REMOVAL_SELFTEST_SKIP %s", qPrintable(reason));
        QCoreApplication::exit(4);
        return;
    }
    const ModelRegistry *registry = m_aiGate->registry();

    // Prefer the full-size smooth acceptance set (a natural-grade gradient GT
    // is a fair quality probe); fall back to the legacy high-frequency 1080p
    // set, then to the committed 640x360 corpus. VIDEOEDITOR_M4_ACCEPTANCE_SET
    // forces "smooth" or "legacy" so both PSNR rows stay reproducible.
    const QString acceptance = QStringLiteral("C:/m4-spike/acceptance");
    const QString repoDir =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("removal"));
    QString videoPath;
    QString maskPath;
    QString cleanPath;
    QString setName;
    bool is1080p = false;
    bool found = false;
    const QString forced =
        qEnvironmentVariable("VIDEOEDITOR_M4_ACCEPTANCE_SET").trimmed().toLower();
    const auto trySet = [&](const QString &name, const QString &video,
                            const QString &mask, const QString &clean,
                            bool fullSize) {
        if (found || !QFileInfo::exists(video) || !QFileInfo::exists(mask)
            || !QFileInfo::exists(clean)) {
            return;
        }
        videoPath = video;
        maskPath = mask;
        cleanPath = clean;
        setName = name;
        is1080p = fullSize;
        found = true;
    };
    const QString softVideo = QDir(acceptance).filePath(
        QStringLiteral("static_logo_soft_1080p_10s.mp4"));
    const QString softMask = QDir(acceptance).filePath(
        QStringLiteral("static_logo_soft_mask_1080p_10s.mp4"));
    const QString softClean = QDir(acceptance).filePath(
        QStringLiteral("clean_soft_1080p_10s.mp4"));
    const QString smoothVideo =
        QDir(acceptance).filePath(QStringLiteral("static_logo_smooth_1080p_10s.mp4"));
    const QString smoothMask = QDir(acceptance).filePath(
        QStringLiteral("static_logo_smooth_mask_1080p_10s.mp4"));
    const QString smoothClean = QDir(acceptance).filePath(
        QStringLiteral("clean_gradient_1080p_10s.mp4"));
    const QString legacyVideo =
        QDir(acceptance).filePath(QStringLiteral("static_logo_1080p_10s.mp4"));
    const QString legacyMask = QDir(acceptance).filePath(
        QStringLiteral("static_logo_mask_1080p_10s.mp4"));
    const QString legacyClean = QDir(acceptance).filePath(
        QStringLiteral("clean_background_1080p_10s.mp4"));
    if (forced == QLatin1String("repo")) {
        // Diagnostic/quick path: use the committed corpus directly.
    } else if (forced == QLatin1String("legacy")) {
        trySet(QStringLiteral("legacy-1080p"), legacyVideo, legacyMask,
               legacyClean, true);
    } else if (forced == QLatin1String("smooth")) {
        trySet(QStringLiteral("smooth-1080p"), smoothVideo, smoothMask,
               smoothClean, true);
    } else if (forced == QLatin1String("soft")) {
        trySet(QStringLiteral("soft-1080p"), softVideo, softMask, softClean,
               true);
    } else {
        // Preference: fair low-frequency soft plate, then smooth gradient,
        // then the legacy high-frequency testsrc2 plate.
        trySet(QStringLiteral("soft-1080p"), softVideo, softMask, softClean,
               true);
        trySet(QStringLiteral("smooth-1080p"), smoothVideo, smoothMask,
               smoothClean, true);
        trySet(QStringLiteral("legacy-1080p"), legacyVideo, legacyMask,
               legacyClean, true);
    }
    if (!found) {
        if (forced != QLatin1String("legacy")) {
            trySet(QStringLiteral("smooth-640"),
                   QDir(repoDir).filePath(QStringLiteral("static_logo_smooth.mp4")),
                   QDir(repoDir).filePath(
                       QStringLiteral("static_logo_smooth_mask.mp4")),
                   QDir(repoDir).filePath(QStringLiteral("clean_gradient.mp4")),
                   false);
        }
        if (forced != QLatin1String("smooth")) {
            trySet(QStringLiteral("static-640"),
                   QDir(repoDir).filePath(QStringLiteral("static_logo.mp4")),
                   QDir(repoDir).filePath(
                       QStringLiteral("static_logo_mask.mp4")),
                   QDir(repoDir).filePath(
                       QStringLiteral("clean_background.mp4")),
                   false);
        }
    }
    if (!found) {
        fail(QStringLiteral("fixtures_missing dir=%1 set=%2")
                 .arg(repoDir, forced.isEmpty() ? QStringLiteral("auto")
                                                : forced));
        return;
    }
    qInfo("REMOVAL_SELFTEST_FIXTURE set=%s file=%s", qPrintable(setName),
          qPrintable(videoPath));

    QString error;
    const auto info = MediaProbe::probe(videoPath, &error);
    if (!info || !info->hasVideo || info->durationUs <= 0) {
        fail(QStringLiteral("probe_failed %1").arg(error));
        return;
    }
    const Rational fps =
        info->frameRate.isValid() ? info->frameRate : Rational{30, 1};
    const int64_t durationUs = info->durationUs;
    Asset asset;
    asset.id = QStringLiteral("selftest-asset");
    asset.path = videoPath;
    asset.info = *info;
    const QSize display = displaySizeForAsset(asset);

    // Mask keyframe 0 from the ground-truth mask video (luma > 127).
    FrameReader maskReader;
    if (!maskReader.open(maskPath, fps, 0, durationUs, display, &error)) {
        fail(QStringLiteral("mask_open_failed %1").arg(error));
        return;
    }
    const QImage maskFrame = maskReader.readFrame(0, &error);
    if (maskFrame.isNull() || maskFrame.size() != display) {
        fail(QStringLiteral("mask_decode_failed size=%1x%2 expected=%3x%4 %5")
                 .arg(maskFrame.width())
                 .arg(maskFrame.height())
                 .arg(display.width())
                 .arg(display.height())
                 .arg(error));
        return;
    }
    BinaryMask mask;
    mask.width = display.width();
    mask.height = display.height();
    mask.bits.assign(size_t(mask.width) * size_t(mask.height), 0);
    int maskPixels = 0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (qGray(maskFrame.pixel(x, y)) > 127) {
                mask.set(x, y, true);
                ++maskPixels;
            }
        }
    }
    if (maskPixels == 0) {
        fail(QStringLiteral("mask_empty"));
        return;
    }

    QTemporaryDir workDir(QDir(QDir::tempPath())
                              .filePath(QStringLiteral(
                                  "videoeditor-removal-selftest-XXXXXX")));
    if (!workDir.isValid()) {
        fail(QStringLiteral("temp_dir_failed"));
        return;
    }
    MaskSidecarStore sidecars(workDir.filePath(QStringLiteral("selftest.veproj")));
    if (!sidecars.ensureLayout(&error)) {
        fail(QStringLiteral("sidecar_layout_failed %1").arg(error));
        return;
    }
    QString maskHash;
    if (!sidecars.writeMask(mask, &maskHash, &error)) {
        fail(QStringLiteral("sidecar_write_failed %1").arg(error));
        return;
    }

    Project project;
    project.timeline.width = display.width();
    project.timeline.height = display.height();
    project.timeline.fps = fps;
    project.assets.append(asset);

    Track track;
    track.id = QStringLiteral("selftest-track");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("selftest-clip");
    clip.assetId = asset.id;
    clip.sourceInUs = 0;
    clip.sourceOutUs = durationUs;
    clip.timelineStartUs = 0;
    clip.removalLayerId = QStringLiteral("selftest-layer");
    track.clips.append(clip);
    project.tracks.append(track);

    RemovalLayer layer;
    layer.id = QStringLiteral("selftest-layer");
    layer.clipId = clip.id;
    layer.sourceInUs = 0;
    layer.sourceOutUs = durationUs;
    layer.parameters.mode = RepairMode::Fast;
    layer.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("selftest-mask-0"), 0, maskHash, 4, 1});
    project.removalLayers.append(layer);

    // The cache key must be exactly the one export preflight will later check.
    const RemovalKeyInput keyInput = RemovalPreflight::keyInputFor(
        project, project.tracks.first().clips.first(),
        project.removalLayers.first(), *registry);
    const QString key = computeRemovalCacheKey(keyInput);
    if (key.isEmpty()) {
        fail(QStringLiteral("cache_key_failed"));
        return;
    }

    const AiModelRecord *tracking = registry->defaultTrackingModel();
    const AiModelRecord *inpaint =
        registry->preferredInpaintModel(layer.parameters.mode);
    if (!tracking || !inpaint) {
        fail(QStringLiteral("model_missing"));
        return;
    }

    QTemporaryDir cacheDir(QDir(QDir::tempPath())
                               .filePath(QStringLiteral(
                                   "videoeditor-removal-cache-XXXXXX")));
    if (!cacheDir.isValid()) {
        fail(QStringLiteral("cache_dir_failed"));
        return;
    }
    const bool keepCache =
        qEnvironmentVariable("VIDEOEDITOR_M4_SELFTEST_KEEP") == QLatin1String("1");
    if (keepCache) {
        cacheDir.setAutoRemove(false);
        qInfo("REMOVAL_SELFTEST_CACHE %s", qPrintable(cacheDir.path()));
    }
    RemovalCache cache(cacheDir.path());

    AiJobSpec spec;
    spec.jobId = QStringLiteral("selftest-removal");
    spec.clipId = clip.id;
    spec.assetPath = videoPath;
    spec.sidecarRoot = sidecars.sidecarRoot();
    spec.cacheDir = cache.cacheDir(key);
    spec.cacheKey = key;
    spec.modelsManifestPath = registry->manifestPath();
    spec.trackingModelId = tracking->id;
    spec.inpaintModelId = inpaint->id;
    spec.sourceInUs = 0;
    spec.sourceOutUs = durationUs;
    spec.width = display.width();
    spec.height = display.height();
    spec.fps = fps;
    spec.maskKeyframes = layer.maskKeyframes;
    spec.parameters = layer.parameters;

    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(600'000); // ten minutes
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    AiJobResult result;
    bool finished = false;
    const QMetaObject::Connection finishedConnection = QObject::connect(
        m_aiJobs.get(), &AiJobManager::jobFinished, &loop,
        [&loop, &result, &finished](const AiJobResult &jobResult) {
            result = jobResult;
            finished = true;
            loop.quit();
        });
    QString startError;
    if (!m_aiJobs->start(spec, &startError)) {
        QObject::disconnect(finishedConnection);
        fail(QStringLiteral("job_start_failed %1").arg(startError));
        return;
    }
    timeout.start();
    loop.exec();
    QObject::disconnect(finishedConnection);
    const double seconds = double(clock.elapsed()) / 1000.0;
    if (!finished) {
        m_aiJobs->cancel();
        fail(QStringLiteral("timeout seconds=%1")
                 .arg(seconds, 0, 'f', 1));
        return;
    }
    if (!result.ok) {
        fail(QStringLiteral("job_failed code=%1 message=%2")
                 .arg(result.errorCode, result.errorMessage));
        return;
    }

    QString stateReason;
    if (cache.state(key, &stateReason) != RemovalCacheState::Complete) {
        fail(QStringLiteral("cache_not_complete %1").arg(stateReason));
        return;
    }
    QString verifyError;
    if (!cache.verify(key, &verifyError)) {
        fail(QStringLiteral("cache_verify_failed %1").arg(verifyError));
        return;
    }
    error.clear();
    const auto source = RepairFrameSource::open(cache, key, &error);
    if (!source) {
        fail(QStringLiteral("repair_source_open_failed %1").arg(error));
        return;
    }

    FrameReader cleanReader;
    if (!cleanReader.open(cleanPath, fps, 0, durationUs, display, &error)) {
        fail(QStringLiteral("clean_open_failed %1").arg(error));
        return;
    }

    // Overall hole PSNR: accumulate squared error over every patch pixel with
    // alpha > 0 (the feathered repair mask) and compare to the clean GT.
    const double frameUs =
        1'000'000.0 * double(fps.den) / double(fps.num);
    double mseSum = 0.0;
    qint64 sampleCount = 0;
    int framesWithPatch = 0;
    const bool debug =
        qEnvironmentVariable("VIDEOEDITOR_M4_SELFTEST_DEBUG") == QLatin1String("1");
    double debugPatchR = 0.0;
    double debugPatchG = 0.0;
    double debugPatchB = 0.0;
    double debugCleanR = 0.0;
    double debugCleanG = 0.0;
    double debugCleanB = 0.0;
    qint64 debugPixels = 0;
    const int totalFrames = source->frameCount();
    // A complete manifest is not enough: the job must have processed every
    // frame, otherwise the cache could be "complete" with skipped frames.
    if (result.completedFrames != totalFrames) {
        fail(QStringLiteral("incomplete_job completedFrames=%1 totalFrames=%2")
                 .arg(result.completedFrames)
                 .arg(totalFrames));
        return;
    }
    for (int i = 0; i < totalFrames; ++i) {
        // The half-frame offset makes floor() resolve to exactly frame i.
        const int64_t sourceUs =
            int64_t(std::llround((double(i) + 0.5) * frameUs));
        const RepairFrameSource::Frame *frame =
            source->frameAt(sourceUs, &error);
        if (!error.isEmpty()) {
            fail(QStringLiteral("patch_read_failed %1").arg(error));
            return;
        }
        if (!frame || frame->patch.isNull())
            continue;
        const QImage clean = cleanReader.readFrame(i, &error);
        if (clean.isNull()) {
            fail(QStringLiteral("clean_decode_failed frame=%1 %2")
                     .arg(i)
                     .arg(error));
            return;
        }
        if (clean.size() != display) {
            fail(QStringLiteral("clean_size_mismatch frame=%1 size=%2x%3 "
                                "display=%4x%5")
                     .arg(i)
                     .arg(clean.width())
                     .arg(clean.height())
                     .arg(display.width())
                     .arg(display.height()));
            return;
        }
        if (frame->patch.size() != frame->crop.size()) {
            fail(QStringLiteral("patch_crop_mismatch frame=%1").arg(i));
            return;
        }
        const qint64 samplesBefore = sampleCount;
        const QRect crop = frame->crop;
        for (int y = 0; y < crop.height(); ++y) {
            for (int x = 0; x < crop.width(); ++x) {
                const QRgb patchPixel = frame->patch.pixel(x, y);
                if (qAlpha(patchPixel) == 0)
                    continue;
                const QRgb cleanPixel =
                    clean.pixel(crop.x() + x, crop.y() + y);
                if (debug && i == 0) {
                    debugPatchR += qRed(patchPixel);
                    debugPatchG += qGreen(patchPixel);
                    debugPatchB += qBlue(patchPixel);
                    debugCleanR += qRed(cleanPixel);
                    debugCleanG += qGreen(cleanPixel);
                    debugCleanB += qBlue(cleanPixel);
                    ++debugPixels;
                }
                const double dr = double(qRed(patchPixel)) - qRed(cleanPixel);
                const double dg = double(qGreen(patchPixel)) - qGreen(cleanPixel);
                const double db = double(qBlue(patchPixel)) - qBlue(cleanPixel);
                mseSum += dr * dr + dg * dg + db * db;
                sampleCount += 3;
            }
        }
        if (sampleCount > samplesBefore)
            ++framesWithPatch;
    }
    if (sampleCount == 0) {
        fail(QStringLiteral("no_repair_pixels frames=%1").arg(totalFrames));
        return;
    }
    // Every frame of this static-mask clip must carry at least one repaired
    // alpha>0 pixel; a complete manifest with skipped frames must not pass.
    if (framesWithPatch != totalFrames) {
        fail(QStringLiteral("incomplete_repair framesWithPatch=%1 "
                            "totalFrames=%2")
                 .arg(framesWithPatch)
                 .arg(totalFrames));
        return;
    }
    if (debug && debugPixels > 0) {
        qInfo("REMOVAL_SELFTEST_DEBUG frame=0 alphaPixels=%lld "
              "patchMeanRGB=%.1f,%.1f,%.1f cleanMeanRGB=%.1f,%.1f,%.1f",
              static_cast<long long>(debugPixels),
              debugPatchR / double(debugPixels),
              debugPatchG / double(debugPixels),
              debugPatchB / double(debugPixels),
              debugCleanR / double(debugPixels),
              debugCleanG / double(debugPixels),
              debugCleanB / double(debugPixels));
    }
    const double psnr = psnrFromMse(mseSum / double(sampleCount));

    QString sizeError;
    const qint64 cacheBytes = cache.sizeOnDisk(&sizeError);
    const qint64 sourceBytes = QFileInfo(videoPath).size();
    const qint64 budget = removalCacheBudgetBytes(sourceBytes);
    const bool psnrOk = psnr >= 28.0;
    const bool cacheOk = cacheBytes <= budget;
    // The ten-minute budget only applies to the full 1080p10s acceptance clip;
    // the 5 s 640x360 fixture is recorded but not gated on time.
    const bool timingOk = !is1080p || seconds <= 300.0;

    if (!psnrOk || !cacheOk || !timingOk) {
        const QString reason = QString::asprintf(
            "threshold set=%s file=%s psnr=%.2f psnrThreshold=28.00 "
            "seconds=%.1f timingThreshold=%s cacheBytes=%lld "
            "cacheBudget=%lld sourceBytes=%lld completedFrames=%d frames=%d "
            "provider=%s",
            qPrintable(setName), qPrintable(videoPath), psnr, seconds,
            is1080p ? "300" : "-", static_cast<long long>(cacheBytes),
            static_cast<long long>(budget),
            static_cast<long long>(sourceBytes), result.completedFrames,
            framesWithPatch, qPrintable(result.provider));
        fail(reason);
        return;
    }

    qInfo("REMOVAL_SELFTEST_OK set=%s file=%s frames=%d completedFrames=%d "
          "psnr=%.2f seconds=%.1f cacheBytes=%lld sourceBytes=%lld "
          "provider=%s",
          qPrintable(setName), qPrintable(videoPath), framesWithPatch,
          result.completedFrames, psnr, seconds,
          static_cast<long long>(cacheBytes),
          static_cast<long long>(sourceBytes), qPrintable(result.provider));
    QCoreApplication::exit(0);
}

void AppController::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

void AppController::refreshModel()
{
    m_mediaModel->setAssets(m_document->project().assets);

    // Drop the remembered asset when undo/redo or a project load removes it,
    // so toggling the proxy cannot try to reopen a stale id.
    if (!m_currentAssetId.isEmpty()) {
        const auto &assets = m_document->project().assets;
        const auto it = std::find_if(assets.cbegin(), assets.cend(),
                                     [this](const Asset &asset) {
                                         return asset.id == m_currentAssetId;
                                     });
        if (it == assets.cend())
            m_currentAssetId.clear();
    }
}

void AppController::setRecoveryAvailable(bool available)
{
    if (available == m_recoveryAvailable)
        return;
    m_recoveryAvailable = available;
    emit recoveryAvailableChanged();
}

void AppController::autosave()
{
    if (!m_document->isDirty())
        return;
    QString error;
    if (!m_store->saveRecovery(m_document->project(), &error,
                               m_document->filePath()))
        setLastError(error);
}

} // namespace VideoEditor
