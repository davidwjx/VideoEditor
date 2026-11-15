#include "TimelineCompositor.h"

#include <algorithm>

#include "ClipVideoSource.h"
#include "D3D11Context.h"
#include "LayerCompositor.h"
#include "LayerGeometry.h"
#include "SourceCachePolicy.h"
#include "SubtitleRasterizer.h"
#include "TimelineEvaluator.h"

namespace VideoEditor {

namespace {

// Upper bound on open decoders. Sources active in the current frame are always
// kept; the LRU policy only evicts decoders no layer is using.
constexpr int kMaxCachedSources = 4;

} // namespace

TimelineCompositor::TimelineCompositor() = default;

TimelineCompositor::~TimelineCompositor()
{
    close();
}

void TimelineCompositor::close()
{
    // Sources own D3D11 decoder assets and must die before the device and the
    // compositor that cached views over their textures.
    m_sources.clear();
    m_failedClips.clear();
    m_reportedErrors.clear();
    m_converters.reset();
    m_compositor.reset();
    m_context.reset();
    m_subtitleKey.clear();
    m_subtitleImage = QImage();
    m_frameSerial = 0;
    m_valid = false;
}

bool TimelineCompositor::open(const Project &project, int width, int height,
                              bool useProxy, QString *error)
{
    const bool reusable = m_valid && m_context && m_compositor
                          && m_width == width && m_height == height;
    m_project = project;
    m_useProxy = useProxy;
    // Every snapshot re-opens clip decoders; a proxy toggle in particular must
    // discard sources opened from the other file.
    m_sources.clear();
    m_failedClips.clear();
    m_reportedErrors.clear();
    m_subtitleKey.clear();
    m_subtitleImage = QImage();
    m_frameSerial = 0;
    if (m_compositor)
        m_compositor->clearTextureCache();
    if (reusable)
        return true;

    close();
    m_width = width;
    m_height = height;
    QString deviceError;
    m_context = D3D11Context::create(true, &deviceError);
    if (!m_context) {
        // Video decode may be unavailable (WARP / no video device); software
        // decoding and upload still work on a plain D3D11 device.
        QString fallbackError;
        m_context = D3D11Context::create(false, &fallbackError);
        if (!m_context) {
            if (error)
                *error =
                    QStringLiteral("%1\n%2").arg(deviceError, fallbackError);
            return false;
        }
    }
    m_compositor = std::make_unique<LayerCompositor>();
    if (!m_compositor->initialize(*m_context, m_width, m_height, error)) {
        close();
        return false;
    }
    m_valid = true;
    return true;
}

bool TimelineCompositor::ensureConverters(QString *error)
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

void TimelineCompositor::reportOnce(const QString &key, const QString &message)
{
    if (message.isEmpty() || m_reportedErrors.contains(key))
        return;
    m_reportedErrors.insert(key);
    if (m_warning)
        m_warning(message);
}

ClipVideoSource *TimelineCompositor::sourceFor(const VideoLayer &layer)
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

QImage
TimelineCompositor::subtitleImageFor(const QList<SubtitleLayer> &layers)
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

int TimelineCompositor::drawFrame(int64_t timelineUs,
                                  ID3D11RenderTargetView *target,
                                  bool clearBlack, QString *error)
{
    if (!m_valid || !m_compositor || !target) {
        if (error)
            *error = QStringLiteral("合成器未初始化。");
        return 0;
    }

    const FrameComposition frame =
        TimelineEvaluator(m_project).frameAt(std::max<int64_t>(0, timelineUs));
    ++m_frameSerial;

    m_compositor->begin(target, clearBlack);

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

        QString layerError;
        const DecodedFrame *decoded =
            source->frameAt(layer.sourceTimeUs, &layerError);
        // Converters may fail after a good frame was decoded; never swallow
        // that error just because a stale frame could be shown.
        if (!layerError.isEmpty())
            reportOnce(QStringLiteral("decode:") + layer.clipId, layerError);
        if (!decoded)
            continue;

        const LayerRect rect =
            layerRectFor(layer.transform, source->width(), source->height(),
                         m_width, m_height);
        const float opacity =
            std::clamp(layer.transform.opacityMilli / 1000.0f, 0.0f, 1.0f);
        if (m_compositor->drawFrame(*decoded, rect, opacity, &layerError))
            ++drawnLayers;
        else if (!layerError.isEmpty())
            reportOnce(QStringLiteral("draw:") + layer.clipId, layerError);
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
        QString drawError;
        if (!m_compositor->drawImageCached(m_subtitleKey, subtitles,
                                           LayerRect{0, 0, 1, 1}, 1.0f,
                                           &drawError))
            reportOnce(QStringLiteral("draw:subtitles"), drawError);
    }

    m_compositor->end();
    return drawnLayers;
}

} // namespace VideoEditor
