#include "TimelineRenderWorker.h"

#include <algorithm>
#include <utility>

#include "ClipVideoSource.h"
#include "D3D11Context.h"
#include "LayerCompositor.h"
#include "LayerGeometry.h"
#include "SharedTextureRing.h"
#include "SourceCachePolicy.h"
#include "SubtitleRasterizer.h"

namespace VideoEditor {

namespace {

// Upper bound on open decoders. Sources active in the current frame are always
// kept; the LRU policy only evicts decoders no layer is using.
constexpr int kMaxCachedSources = 4;

} // namespace

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
    m_reportedErrors.clear();
    m_converters.reset();
    m_compositor.reset();
    m_ring.reset();
    m_context.reset();
    m_subtitleKey.clear();
    m_subtitleImage = QImage();
    m_frameSerial = 0;
    m_valid = false;
}

void TimelineRenderWorker::close()
{
    reset();
    m_project = Project{};
    m_projectPath.clear();
    m_generation = 0;
}

void TimelineRenderWorker::reportOnce(const QString &key,
                                      const QString &message)
{
    if (message.isEmpty() || m_reportedErrors.contains(key))
        return;
    m_reportedErrors.insert(key);
    emit error(message);
}

bool TimelineRenderWorker::ensureConverters(QString *error)
{
    if (m_converters && m_converters->isInitialized())
        return true;
    if (!m_context) {
        if (error)
            *error = QStringLiteral("D3D11 设备不可用。");
        return false;
    }
    m_converters = std::make_unique<VideoConverters>();
    if (!m_converters->initialize(*m_context, error)) {
        m_converters.reset();
        return false;
    }
    return true;
}

void TimelineRenderWorker::setProject(VideoEditor::Project project,
                                      QString projectPath, quint64 generation,
                                      bool useProxy)
{
    const int width = project.timeline.width > 0 ? project.timeline.width : 1920;
    const int height =
        project.timeline.height > 0 ? project.timeline.height : 1080;

    // Project edits usually keep the output resolution: keep the device, ring
    // and compositor, and only drop the clip decoders and subtitle cache.
    const bool reusable = m_valid && m_context && m_ring && m_compositor
                          && m_width == width && m_height == height;

    m_project = std::move(project);
    m_projectPath = std::move(projectPath);
    m_generation = generation;
    // Every queued snapshot re-opens clip decoders; a proxy toggle in
    // particular must discard sources opened from the other file.
    m_useProxy = useProxy;
    m_sources.clear();
    m_failedClips.clear();
    m_reportedErrors.clear();
    m_subtitleKey.clear();
    m_subtitleImage = QImage();
    m_frameSerial = 0;
    if (m_compositor)
        m_compositor->clearTextureCache();

    if (reusable)
        return;

    reset();
    m_width = width;
    m_height = height;

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
    if (existing != m_sources.end()) {
        existing->second.lastUsedFrame = m_frameSerial;
        return existing->second.source.get();
    }

    if (m_failedClips.contains(layer.clipId))
        return nullptr;

    const Asset *asset = m_project.assetById(layer.assetId);
    if (!asset)
        return nullptr;

    QString error;
    if (!ensureConverters(&error)) {
        reportOnce(QStringLiteral("converters"), error);
        return nullptr;
    }

    const QString proxyPath = activeProxyPath(*asset, m_useProxy);
    auto source = ClipVideoSource::open(*asset, *m_context, proxyPath,
                                        m_converters.get(), &error);
    if (!source) {
        m_failedClips.insert(layer.clipId);
        reportOnce(QStringLiteral("open:") + layer.clipId,
                   QStringLiteral("无法打开片段素材“%1”：%2")
                       .arg(asset->path, error));
        return nullptr;
    }

    ClipVideoSource *raw = source.get();
    SourceEntry entry;
    entry.source = std::move(source);
    entry.lastUsedFrame = m_frameSerial;
    m_sources.emplace(layer.clipId, std::move(entry));
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
    const int64_t requestedUs = std::max<int64_t>(0, timelineUs);
    if (generation != m_generation)
        return;
    if (!m_valid || !m_ring || !m_compositor) {
        emit frameSkipped(requestedUs, generation);
        return;
    }

    const FrameComposition frame =
        TimelineEvaluator(m_project).frameAt(requestedUs);
    ++m_frameSerial;

    const int slot = m_ring->nextWriteIndex();
    IDXGIKeyedMutex *mutex = m_ring->mutex(slot);
    if (!mutex || mutex->AcquireSync(0, 50) != S_OK) {
        // The scene graph still owns this slot. Tell the controller so its
        // single-in-flight gate is released instead of stalling.
        emit frameSkipped(requestedUs, generation);
        return;
    }

    m_compositor->begin(m_ring->rtv(slot), true);

    QSet<QString> usedThisFrame;
    int drawnLayers = 0;
    for (const VideoLayer &layer : frame.video) {
        // Missing assets become gaps/black, never a crash: the evaluator may
        // reference an asset that was removed or never resolved.
        if (!m_project.assetById(layer.assetId))
            continue;

        ClipVideoSource *source = sourceFor(layer);
        if (!source)
            continue;
        usedThisFrame.insert(layer.clipId);

        QString error;
        const DecodedFrame *decoded =
            source->frameAt(layer.sourceTimeUs, &error);
        // Converters may fail after a good frame was decoded; never swallow
        // that error just because a stale frame could be shown.
        if (!error.isEmpty())
            reportOnce(QStringLiteral("decode:") + layer.clipId, error);
        if (!decoded)
            continue;

        const LayerRect rect =
            layerRectFor(layer.transform, source->width(), source->height(),
                         m_width, m_height);
        const float opacity =
            std::clamp(layer.transform.opacityMilli / 1000.0f, 0.0f, 1.0f);
        if (m_compositor->drawFrame(*decoded, rect, opacity, &error))
            ++drawnLayers;
        else if (!error.isEmpty())
            reportOnce(QStringLiteral("draw:") + layer.clipId, error);
    }

    // No decoder may accumulate for clips that are no longer on screen.
    QHash<QString, quint64> lastUsed;
    for (const auto &entry : m_sources)
        lastUsed.insert(entry.first, entry.second.lastUsedFrame);
    const QStringList evicted =
        evictSourceKeys(lastUsed, usedThisFrame, kMaxCachedSources);
    for (const QString &clipId : evicted) {
        auto it = m_sources.find(clipId);
        if (it == m_sources.end())
            continue;
        m_compositor->forgetTexture(it->second.source->texture());
        m_sources.erase(it);
    }

    // Subtitles are one cached transparent full-frame image drawn topmost.
    // The RGBA upload is skipped while the cue/style cache key is unchanged.
    const QImage subtitles = subtitleImageFor(frame.subtitles);
    if (!subtitles.isNull()) {
        QString error;
        if (!m_compositor->drawImageCached(
                m_subtitleKey, subtitles, LayerRect{0, 0, 1, 1}, 1.0f,
                &error)
            && !error.isEmpty()) {
            reportOnce(QStringLiteral("draw:subtitles"), error);
        }
    }

    m_compositor->end();
    mutex->ReleaseSync(1);
    m_ring->advanceWriteIndex();

    emit frameReady(slot, QSize(m_width, m_height), requestedUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)),
                    drawnLayers, m_generation);
}

} // namespace VideoEditor
