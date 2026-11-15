#include "ExportController.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <objbase.h>

#include "ExportCheckpoint.h"
#include "ExportWorker.h"
#include "ProjectDocument.h"

namespace VideoEditor {

namespace {

QString stageLabel(ExportStage stage)
{
    switch (stage) {
    case ExportStage::Preparing:
        return QStringLiteral("准备中");
    case ExportStage::EncodingAudio:
        return QStringLiteral("音频编码");
    case ExportStage::Rendering:
        return QStringLiteral("渲染中");
    case ExportStage::Finalizing:
        return QStringLiteral("封装中");
    case ExportStage::Validating:
        return QStringLiteral("校验中");
    case ExportStage::Done:
        return QStringLiteral("完成");
    case ExportStage::Failed:
        return QStringLiteral("失败");
    case ExportStage::Canceled:
        return QStringLiteral("已取消");
    }
    return QString();
}

QString formatEtaText(qint64 milliseconds)
{
    if (milliseconds < 0)
        return QString();
    const qint64 seconds = milliseconds / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 rest = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(rest, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(rest, 2, 10, QLatin1Char('0'));
}

} // namespace

ExportController::ExportController(ProjectDocument &document,
                                   QString projectPath, QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_projectPath(std::move(projectPath))
{
    qRegisterMetaType<VideoEditor::Project>();
    qRegisterMetaType<VideoEditor::ExportSettings>();
    qRegisterMetaType<VideoEditor::ExportProgress>();
    qRegisterMetaType<VideoEditor::ExportResult>();

    m_presets = builtinExportPresets();
    for (const ExportPreset &preset : m_presets) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), preset.id);
        entry.insert(QStringLiteral("name"), preset.name);
        entry.insert(QStringLiteral("codec"), exportCodecToString(preset.codec));
        // The "match" preset has no fixed size; show the timeline's target so
        // the generic preset list does not display 0x0.
        entry.insert(QStringLiteral("width"),
                     preset.matchTimeline ? document.project().timeline.width
                                          : preset.width);
        entry.insert(QStringLiteral("height"),
                     preset.matchTimeline ? document.project().timeline.height
                                          : preset.height);
        entry.insert(QStringLiteral("fpsNum"),
                     preset.matchTimeline ? document.project().timeline.fps.num
                                          : preset.fps.num);
        entry.insert(QStringLiteral("fpsDen"),
                     preset.matchTimeline ? document.project().timeline.fps.den
                                          : preset.fps.den);
        m_presetsVariant.append(entry);
    }

    // Empty until refreshCapabilities() probes the real encoders off the UI
    // thread. Never seed production with the test-only software catalog.
    m_catalog = EncoderCatalog{};
    m_outputPath = defaultOutputPath();
    applyPreset(QStringLiteral("match"));

    // The "match" preset follows the project's resolution/frame rate, so a
    // project switch must re-derive it instead of exporting the old geometry.
    connect(&m_document, &ProjectDocument::projectReplaced, this, [this] {
        if (!m_presetId.isEmpty())
            applyPreset(m_presetId);
        else
            refreshValidation();
    });
}

ExportController::~ExportController()
{
    if (m_worker)
        m_worker->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
        m_worker = nullptr;
    }
    // detectEncoders() can still be running; join before the object dies so
    // its completion callback can never touch a dangling controller.
    if (m_detectThread.joinable())
        m_detectThread.join();
}

QString ExportController::defaultOutputPath() const
{
    QString dir;
    if (!m_projectPath.isEmpty())
        dir = QFileInfo(m_projectPath).absolutePath();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QDir::homePath();
    const QString name = QStringLiteral("导出_%1.mp4")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd_HHmmss")));
    return QDir(dir).filePath(name);
}

ExportSettings ExportController::currentSettings() const
{
    ExportSettings settings;
    settings.codec = m_codec;
    settings.width = m_width;
    settings.height = m_height;
    settings.fps = Rational{m_fpsNum, m_fpsDen};
    settings.rateControl = m_qualityMode ? ExportRateControl::Quality
                                         : ExportRateControl::Bitrate;
    settings.quality = ExportQuality(m_quality);
    // In quality mode the encoder derives the bitrate from codec/geometry, so
    // the displayed bitrate must not pin it.
    settings.bitrateKbps = m_qualityMode ? 0 : m_bitrateKbps;
    settings.preferHardware = m_hardwarePreferred;
    settings.burnSubtitles = m_burnSubtitles;
    settings.writeSrt = m_writeSrt;
    settings.outputPath = m_outputPath;
    if (!m_srtPath.isEmpty())
        settings.srtPath = m_srtPath;
    else if (!m_outputPath.isEmpty())
        settings.srtPath = defaultSrtPathFor(m_outputPath);
    return settings;
}

int ExportController::derivedBitrateKbps() const
{
    ExportSettings settings = currentSettings();
    settings.rateControl = ExportRateControl::Quality;
    settings.bitrateKbps = 0;
    return effectiveBitrateKbps(settings);
}

void ExportController::applyPreset(const QString &id)
{
    const auto it = std::find_if(
        m_presets.cbegin(), m_presets.cend(),
        [&id](const ExportPreset &preset) { return preset.id == id; });
    if (it == m_presets.cend())
        return;

    const ExportSettings derived =
        settingsFromPreset(*it, m_document.project().timeline);
    m_presetId = id;
    m_codec = derived.codec;
    m_width = derived.width;
    m_height = derived.height;
    m_fpsNum = derived.fps.num;
    m_fpsDen = derived.fps.den;
    m_quality = int(derived.quality);
    m_qualityMode = derived.rateControl == ExportRateControl::Quality;
    m_bitrateKbps = derived.bitrateKbps > 0 ? derived.bitrateKbps
                                            : effectiveBitrateKbps(derived);

    emit presetIdChanged();
    emit codecChanged();
    emit widthChanged();
    emit heightChanged();
    emit fpsNumChanged();
    emit fpsDenChanged();
    emit qualityChanged();
    emit bitrateKbpsChanged();
    invalidateResumeState();
    refreshValidation();
}

QString ExportController::validate()
{
    const auto finishValidation = [this](const QString &error) {
        // Any non-removal validation failure means the removal check was not
        // reached; clear a stale "regenerate first" prompt.
        syncRemovalState(nullptr);
        return error;
    };

    const ExportSettings settings = currentSettings();
    const QString error =
        validateExportSettings(settings, m_document.project().timeline);
    if (!error.isEmpty())
        return finishValidation(error);

    // Finalization replaces the destination, so an output that is also an
    // imported asset (or the project file) would delete source media. Reject
    // before any part or sidecar is written.
    const QString projectPath =
        m_projectPath.isEmpty() ? m_document.filePath() : m_projectPath;
    const QString destinationError = validateExportDestinations(
        settings, m_document.project(), projectPath);
    if (!destinationError.isEmpty())
        return finishValidation(destinationError);

    // Ask the catalog for the encoder the pipeline would actually pick. A
    // bare hasAny() also counts rows that the selection gate never uses (for
    // example OpenH264 without the explicit opt-in).
    QString reason;
    const QString encoderId = m_catalog.selectEncoder(
        m_codec, m_hardwarePreferred, nullptr, &reason);
    if (encoderId.isEmpty()) {
        // The first dialog open probes encoders on a worker thread. Until that
        // catalog arrives it is empty; reporting "no encoder" then would
        // flash a false error over an otherwise valid configuration.
        if (!m_capabilitiesReady)
            return finishValidation(QString());
        if (m_codec == ExportVideoCodec::H265) {
            return finishValidation(
                reason.isEmpty()
                    ? QStringLiteral(
                          "当前系统没有可用的 H.265/HEVC 硬件编码器。")
                    : reason);
        }
        if (m_codec == ExportVideoCodec::H264) {
            return finishValidation(
                reason.isEmpty()
                    ? QStringLiteral("当前系统没有可用的 H.264 编码器。")
                    : reason);
        }
        return finishValidation(
            reason.isEmpty() ? QStringLiteral("没有可用的视频编码器。")
                             : reason);
    }

    // AI removal is checked only after the ordinary settings/encoder checks:
    // a missing model or cache must never mask a more fundamental export
    // problem. The checker owns the cache/model types; the controller only
    // forwards the verdict.
    if (m_removalChecker) {
        const RemovalPreflightResult result =
            m_removalChecker(m_document.project());
        syncRemovalState(&result);
        if (!result.ok) {
            return result.message.isEmpty()
                       ? QStringLiteral(
                             "AI 修复结果缺失或已过期。请先生成修复，再导出。")
                       : result.message;
        }
        return {};
    }
    syncRemovalState(nullptr);
    return {};
}

bool ExportController::hasResumableCheckpoint() const
{
    if (m_running || m_outputPath.isEmpty())
        return false;
    // A stat on an unreachable network share can block for many seconds; the
    // resume hint is a convenience, never worth freezing the dialog.
    if (m_outputPath.startsWith(QStringLiteral("//"))
        || m_outputPath.startsWith(QStringLiteral("\\\\"))) {
        return false;
    }
    const QString checkpointPath =
        QDir(partsDirectoryFor(m_outputPath))
            .filePath(QStringLiteral("checkpoint.json"));
    if (!QFileInfo::exists(checkpointPath))
        return false;
    const auto checkpoint = ExportCheckpoint::load(checkpointPath);
    if (!checkpoint || checkpoint->completedSegments <= 0)
        return false;
    return checkpoint->projectHash
           == exportProjectHash(m_document.project(), currentSettings());
}

void ExportController::refreshResumeState()
{
    const bool resume = hasResumableCheckpoint();
    if (resume != m_canResume) {
        m_canResume = resume;
        emit canResumeChanged();
    }
}

void ExportController::invalidateResumeState()
{
    // Every export setting participates in the checkpoint hash, so a setting
    // change can never resume the old parts. Avoid hashing on every keystroke.
    if (!m_canResume)
        return;
    m_canResume = false;
    emit canResumeChanged();
}

void ExportController::refreshValidation()
{
    const QString error = validate();
    if (error != m_validationError) {
        m_validationError = error;
        emit validationErrorChanged();
    }

    // H.265 is hardware-only; keep the gate and its explanation in sync with
    // the current codec/resolution probe.
    const bool available = m_catalog.hasHardware(ExportVideoCodec::H265);
    if (available != m_h265Available) {
        m_h265Available = available;
        emit h265AvailableChanged();
    }
    QString reason;
    if (!m_capabilitiesReady) {
        reason = QStringLiteral("正在检测硬件编码器…");
    } else if (!available) {
        m_catalog.selectEncoder(ExportVideoCodec::H265, true, nullptr, &reason);
        if (reason.isEmpty()) {
            reason = QStringLiteral(
                "当前系统没有可用的 H.265/HEVC 硬件编码器。"
                "H.265 导出仅支持硬件编码，请改用 H.264。");
        }
    }
    if (reason != m_h265UnavailableReason) {
        m_h265UnavailableReason = reason;
        emit h265UnavailableReasonChanged();
    }

    const QString encoderId = m_catalog.selectEncoder(
        m_codec, m_hardwarePreferred, nullptr, nullptr);
    const QString encoderName =
        encoderId.isEmpty() ? QString() : displayNameForEncoder(encoderId);
    if (encoderName != m_encoderName) {
        m_encoderName = encoderName;
        emit encoderNameChanged();
    }
}

QString ExportController::displayNameForEncoder(const QString &encoderKey) const
{
    // Finished exports report EncoderDescriptor::key() ("h264_mf@sw"); accept
    // that, a bare FFmpeg id, or an already-friendly name.
    if (const EncoderDescriptor *encoder = m_catalog.findByKey(encoderKey))
        return encoder->displayName;
    for (const EncoderDescriptor &encoder : m_catalog.encoders) {
        if (encoder.id == encoderKey && encoder.available)
            return encoder.displayName;
    }
    return encoderKey;
}

void ExportController::refreshCapabilities()
{
    refreshResumeState();

    // Opened from the dialog: clear a finished run's bar and give the default
    // file name a fresh timestamp unless the user chose a path or the old path
    // still has resumable parts.
    if (!m_running) {
        if (m_progress != 0.0) {
            m_progress = 0.0;
            emit progressChanged();
        }
        if (!m_outputPathEdited && !m_canResume) {
            const QString suggestion = defaultOutputPath();
            if (suggestion != m_outputPath) {
                m_outputPath = suggestion;
                emit outputPathChanged();
            }
        }
    }

    if (m_catalogInjected) {
        if (!m_capabilitiesReady) {
            m_capabilitiesReady = true;
            emit capabilitiesReadyChanged();
        }
        refreshValidation();
        return;
    }
    if (m_detectRunning)
        return;

    // Encoder probing opens every candidate; keep it off the UI thread. The
    // completion event is posted back to this object, so a destroyed
    // controller never receives a stale catalog (the destructor joins).
    if (m_detectThread.joinable())
        m_detectThread.join();
    m_detectRunning = true;
    const int width = m_width;
    const int height = m_height;
    const int fpsNum = m_fpsNum;
    const int fpsDen = m_fpsDen;
    m_detectThread = std::thread([this, width, height, fpsNum, fpsDen] {
        // Media Foundation / AMF probing needs COM on this worker thread;
        // the UI thread's Qt-initialized apartment is not available here.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        EncoderCatalog catalog = detectEncoders(
            width, height, Rational{fpsNum, fpsDen});
        if (SUCCEEDED(com))
            CoUninitialize();
        QMetaObject::invokeMethod(
            this,
            [this, catalog] {
                m_catalog = catalog;
                m_detectRunning = false;
                if (!m_capabilitiesReady) {
                    m_capabilitiesReady = true;
                    emit capabilitiesReadyChanged();
                }
                refreshValidation();
                refreshResumeState();
                // A start requested while the probe was running now has the
                // catalog it was waiting for.
                if (m_startPending) {
                    m_startPending = false;
                    startExport();
                }
            },
            Qt::QueuedConnection);
    });
}

void ExportController::startExport()
{
    if (m_running)
        return;

    // Validation intentionally stays silent while the first async probe runs.
    // Defer this start instead of handing an empty catalog to the worker, then
    // run it from the probe completion.
    if (!m_capabilitiesReady) {
        if (!m_detectRunning)
            refreshCapabilities();
        m_startPending = true;
        setErrorText(QString());
        setStageText(stageLabel(ExportStage::Preparing));
        setStatusText(QStringLiteral("正在检测编码器，请稍候…"));
        return;
    }

    const QString errorText = validate();
    if (!errorText.isEmpty()) {
        // A resume request must not survive a rejected start, or the next
        // plain start would unexpectedly continue the old export.
        m_resumeRequested = false;
        setErrorText(errorText);
        // The removal check failed: route the user to generation instead of
        // leaving them with a dead export button.
        if (m_removalRequired)
            emit removalGenerationRequired(m_removalMessage.isEmpty()
                                              ? errorText
                                              : m_removalMessage);
        return;
    }

    // Only the explicit "继续上次导出" action resumes; a plain start on the
    // same output intentionally begins a new export.
    const bool resume = m_resumeRequested;
    m_resumeRequested = false;

    setErrorText(QString());
    m_progress = 0.0;
    emit progressChanged();
    setStageText(stageLabel(ExportStage::Preparing));
    setStatusText(QStringLiteral("正在准备导出…"));
    if (!m_validationSummary.isEmpty()) {
        m_validationSummary.clear();
        emit validationSummaryChanged();
    }

    // Freeze the project now: later edits must not affect this export.
    const Project frozen = m_document.project();
    const ExportSettings settings = currentSettings();

    m_running = true;
    emit runningChanged();
    m_canCancel = true;
    emit canCancelChanged();

    if (m_syncRunner) {
        const ExportResult result =
            m_syncRunner(frozen, settings, resume,
                         [this](const ExportProgress &progress) {
                             applyProgress(progress);
                         });
        finishWith(result);
        return;
    }

    // One worker per run: a canceled worker keeps its cancel flag forever.
    m_thread = new QThread;
    m_worker = new ExportWorker;
    m_worker->setCatalog(m_catalog);
    m_worker->setRemovalChecker(m_removalChecker);
    m_worker->setRemovalCacheRoot(m_removalCacheRoot);
    // The pipeline needs the project path to reject an output that would
    // overwrite the project file itself.
    m_worker->setProjectPath(m_projectPath.isEmpty() ? m_document.filePath()
                                                    : m_projectPath);
    m_worker->moveToThread(m_thread);
    connect(m_worker, &ExportWorker::progress, this,
            &ExportController::applyProgress, Qt::QueuedConnection);
    connect(m_worker, &ExportWorker::finished, this,
            &ExportController::handleWorkerFinished, Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
    QMetaObject::invokeMethod(
        m_worker, "run", Qt::QueuedConnection,
        Q_ARG(VideoEditor::Project, frozen),
        Q_ARG(VideoEditor::ExportSettings, settings), Q_ARG(bool, resume));
}

void ExportController::cancelExport()
{
    if (!m_running) {
        // A start queued behind the encoder probe can still be abandoned.
        m_startPending = false;
        m_resumeRequested = false;
        return;
    }
    if (m_worker)
        m_worker->cancel();
    setStatusText(QStringLiteral("正在取消…已完成的片段会保留，可稍后继续。"));
}

void ExportController::resumeExport()
{
    if (m_running)
        return;
    m_resumeRequested = true;
    startExport();
}

void ExportController::applyProgress(const ExportProgress &progress)
{
    const double value = std::clamp(progress.percent / 100.0, 0.0, 1.0);
    if (std::abs(value - m_progress) > 1e-9) {
        m_progress = value;
        emit progressChanged();
    }

    const QString stage = stageLabel(progress.stage);
    setStageText(stage);

    QString status = QStringLiteral("%1  %2/%3 帧  第 %4/%5 段")
                         .arg(stage)
                         .arg(progress.completedFrames)
                         .arg(progress.totalFrames)
                         .arg(progress.completedSegments)
                         .arg(progress.totalSegments);
    if (progress.etaMs >= 0)
        status += QStringLiteral("  剩余 %1").arg(formatEtaText(progress.etaMs));
    setStatusText(status);
}

void ExportController::finishWith(const ExportResult &result)
{
    if (result.ok) {
        m_progress = 1.0;
        emit progressChanged();
        setStageText(stageLabel(ExportStage::Done));
        const QString path =
            result.outputPath.isEmpty() ? m_outputPath : result.outputPath;
        setStatusText(QStringLiteral("导出完成：%1").arg(path));
        QString summary = QStringLiteral("文件已通过导出后校验。");
        if (!result.warning.isEmpty())
            summary = result.warning;
        if (summary != m_validationSummary) {
            m_validationSummary = summary;
            emit validationSummaryChanged();
        }
        setErrorText(QString());
    } else if (result.canceled) {
        setStageText(stageLabel(ExportStage::Canceled));
        setStatusText(result.resumable
                          ? QStringLiteral("已取消。已完成的分段会保留，可继续上次导出。")
                          : QStringLiteral("导出已取消。"));
        setErrorText(QString());
        if (!m_validationSummary.isEmpty()) {
            m_validationSummary.clear();
            emit validationSummaryChanged();
        }
    } else {
        setStageText(stageLabel(ExportStage::Failed));
        const QString message =
            result.error.isEmpty() ? QStringLiteral("导出失败。") : result.error;
        setStatusText(message);
        setErrorText(message);
        if (!m_validationSummary.isEmpty()) {
            m_validationSummary.clear();
            emit validationSummaryChanged();
        }
    }

    if (!result.encoderId.isEmpty()) {
        const QString name = displayNameForEncoder(result.encoderId);
        if (!name.isEmpty() && name != m_encoderName) {
            m_encoderName = name;
            emit encoderNameChanged();
        }
    }

    m_running = false;
    emit runningChanged();
    m_canCancel = false;
    emit canCancelChanged();

    cleanupThread();
    refreshValidation();
    refreshResumeState();

    const QString message =
        result.ok ? m_statusText
                  : (result.error.isEmpty() ? m_statusText : result.error);
    emit finished(result.ok, message);
}

void ExportController::handleWorkerFinished(
    const VideoEditor::ExportResult &result)
{
    finishWith(result);
}

void ExportController::cleanupThread()
{
    if (!m_thread) {
        m_worker = nullptr;
        return;
    }
    QThread *thread = m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
    thread->quit();
    thread->wait();
    thread->deleteLater();
}

void ExportController::setRemovalChecker(RemovalChecker checker)
{
    m_removalChecker = std::move(checker);
    refreshValidation();
}

void ExportController::syncRemovalState(const RemovalPreflightResult *result)
{
    const bool required = result && !result->ok;
    const QString message =
        required ? (result->message.isEmpty()
                        ? QStringLiteral(
                              "AI 修复结果缺失或已过期。请先生成修复，再导出。")
                        : result->message)
                 : QString();
    if (required != m_removalRequired) {
        m_removalRequired = required;
        emit removalRequiredChanged();
    }
    if (message != m_removalMessage) {
        m_removalMessage = message;
        emit removalMessageChanged();
    }
}

void ExportController::setErrorText(const QString &message)
{
    if (message == m_error)
        return;
    m_error = message;
    emit errorChanged();
}

void ExportController::setStatusText(const QString &message)
{
    if (message == m_statusText)
        return;
    m_statusText = message;
    emit statusTextChanged();
}

void ExportController::setStageText(const QString &message)
{
    if (message == m_stageText)
        return;
    m_stageText = message;
    emit stageTextChanged();
}

void ExportController::setPresetId(const QString &id)
{
    if (id == m_presetId)
        return;
    // Writing presetId behaves like choosing that preset; unknown ids are
    // ignored so the visual preset and the encoded settings never diverge.
    const auto it = std::find_if(
        m_presets.cbegin(), m_presets.cend(),
        [&id](const ExportPreset &preset) { return preset.id == id; });
    if (it != m_presets.cend())
        applyPreset(id);
}

void ExportController::setCodec(const QString &codec)
{
    ExportVideoCodec parsed = ExportVideoCodec::H264;
    if (!exportCodecFromString(codec, &parsed) || parsed == m_codec)
        return;
    m_codec = parsed;
    emit codecChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setWidth(int width)
{
    if (width == m_width)
        return;
    m_width = width;
    emit widthChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setHeight(int height)
{
    if (height == m_height)
        return;
    m_height = height;
    emit heightChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setFpsNum(int fpsNum)
{
    if (fpsNum == m_fpsNum)
        return;
    m_fpsNum = fpsNum;
    emit fpsNumChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setFpsDen(int fpsDen)
{
    if (fpsDen == m_fpsDen)
        return;
    m_fpsDen = fpsDen;
    emit fpsDenChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setQuality(int quality)
{
    const int clamped = std::clamp(quality, int(ExportQuality::Low),
                                   int(ExportQuality::High));
    if (clamped == m_quality && m_qualityMode)
        return;
    m_quality = clamped;
    m_qualityMode = true;
    const int derived = derivedBitrateKbps();
    if (derived != m_bitrateKbps) {
        m_bitrateKbps = derived;
        emit bitrateKbpsChanged();
    }
    emit qualityChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setBitrateKbps(int kbps)
{
    if (kbps == m_bitrateKbps && !m_qualityMode)
        return;
    m_bitrateKbps = kbps;
    m_qualityMode = false;
    emit bitrateKbpsChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setHardwarePreferred(bool preferred)
{
    if (preferred == m_hardwarePreferred)
        return;
    m_hardwarePreferred = preferred;
    emit hardwarePreferredChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setBurnSubtitles(bool enabled)
{
    if (enabled == m_burnSubtitles)
        return;
    m_burnSubtitles = enabled;
    emit burnSubtitlesChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setWriteSrt(bool enabled)
{
    if (enabled == m_writeSrt)
        return;
    m_writeSrt = enabled;
    emit writeSrtChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setOutputPath(const QString &path)
{
    m_outputPathEdited = true;
    QString local = path.trimmed();
    if (local.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        const QUrl url(local);
        if (url.isLocalFile()) {
            local = url.toLocalFile();
        } else if (!url.host().isEmpty()) {
            // UNC share (file://server/share/file); keep the authority.
            local = QStringLiteral("//%1%2").arg(url.host(), url.path());
        } else {
            local = url.path();
        }
    }
    const QString normalized = normalizedOutputPath(local);
    if (normalized == m_outputPath)
        return;
    m_outputPath = normalized;
    emit outputPathChanged();
    // Resume detection touches the file system; defer it to the next dialog
    // open / finished run instead of doing I/O on every path edit.
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setSrtPath(const QString &path)
{
    const QString normalized = path.trimmed();
    if (normalized == m_srtPath)
        return;
    m_srtPath = normalized;
    emit srtPathChanged();
    invalidateResumeState();
    refreshValidation();
}

void ExportController::setCatalogForTest(const EncoderCatalog &catalog)
{
    m_catalog = catalog;
    m_catalogInjected = true;
    if (!m_capabilitiesReady) {
        m_capabilitiesReady = true;
        emit capabilitiesReadyChanged();
    }
}

void ExportController::setSyncRunnerForTest(SyncRunner runner)
{
    m_syncRunner = std::move(runner);
}

} // namespace VideoEditor
