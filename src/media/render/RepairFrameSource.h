#pragma once

#include <cstdint>
#include <memory>

#include <QCache>
#include <QHash>
#include <QImage>
#include <QRect>
#include <QString>

#include "RemovalCache.h"

namespace VideoEditor {

// Read-only view over one complete repair cache. The manifest is the commit
// point: a partial/missing/corrupt cache is rejected at open() and never
// silently treated as "no removal". Patches are sparse (only frames with a
// non-empty mask have an entry), so frameAt() returns nullptr for frames the
// generator intentionally skipped; the caller keeps drawing the original.
class RepairFrameSource {
public:
    struct Frame {
        QImage patch;
        QRect crop;
        int sourceWidth = 0;
        int sourceHeight = 0;
    };

    static std::unique_ptr<RepairFrameSource> open(
        const RemovalCache &cache, const QString &cacheKey,
        QString *error = nullptr);

    // Maps source time to a global frame index, clamps to the manifest range,
    // reads the PNG lazily and caches the last 12 decoded patches. Returns
    // nullptr outside [sourceInUs, sourceOutUs) and for frames without a
    // patch. A non-empty *error reports an unreadable/corrupt patch.
    const Frame *frameAt(int64_t sourceUs, QString *error = nullptr);

    bool isComplete() const { return m_complete; }
    int frameCount() const { return m_manifest.totalFrames; }
    const RemovalCacheManifest &manifest() const { return m_manifest; }

private:
    RepairFrameSource(const QString &cacheRoot, RemovalCacheManifest manifest);

    RemovalCache m_cache;
    RemovalCacheManifest m_manifest;
    QString m_cacheKey;
    QHash<int, RemovalChunkFrame> m_locations;
    QCache<int, Frame> m_frames{12};
    bool m_complete = false;
};

} // namespace VideoEditor
