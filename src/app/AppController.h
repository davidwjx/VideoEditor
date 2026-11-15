#pragma once

#include <memory>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include "AudioEngine.h"
#include "MediaListModel.h"
#include "ProjectDocument.h"
#include "ProjectStore.h"
#include "TimelineController.h"
#include "WaveformService.h"

namespace VideoEditor {

class PreviewController;
class ProxyService;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(MediaListModel *mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(VideoEditor::TimelineController *timeline READ timeline CONSTANT)
    Q_PROPERTY(VideoEditor::AudioEngine *audioEngine READ audioEngine CONSTANT)
    Q_PROPERTY(VideoEditor::WaveformService *waveforms READ waveforms CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryAvailableChanged)
    Q_PROPERTY(bool useProxy READ useProxy WRITE setUseProxy NOTIFY useProxyChanged)

public:
    // The optional sink factory lets tests replace the WASAPI device before
    // the audio engine starts; production callers keep the default.
    explicit AppController(QObject *parent = nullptr, QString recoveryPath = {},
                           AudioEngine::SinkFactory audioSinkFactory = {});
    ~AppController() override;

    MediaListModel *mediaModel() const { return m_mediaModel; }
    TimelineController *timeline() const { return m_timeline; }
    AudioEngine *audioEngine() const { return m_audio.get(); }
    WaveformService *waveforms() const { return m_waveforms; }
    QString lastError() const { return m_lastError; }
    bool canUndo() const;
    bool canRedo() const;
    bool recoveryAvailable() const { return m_recoveryAvailable; }
    bool useProxy() const { return m_useProxy; }
    void setUseProxy(bool enabled);

    Q_INVOKABLE void importUrls(const QList<QUrl> &urls);
    Q_INVOKABLE void openAsset(const QString &id);
    Q_INVOKABLE void addAssetToTimeline(const QString &assetId);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void restoreRecovery();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void saveProject(const QUrl &url);
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void selftest(const QString &path);
    // Moves both the edit playhead and the timeline preview to one position.
    Q_INVOKABLE void seekTimeline(qint64 positionUs);
    // Style presets as a list of {id, name} maps for the subtitle ComboBox.
    Q_INVOKABLE QVariantList subtitleStyles() const;

    // Re-opens the preview on the current project and resets the playhead.
    void refreshTimelinePreview();
    // Multi-track fixture projects used by the command-line self-tests.
    // --selftest-timeline exits after 30 presented frames in main.cpp;
    // --selftest-perf measures the presented-frame rate here.
    void selftestTimeline();
    void selftestPerf();

    // Called on clean shutdown: removes a stale autosave only when the open
    // document is clean and no startup recovery prompt is still pending.
    void discardRecoveryIfClean();

    ProjectDocument *document() const { return m_document; }
    ProjectStore *store() const { return m_store.get(); }
    PreviewController *preview() const { return m_preview; }
    ProxyService *proxyService() const { return m_proxyService; }

signals:
    void lastErrorChanged();
    void undoStateChanged();
    void recoveryAvailableChanged();
    void useProxyChanged();
    void projectChanged();
    void selftestFramePresented();
    void timelineFramePresented();
    // Number of video layers actually drawn into the timeline frame that was
    // just presented. Used by --selftest-perf to reject empty composites.
    void timelineFrameLayersPresented(int layerCount);

private:
    void setLastError(const QString &message);
    void refreshModel();
    void setRecoveryAvailable(bool available);
    void autosave();
    void requestWaveforms();
    void buildTimelineSelftestProject();

    ProjectDocument *m_document = nullptr;
    MediaListModel *m_mediaModel = nullptr;
    PreviewController *m_preview = nullptr;
    ProxyService *m_proxyService = nullptr;
    TimelineController *m_timeline = nullptr;
    WaveformService *m_waveforms = nullptr;
    std::unique_ptr<AudioEngine> m_audio;
    std::unique_ptr<ProjectStore> m_store;
    QTimer m_autosaveTimer;
    // Coalesces rapid edits into one preview/audio snapshot update so a drag
    // release or command burst does not queue a recomposition per command.
    QTimer m_timelineUpdateTimer;
    // --selftest-perf counts presented timeline frames while the clock runs,
    // plus the layer content of those frames.
    QElapsedTimer m_perfClock;
    int m_perfFrames = 0;
    int m_perfLayerSamples = 0;
    int m_perfFullLayerFrames = 0;
    int m_perfMinLayers = 0;
    int m_perfExpectedLayers = 0;
    QString m_lastError;
    QString m_currentAssetId;
    bool m_recoveryAvailable = false;
    bool m_useProxy = true;
    bool m_syncingPlayhead = false;
};

} // namespace VideoEditor
