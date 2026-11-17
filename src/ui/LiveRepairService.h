#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>

#include "LiveRepairProvider.h"
#include "Removal.h"
#include "TimeBase.h"

class QThread;

namespace VideoEditor {

class FrameReader;
class InpaintPipeline;
class Inpainter;

// Everything the live preview worker needs to repair the selected layer
// without touching the on-disk cache. The UI resolves the model record and the
// tracking cache directory; the service only decodes, inpaints and publishes
// patches in memory.
struct LiveRepairSpec {
    QString assetPath;
    QString cacheDir;      // <removal cache root>/<cacheKey>
    QString layerId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    Rational fps{30, 1};
    QSize displaySize{1920, 1080};
    RemovalParameters parameters;
    int featherPx = 4;
    QString inpainterModelPath;
    RepairMode mode = RepairMode::Fast;
};

// Owns a worker thread that repairs the frame under the current playhead first
// and then a bounded +-2 s window. It never writes to disk: the results stay in
// an in-memory LRU cache that TimelineCompositor reads through the
// LiveRepairProvider interface. Missing tracking masks stop the worker with a
// Chinese error because the user must run tracking first.
class LiveRepairService : public QObject, public LiveRepairProvider {
    Q_OBJECT

public:
    explicit LiveRepairService(QObject *parent = nullptr);
    ~LiveRepairService() override;

    // Validates the spec, opens the asset reader and prepares the inpaint
    // pipeline. Must be called while stopped; stop() joins any previous run.
    bool configure(const LiveRepairSpec &spec, QString *error = nullptr);
    void start();
    void stop();
    bool isRunning() const;
    bool isConfigured() const { return m_configured; }

    // Safe from the UI thread. Wakes the worker and asks it to repair this
    // source instant before the surrounding window.
    void setTarget(int64_t sourceUs);

    // TimelineCompositor calls this from its render thread. Returns false when
    // this layer is not configured, the instant is outside the layer range or
    // the frame has not been repaired yet (including frames with an empty
    // mask, where the original frame is the correct result).
    bool patchFor(const QString &layerId, int64_t sourceUs,
                  LiveRepairPatch *patch) const override;

    // Test hooks: inject a deterministic Inpainter and tighten the cache. Both
    // are only meaningful before start().
    void setInpainterForTest(std::unique_ptr<Inpainter> inpainter);
    void setCacheLimitForTest(int limit);
    int cachedPatchCountForTest() const;
    int frameCountForTest() const;
    QString backendForTest() const;

signals:
    // Emitted once the worker thread has started; patches follow per frame.
    void ready();
    void frameReady(int frame);
    // Emitted only when the repaired frame is still the current target, so the
    // UI can distinguish "the playhead frame is ready" from prefetch noise.
    void targetReady(int frame);
    // "dml" / "cpu" once the model session exists. Lets the UI say "CPU，较慢"
    // instead of promising GPU inference.
    void backendChanged(QString backend);
    void error(QString message);

private:
    static constexpr int kDefaultCacheLimit = 240;
    static constexpr int kWindowSeconds = 2;

    void run();
    bool processFrame(int frame, bool targetFrame = false);
    void publishBackend();
    int frameForSourceUs(int64_t sourceUs) const;
    void storePatch(int frame, const LiveRepairPatch &patch);
    void clearWorkingState();

    mutable std::mutex m_stateMutex;
    // Guards only the condition variable; never held while decoding.
    std::mutex m_waitMutex;
    std::condition_variable m_waitCv;
    std::atomic<bool> m_stop{false};
    std::atomic<int64_t> m_targetUs{0};
    std::atomic<quint64> m_targetSerial{0};

    LiveRepairSpec m_spec;
    int m_frameCount = 0;
    int m_cacheLimit = kDefaultCacheLimit;
    QHash<int, LiveRepairPatch> m_patches;
    mutable QList<int> m_lru; // most recently used first
    QSet<int> m_emptyFrames;  // the original frame is already correct
    QString m_backend;        // "dml"/"cpu", guarded by m_stateMutex

    // Created by configure() and used only by the worker thread afterwards.
    std::unique_ptr<FrameReader> m_reader;
    std::unique_ptr<InpaintPipeline> m_pipeline;
    std::unique_ptr<Inpainter> m_testInpainter;

    QThread *m_thread = nullptr;
    bool m_configured = false;
};

} // namespace VideoEditor
