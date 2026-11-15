#include "AppController.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

#include "AudioEngine.h"
#include "CodecSupport.h"
#include "MediaListModel.h"
#include "MediaProbe.h"
#include "PreviewController.h"
#include "ProxyPlanner.h"
#include "ProxyService.h"
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
                             AudioEngine::SinkFactory audioSinkFactory)
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

    m_preview = new PreviewController(this);
    connect(m_preview, &PreviewController::framePresented, this,
            [this](qint64) { emit selftestFramePresented(); });
    connect(m_preview, &PreviewController::timelineFramePresented, this,
            [this](qint64) {
                emit timelineFramePresented();
                if (m_perfClock.isValid())
                    ++m_perfFrames;
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

    const QString cacheRoot = QDir(appData).filePath(QStringLiteral("cache"));
    m_proxyService = new ProxyService(cacheRoot, resolveFfmpegPath(), this);
    connect(m_proxyService, &ProxyService::ready, this,
            [this](const QString &assetId, const QString &proxyPath) {
                m_document->setAssetProxyPath(assetId, proxyPath);
            });
    connect(m_proxyService, &ProxyService::failed, this,
            [this](const QString &, const QString &message) {
                setLastError(message);
            });
}

AppController::~AppController()
{
    // Stop the preview first: it forwards to the audio engine, which must
    // still be alive. AudioEngine owns its own worker thread, so shutdown()
    // is a blocking, idempotent stop that no callback can outlive.
    if (m_preview)
        m_preview->pause();
    if (m_audio)
        m_audio->shutdown();
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

void AppController::addAssetToTimeline(const QString &assetId)
{
    const Asset *asset = m_document->project().assetById(assetId);
    if (!asset) {
        setLastError(QStringLiteral("未找到素材。"));
        return;
    }

    const bool wantsVideo =
        asset->info.hasVideo || asset->info.kind == MediaKind::Video;
    const TrackType wanted = wantsVideo ? TrackType::Video : TrackType::Audio;

    QString trackId;
    for (const Track &track : m_document->project().tracks) {
        if (track.type == wanted) {
            trackId = track.id;
            break;
        }
    }
    if (trackId.isEmpty()) {
        setLastError(QStringLiteral("没有可用的轨道。"));
        return;
    }

    const QString clipId = m_timeline->addAssetToTimeline(
        assetId, trackId, m_timeline->positionUs());
    if (clipId.isEmpty())
        return; // TimelineController already reported the failure.
    clearError();
    refreshTimelinePreview();
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
    QVariantList styles;
    for (const SubtitleStyle &style : builtinSubtitleStyles()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), style.id);
        entry.insert(QStringLiteral("name"), style.name);
        styles.append(entry);
    }
    return styles;
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
    m_preview->openTimeline(project, duration);
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

void AppController::setUseProxy(bool enabled)
{
    if (m_useProxy == enabled)
        return;
    m_useProxy = enabled;
    emit useProxyChanged();
    // Re-opening the current asset is what actually switches the preview
    // between the proxy and the source; changing the flag alone does nothing.
    // In timeline mode, re-opening the last asset would jump out of the
    // timeline (e.g. right after double-clicking a media item to add it).
    if (!m_currentAssetId.isEmpty() && !m_preview->timelineMode())
        openAsset(m_currentAssetId);
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
    auto project = m_store->loadRecovery(&error);
    if (!project) {
        setLastError(error);
        return;
    }
    m_document->setProject(std::move(*project));
    m_currentAssetId.clear();
    // The restored document replaces whatever was previewed; stop and clear
    // the old preview so its clip does not keep playing behind the new project.
    m_preview->closeMedia();
    m_store->discardRecovery();
    setRecoveryAvailable(false);
    requestWaveforms();
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

    QString error;
    if (!m_document->save(path, &error)) {
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

void AppController::buildTimelineSelftestProject()
{
    const QString root = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR);
    importUrls({QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("h264.mp4"))),
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

    // h264.mp4 is two seconds long. Repeat the source on every video track so
    // all three layers stay active for the whole five-second perf window; a
    // gate that only sees three layers for two seconds could average a bad
    // three-layer frame rate away with fast empty frames.
    const auto addVideoClip = [this, &videoId](const QString &trackId,
                                               const QString &id, int index,
                                               int scaleMilli, int offsetX) {
        Clip clip;
        clip.id = id;
        clip.assetId = videoId;
        clip.sourceInUs = 0;
        clip.sourceOutUs = 2'000'000;
        clip.timelineStartUs = index * 2'000'000;
        clip.transform.scaleMilli = scaleMilli;
        clip.transform.offsetXPermille = offsetX;
        m_timeline->addClipForTest(trackId, clip);
    };

    if (videoTracks.size() >= 3) {
        const QString v1 = videoTracks.at(videoTracks.size() - 3);
        const QString v2 = videoTracks.at(videoTracks.size() - 2);
        const QString v3 = videoTracks.last();
        for (int i = 1; i < 3; ++i)
            addVideoClip(v1, QStringLiteral("perf-v1-%1").arg(i), i, 1000, 0);
        for (int i = 0; i < 3; ++i) {
            addVideoClip(v2, QStringLiteral("perf-v2-%1").arg(i), i, 500, 300);
            addVideoClip(v3, QStringLiteral("perf-v3-%1").arg(i), i, 500, -300);
        }
    }

    // A1 carries the original h264 audio (linked by addAssetToTimeline) plus
    // the tone fixture; A2 carries the BGM so both can be mixed independently.
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
    // scene graph actually presented over five seconds; compositor throughput,
    // decode and audio mixing all run for real.
    m_perfFrames = 0;
    selftestTimeline();
    if (m_document->project().assets.size() < 3) {
        qCritical("TIMELINE_PERF_MISSING_FIXTURES");
        QCoreApplication::exit(3);
        return;
    }

    m_perfClock.start();
    QTimer::singleShot(5000, this, [this] {
        const qint64 elapsedMs = m_perfClock.elapsed();
        m_perfClock.invalidate();
        const double seconds = elapsedMs > 0 ? elapsedMs / 1000.0 : 0.0;
        const double fps = seconds > 0.0 ? m_perfFrames / seconds : 0.0;
        qInfo("TIMELINE_FPS=%.1f FRAMES=%d ELAPSED_MS=%lld", fps, m_perfFrames,
              static_cast<long long>(elapsedMs));
        QCoreApplication::exit(fps >= 24.0 ? 0 : 3);
    });
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
    if (!m_store->saveRecovery(m_document->project(), &error))
        setLastError(error);
}

} // namespace VideoEditor
