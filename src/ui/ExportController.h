#pragma once

#include <functional>
#include <thread>

#include <QObject>
#include <QString>
#include <QVariantList>

#include "ExportEncoderCatalog.h"
#include "ExportProgress.h"
#include "ExportSettings.h"
#include "Removal.h"

class QThread;

namespace VideoEditor {

class ExportWorker;
struct Project;
class ProjectDocument;

// UI-facing owner of one export run. It freezes the project when an export
// starts, drives a fresh ExportWorker on its own QThread (or a synchronous
// runner in tests), forwards progress in Chinese and exposes validation and
// resume state as QML properties.
class ExportController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    Q_PROPERTY(QString presetId READ presetId WRITE setPresetId NOTIFY presetIdChanged)
    Q_PROPERTY(QString codec READ codec WRITE setCodec NOTIFY codecChanged)
    Q_PROPERTY(int width READ width WRITE setWidth NOTIFY widthChanged)
    Q_PROPERTY(int height READ height WRITE setHeight NOTIFY heightChanged)
    Q_PROPERTY(int fpsNum READ fpsNum WRITE setFpsNum NOTIFY fpsNumChanged)
    Q_PROPERTY(int fpsDen READ fpsDen WRITE setFpsDen NOTIFY fpsDenChanged)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY qualityChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps
                   NOTIFY bitrateKbpsChanged)
    Q_PROPERTY(bool hardwarePreferred READ hardwarePreferred WRITE setHardwarePreferred
                   NOTIFY hardwarePreferredChanged)
    Q_PROPERTY(bool burnSubtitles READ burnSubtitles WRITE setBurnSubtitles
                   NOTIFY burnSubtitlesChanged)
    Q_PROPERTY(bool writeSrt READ writeSrt WRITE setWriteSrt NOTIFY writeSrtChanged)
    Q_PROPERTY(QString outputPath READ outputPath WRITE setOutputPath
                   NOTIFY outputPathChanged)
    Q_PROPERTY(QString srtPath READ srtPath WRITE setSrtPath NOTIFY srtPathChanged)
    Q_PROPERTY(bool h265Available READ h265Available NOTIFY h265AvailableChanged)
    Q_PROPERTY(QString h265UnavailableReason READ h265UnavailableReason
                   NOTIFY h265UnavailableReasonChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool capabilitiesReady READ capabilitiesReady
                   NOTIFY capabilitiesReadyChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY canCancelChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY canResumeChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stageText READ stageText NOTIFY stageTextChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString validationError READ validationError
                   NOTIFY validationErrorChanged)
    Q_PROPERTY(QString validationSummary READ validationSummary
                   NOTIFY validationSummaryChanged)
    Q_PROPERTY(QString encoderName READ encoderName NOTIFY encoderNameChanged)
    Q_PROPERTY(bool removalRequired READ removalRequired
                   NOTIFY removalRequiredChanged)
    Q_PROPERTY(QString removalMessage READ removalMessage
                   NOTIFY removalMessageChanged)
    Q_PROPERTY(QString removalWarning READ removalWarning
                   NOTIFY removalWarningChanged)

public:
    using ProgressCallback = std::function<void(const ExportProgress &)>;
    using RemovalChecker =
        std::function<RemovalPreflightResult(const Project &)>;
    using SyncRunner = std::function<ExportResult(
        const Project &, const ExportSettings &, bool, const ProgressCallback &)>;

    explicit ExportController(ProjectDocument &document,
                              QString projectPath = QString(),
                              QObject *parent = nullptr);
    ~ExportController() override;

    QVariantList presets() const { return m_presetsVariant; }
    QString presetId() const { return m_presetId; }
    QString codec() const { return exportCodecToString(m_codec); }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int fpsNum() const { return m_fpsNum; }
    int fpsDen() const { return m_fpsDen; }
    int quality() const { return m_quality; }
    int bitrateKbps() const { return m_bitrateKbps; }
    bool hardwarePreferred() const { return m_hardwarePreferred; }
    bool burnSubtitles() const { return m_burnSubtitles; }
    bool writeSrt() const { return m_writeSrt; }
    QString outputPath() const { return m_outputPath; }
    QString srtPath() const { return m_srtPath; }
    bool h265Available() const { return m_h265Available; }
    QString h265UnavailableReason() const { return m_h265UnavailableReason; }
    bool running() const { return m_running; }
    // False until the asynchronous encoder probe (or a test-injected catalog)
    // has produced a capability table. Validation suppresses "no encoder"
    // errors while false so first-open never flashes a false failure.
    bool capabilitiesReady() const { return m_capabilitiesReady; }
    bool canCancel() const { return m_canCancel; }
    // Cached so property reads and unrelated setters never touch the
    // checkpoint file or hash the project.
    bool canResume() const { return m_canResume; }
    double progress() const { return m_progress; }
    QString stageText() const { return m_stageText; }
    QString statusText() const { return m_statusText; }
    QString error() const { return m_error; }
    QString validationError() const { return m_validationError; }
    QString validationSummary() const { return m_validationSummary; }
    QString encoderName() const { return m_encoderName; }
    bool removalRequired() const { return m_removalRequired; }
    QString removalMessage() const { return m_removalMessage; }
    // Non-blocking preflight notes (for example low-confidence tracking).
    QString removalWarning() const { return m_removalWarning; }

    Q_INVOKABLE void applyPreset(const QString &id);
    // Re-probes encoders for the current resolution unless a catalog was
    // injected by a test, then refreshes validation and resume state.
    Q_INVOKABLE void refreshCapabilities();
    Q_INVOKABLE void startExport();
    Q_INVOKABLE void cancelExport();
    Q_INVOKABLE void resumeExport();
    Q_INVOKABLE void setOutputPath(const QString &path);
    Q_INVOKABLE QString validate();

    void setPresetId(const QString &id);
    void setCodec(const QString &codec);
    void setWidth(int width);
    void setHeight(int height);
    void setFpsNum(int fpsNum);
    void setFpsDen(int fpsDen);
    void setQuality(int quality);
    void setBitrateKbps(int kbps);
    void setHardwarePreferred(bool preferred);
    void setBurnSubtitles(bool enabled);
    void setWriteSrt(bool enabled);
    void setSrtPath(const QString &path);

    // Injected by AppController. Not owned: the lambda captures the removal
    // cache and model registry, which outlive this controller.
    void setRemovalChecker(RemovalChecker checker);
    // Drops the cached preflight verdict so the next validate() re-runs the
    // checker. AppController calls this on document changes; typing in the
    // export dialog does not.
    void invalidateRemovalPreflight();
    // Repair cache root forwarded to the export worker.
    void setRemovalCacheRoot(const QString &root) { m_removalCacheRoot = root; }

    // Test injection: a deterministic catalog and a synchronous pipeline
    // stand-in so the controller can be tested without FFmpeg or a GPU.
    void setCatalogForTest(const EncoderCatalog &catalog);
    void setSyncRunnerForTest(SyncRunner runner);

signals:
    void presetIdChanged();
    void codecChanged();
    void widthChanged();
    void heightChanged();
    void fpsNumChanged();
    void fpsDenChanged();
    void qualityChanged();
    void bitrateKbpsChanged();
    void hardwarePreferredChanged();
    void burnSubtitlesChanged();
    void writeSrtChanged();
    void outputPathChanged();
    void srtPathChanged();
    void h265AvailableChanged();
    void h265UnavailableReasonChanged();
    void runningChanged();
    void capabilitiesReadyChanged();
    void canCancelChanged();
    void canResumeChanged();
    void progressChanged();
    void stageTextChanged();
    void statusTextChanged();
    void errorChanged();
    void validationErrorChanged();
    void validationSummaryChanged();
    void encoderNameChanged();
    void removalRequiredChanged();
    void removalMessageChanged();
    void removalWarningChanged();
    // Emitted instead of starting an export that would silently encode the
    // original in place of a required repair. Task 8 connects this to the
    // removal generation flow.
    void removalGenerationRequired(QString message);
    void finished(bool ok, const QString &message);

private:
    ExportSettings currentSettings() const;
    bool hasResumableCheckpoint() const;
    void refreshValidation();
    void refreshResumeState();
    void invalidateResumeState();
    void applyProgress(const ExportProgress &progress);
    void finishWith(const ExportResult &result);
    void handleWorkerFinished(const VideoEditor::ExportResult &result);
    void cleanupThread();
    void syncRemovalState(const RemovalPreflightResult *result);
    void setErrorText(const QString &message);
    void setStatusText(const QString &message);
    void setStageText(const QString &message);
    int derivedBitrateKbps() const;
    QString defaultOutputPath() const;
    QString displayNameForEncoder(const QString &encoderId) const;

    ProjectDocument &m_document;
    QString m_projectPath;
    QList<ExportPreset> m_presets;
    QVariantList m_presetsVariant;

    QString m_presetId;
    ExportVideoCodec m_codec = ExportVideoCodec::H264;
    int m_width = 1920;
    int m_height = 1080;
    int m_fpsNum = 30;
    int m_fpsDen = 1;
    int m_quality = int(ExportQuality::Medium);
    int m_bitrateKbps = 0;
    // True while the bitrate follows the quality tier. Editing the bitrate
    // field switches to an explicit target; choosing a quality switches back.
    bool m_qualityMode = false;
    bool m_hardwarePreferred = true;
    bool m_burnSubtitles = true;
    bool m_writeSrt = false;
    QString m_outputPath;
    QString m_srtPath;
    // False until the user types or browses for a path. The then-current
    // auto-generated path is refreshed on every dialog open so repeated
    // exports do not overwrite each other.
    bool m_outputPathEdited = false;

    bool m_h265Available = false;
    QString m_h265UnavailableReason;
    bool m_running = false;
    bool m_canCancel = false;
    bool m_canResume = false;
    double m_progress = 0.0;
    QString m_stageText;
    QString m_statusText;
    QString m_error;
    QString m_validationError;
    QString m_validationSummary;
    QString m_encoderName;
    bool m_removalRequired = false;
    QString m_removalMessage;
    QString m_removalWarning;
    // Preflight hashes model bundles and every cached patch. Cache the verdict
    // until the document changes; the export worker still runs it once.
    bool m_removalPreflightValid = false;
    RemovalPreflightResult m_removalPreflightResult;
    RemovalChecker m_removalChecker;
    QString m_removalCacheRoot;

    EncoderCatalog m_catalog;
    bool m_catalogInjected = false;
    bool m_capabilitiesReady = false;
    // A start requested before the encoder probe finished runs as soon as the
    // catalog arrives.
    bool m_startPending = false;
    bool m_resumeRequested = false;

    QThread *m_thread = nullptr;
    ExportWorker *m_worker = nullptr;
    SyncRunner m_syncRunner;

    // detectEncoders() opens every candidate encoder and must not block the
    // UI thread. Tests inject a catalog, which keeps this path synchronous.
    std::thread m_detectThread;
    bool m_detectRunning = false;
};

} // namespace VideoEditor
