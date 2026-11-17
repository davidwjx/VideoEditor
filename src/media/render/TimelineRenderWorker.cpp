#include "TimelineRenderWorker.h"

#include <algorithm>
#include <utility>

#include "SharedTextureRing.h"
#include "TimelineCompositor.h"

namespace VideoEditor {

TimelineRenderWorker::TimelineRenderWorker(QObject *parent)
    : QObject(parent)
{
}

TimelineRenderWorker::~TimelineRenderWorker()
{
    reset();
}

void TimelineRenderWorker::reset()
{
    // The compositor owns the device and the clip decoders; it must die before
    // the ring's shared textures.
    m_compositor.reset();
    m_ring.reset();
    m_valid = false;
}

void TimelineRenderWorker::close()
{
    reset();
    // Release the preview-only repair source; the controller re-sends it when
    // the timeline is opened again.
    m_liveRepairProvider.reset();
    m_project = Project{};
    m_projectPath.clear();
    m_generation = 0;
}

void TimelineRenderWorker::setProject(VideoEditor::Project project,
                                      QString projectPath,
                                      quint64 generation, bool useProxy)
{
    const int width = project.timeline.width > 0 ? project.timeline.width : 1920;
    const int height =
        project.timeline.height > 0 ? project.timeline.height : 1080;
    // Project edits usually keep the output resolution: keep the ring and the
    // compositor's device, and let open() drop only the clip decoders and the
    // subtitle cache.
    const bool reusableRing = m_valid && m_ring && m_compositor
                              && m_width == width && m_height == height;

    m_project = std::move(project);
    m_projectPath = std::move(projectPath);
    m_generation = generation;
    m_useProxy = useProxy;
    if (!m_compositor) {
        m_compositor = std::make_unique<TimelineCompositor>();
        m_compositor->setWarningCallback(
            [this](const QString &message) { emit error(message); });
    }
    QString error;
    if (!m_compositor->open(m_project, width, height, useProxy, &error)) {
        emit this->error(error);
        reset();
        return;
    }
    // open() clears repair sources but retains the compare settings; reapply
    // both so a compositor created after close() still honours the user's
    // current mode and the app's cache location.
    m_compositor->setRemovalCacheRoot(m_removalCacheRoot);
    m_compositor->setRemovalCompareMode(
        static_cast<RemovalCompareMode>(m_removalCompare), m_splitPermille);
    m_compositor->setLiveRepairProvider(m_liveRepairProvider);
    m_width = m_compositor->width();
    m_height = m_compositor->height();
    if (!reusableRing) {
        m_ring = std::make_unique<SharedTextureRing>(*m_compositor->context());
        if (!m_ring->create(m_width, m_height, SharedTextureRing::kSlotCount,
                            &error)) {
            emit this->error(error);
            reset();
            return;
        }
    }
    m_valid = true;
}

void TimelineRenderWorker::setRemovalCompare(int mode, int splitPermille)
{
    m_removalCompare = std::clamp(mode, 0, 2);
    m_splitPermille = std::clamp(splitPermille, 0, 1000);
    if (m_compositor) {
        m_compositor->setRemovalCompareMode(
            static_cast<RemovalCompareMode>(m_removalCompare),
            m_splitPermille);
    }
}

void TimelineRenderWorker::setLiveRepairProvider(
    VideoEditor::LiveRepairProviderPtr provider)
{
    m_liveRepairProvider = std::move(provider);
    if (m_compositor)
        m_compositor->setLiveRepairProvider(m_liveRepairProvider);
}

void TimelineRenderWorker::setRemovalCacheRoot(QString root)
{
    if (m_removalCacheRoot == root)
        return;
    m_removalCacheRoot = std::move(root);
    if (m_compositor)
        m_compositor->setRemovalCacheRoot(m_removalCacheRoot);
}

void TimelineRenderWorker::renderFrame(qint64 timelineUs, quint64 generation)
{
    const int64_t requestedUs = std::max<int64_t>(0, timelineUs);
    if (generation != m_generation)
        return;
    if (!m_valid || !m_ring || !m_compositor || !m_compositor->isValid()) {
        emit frameSkipped(requestedUs, generation);
        return;
    }

    const int slot = m_ring->nextWriteIndex();
    IDXGIKeyedMutex *mutex = m_ring->mutex(slot);
    if (!mutex || mutex->AcquireSync(0, 50) != S_OK) {
        // The scene graph still owns this slot. Tell the controller so its
        // single-in-flight gate is released instead of stalling.
        emit frameSkipped(requestedUs, generation);
        return;
    }

    QString error;
    const int drawnLayers = m_compositor->drawFrame(
        requestedUs, m_ring->rtv(slot), true, &error);
    if (!error.isEmpty()) {
        // Nothing was drawn into this slot: hand it back to the producer side
        // so a later request can reuse it instead of presenting a stale frame.
        mutex->ReleaseSync(0);
        emit this->error(error);
        emit frameSkipped(requestedUs, generation);
        return;
    }

    mutex->ReleaseSync(1);
    m_ring->advanceWriteIndex();

    emit frameReady(slot, QSize(m_width, m_height), requestedUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)),
                    drawnLayers, m_generation);
}

} // namespace VideoEditor
