#include "RepairFrameSource.h"

#include <algorithm>
#include <cmath>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString cacheError(const QString &detail)
{
    return QStringLiteral("AI 修复缓存缺失或损坏：%1").arg(detail);
}

} // namespace

RepairFrameSource::RepairFrameSource(const QString &cacheRoot,
                                     RemovalCacheManifest manifest)
    : m_cache(cacheRoot)
    , m_manifest(std::move(manifest))
    , m_cacheKey(m_manifest.cacheKey)
{
}

std::unique_ptr<RepairFrameSource> RepairFrameSource::open(
    const RemovalCache &cache, const QString &cacheKey, QString *error)
{
    if (error)
        error->clear();

    QString stateReason;
    if (cache.state(cacheKey, &stateReason) != RemovalCacheState::Complete) {
        setError(error, cacheError(stateReason.isEmpty()
                                       ? QStringLiteral("修复缓存尚未生成。")
                                       : stateReason));
        return nullptr;
    }

    QString manifestError;
    const auto manifest = cache.readManifest(cacheKey, &manifestError);
    if (!manifest) {
        setError(error, cacheError(manifestError.isEmpty()
                                       ? QStringLiteral("无法读取修复缓存清单。")
                                       : manifestError));
        return nullptr;
    }
    if (manifest->status != QStringLiteral("complete")) {
        setError(error, cacheError(QStringLiteral("修复缓存不完整（%1）。")
                                       .arg(manifest->status)));
        return nullptr;
    }
    if (manifest->width <= 0 || manifest->height <= 0) {
        setError(error, cacheError(QStringLiteral("修复缓存尺寸无效。")));
        return nullptr;
    }

    auto source = std::unique_ptr<RepairFrameSource>(
        new RepairFrameSource(cache.root(), *manifest));

    // Load every chunk index up front. Failure here means the manifest claims
    // a chunk that cannot be parsed; treating it as a partial cache can never
    // be mistaken for "this frame has no hole".
    for (const RemovalCacheChunk &chunk : manifest->chunks) {
        QString indexError;
        const auto index =
            cache.readChunkIndex(cacheKey, chunk.index, &indexError);
        if (!index) {
            setError(error, cacheError(indexError.isEmpty()
                                           ? QStringLiteral("无法读取分块 %1。")
                                                 .arg(chunk.index)
                                           : indexError));
            return nullptr;
        }
        for (const RemovalChunkFrame &frame : index->frames) {
            // A corrupt index must not be able to stretch a patch over the
            // whole layer. Validate the global frame and its crop against the
            // display-sized source before accepting the chunk.
            if (frame.frame < 0 || frame.frame >= manifest->totalFrames) {
                setError(error,
                         cacheError(QStringLiteral("第 %1 帧序号越界。")
                                        .arg(frame.frame)));
                return nullptr;
            }
            const QRect &crop = frame.crop;
            if (!crop.isValid() || crop.x() < 0 || crop.y() < 0
                || crop.x() + crop.width() > manifest->width
                || crop.y() + crop.height() > manifest->height) {
                setError(error,
                         cacheError(QStringLiteral("第 %1 帧裁剪区域无效。")
                                        .arg(frame.frame)));
                return nullptr;
            }
            source->m_locations.insert(frame.frame, frame);
        }
    }

    source->m_complete = true;
    return source;
}

const RepairFrameSource::Frame *RepairFrameSource::frameAt(int64_t sourceUs,
                                                           QString *error)
{
    if (error)
        error->clear();
    if (!m_complete)
        return nullptr;
    // The caller draws the original outside the generated source range. The
    // range is half-open, matching the layer enablement rule in the evaluator.
    if (sourceUs < m_manifest.sourceInUs || sourceUs >= m_manifest.sourceOutUs)
        return nullptr;
    if (m_manifest.totalFrames <= 0)
        return nullptr;

    const Rational fps = m_manifest.fps.isValid() ? m_manifest.fps
                                                  : Rational{30, 1};
    const double index =
        std::floor(double(sourceUs - m_manifest.sourceInUs) * double(fps.num)
                   / (1'000'000.0 * double(fps.den)));
    const int frame = int(std::clamp(index, 0.0,
                                     double(m_manifest.totalFrames - 1)));

    if (Frame *cached = m_frames.object(frame))
        return cached;

    const auto location = m_locations.constFind(frame);
    if (location == m_locations.constEnd())
        return nullptr; // Sparse cache: this frame has no hole.

    QString readError;
    const QImage patch = m_cache.readFrameImage(m_cacheKey, *location, &readError);
    if (patch.isNull()) {
        // Always carry the "AI 修复缓存" prefix so a mid-export deletion is
        // forwarded verbatim by ExportPipeline instead of being mistaken for
        // a transient decode failure that may fall back to the original.
        setError(error, cacheError(readError.isEmpty()
                                       ? QStringLiteral("无法读取第 %1 帧。")
                                             .arg(frame)
                                       : readError));
        return nullptr;
    }

    auto *entry = new Frame{patch, location->crop, m_manifest.width,
                            m_manifest.height};
    m_frames.insert(frame, entry);
    return entry;
}

} // namespace VideoEditor
