#include "TimelineRenderWorker.h"

#include <algorithm>
#include <utility>

#include "ClipVideoSource.h"
#include "D3D11Context.h"
#include "LayerCompositor.h"
#include "LayerGeometry.h"
#include "SharedTextureRing.h"
#include "SubtitleRasterizer.h"

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
    // Sources own D3D11 decoder assets and must die before the device.
    m_sources.clear();
    m_failedClips.clear();
    m_reportedClips.clear();
    m_compositor.reset();
    m_ring.reset();
    m_context.reset();
    m_subtitleKey.clear();
    m_subtitleImage = QImage();
    m_valid = false;
}

void TimelineRenderWorker::close()
{
    reset();
    m_project = Project{};
    m_projectPath.clear();
    m_generation = 0;
}

void TimelineRenderWorker::setProject(VideoEditor::Project project,
                                      QString projectPath, quint64 generation)
{
    reset();
    m_project = std::move(project);
    m_projectPath = std::move(projectPath);
    m_generation = generation;

    m_width = m_project.timeline.width > 0 ? m_project.timeline.width : 1920;
    m_height = m_project.timeline.height > 0 ? m_project.timeline.height : 1080;

    QString error;
    m_context = D3D11Context::create(true, &error);
    if (!m_context) {
        // Video decode may be unavailable (WARP / no video device); software
        // decoding and upload still work on a plain D3D11 device.
        QString fallbackError;
        m_context = D3D11Context::create(false, &fallbackError);
        if (!m_context) {
            emit this->error(QStringLiteral("%1\n%2").arg(error, fallbackError));
            return;
        }
    }

    m_ring = std::make_unique<SharedTextureRing>(*m_context);
    if (!m_ring->create(m_width, m_height, SharedTextureRing::kSlotCount,
                        &error)) {
        emit this->error(error);
        reset();
        return;
    }

    m_compositor = std::make_unique<LayerCompositor>();
    if (!m_compositor->initialize(*m_context, m_width, m_height, &error)) {
        emit this->error(error);
        reset();
        return;
    }

    m_valid = true;
}

ClipVideoSource *TimelineRenderWorker::sourceFor(const VideoLayer &layer)
{
    auto existing = m_sources.find(layer.clipId);
    if (existing != m_sources.end())
        return existing->second.get();

    if (m_failedClips.contains(layer.clipId))
        return nullptr;

    const Asset *asset = m_project.assetById(layer.assetId);
    if (!asset)
        return nullptr;

    QString error;
    auto source = ClipVideoSource::open(*asset, *m_context, asset->proxyPath,
                                        &error);
    if (!source) {
        m_failedClips.insert(layer.clipId);
        emit this->error(QStringLiteral("无法打开片段素材“%1”：%2")
                             .arg(asset->path, error));
        return nullptr;
    }

    ClipVideoSource *raw = source.get();
    m_sources.emplace(layer.clipId, std::move(source));
    return raw;
}

QImage TimelineRenderWorker::subtitleImageFor(
    const QList<SubtitleLayer> &layers)
{
    const QString key = SubtitleRasterizer::cacheKey(layers, m_width, m_height);
    if (key != m_subtitleKey) {
        m_subtitleKey = key;
        m_subtitleImage =
            layers.isEmpty()
                ? QImage()
                : SubtitleRasterizer::render(layers, m_width, m_height,
                                             m_project.subtitleStyles);
    }
    return m_subtitleImage;
}

void TimelineRenderWorker::renderFrame(qint64 timelineUs, quint64 generation)
{
    if (!m_valid || !m_ring || !m_compositor || generation != m_generation)
        return;

    const int64_t requestedUs = std::max<int64_t>(0, timelineUs);
    const FrameComposition frame =
        TimelineEvaluator(m_project).frameAt(requestedUs);

    const int slot = m_ring->nextWriteIndex();
    IDXGIKeyedMutex *mutex = m_ring->mutex(slot);
    if (!mutex || mutex->AcquireSync(0, 50) != S_OK)
        return; // the scene graph still owns this slot; skip this frame

    m_compositor->begin(m_ring->rtv(slot), true);

    for (const VideoLayer &layer : frame.video) {
        // Missing assets become gaps/black, never a crash: the evaluator may
        // reference an asset that was removed or never resolved.
        if (!m_project.assetById(layer.assetId))
            continue;

        ClipVideoSource *source = sourceFor(layer);
        if (!source)
            continue;

        QString error;
        const DecodedFrame *decoded = source->frameAt(layer.sourceTimeUs, &error);
        if (!decoded) {
            if (!error.isEmpty() && !m_reportedClips.contains(layer.clipId)) {
                m_reportedClips.insert(layer.clipId);
                emit this->error(error);
            }
            continue;
        }

        const LayerRect rect =
            layerRectFor(layer.transform, source->width(), source->height(),
                         m_width, m_height);
        const float opacity =
            std::clamp(layer.transform.opacityMilli / 1000.0f, 0.0f, 1.0f);
        if (!m_compositor->drawFrame(*decoded, rect, opacity, &error)
            && !error.isEmpty()) {
            emit this->error(error);
        }
    }

    // Subtitles are one cached transparent full-frame image drawn topmost.
    const QImage subtitles = subtitleImageFor(frame.subtitles);
    if (!subtitles.isNull()) {
        QString error;
        if (!m_compositor->drawImage(subtitles, LayerRect{0, 0, 1, 1}, 1.0f,
                                     &error)
            && !error.isEmpty()) {
            emit this->error(error);
        }
    }

    m_compositor->end();
    mutex->ReleaseSync(1);
    m_ring->advanceWriteIndex();

    emit frameReady(slot, QSize(m_width, m_height), requestedUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)),
                    m_generation);
}

} // namespace VideoEditor
