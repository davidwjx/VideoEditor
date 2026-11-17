#pragma once

#include <cstdint>
#include <memory>

#include <QImage>
#include <QMetaType>
#include <QRect>
#include <QString>

namespace VideoEditor {

// Alpha-bearing patch above `crop` in the display-sized source frame, produced
// by the GPU live preview. It is intentionally identical in shape to a cached
// repair patch so the compositor can draw both through the same path.
struct LiveRepairPatch {
    QImage image;
    QRect crop;
};

// Read-only view the compositor consults before the on-disk repair cache. The
// implementation lives outside videoeditor_media (the UI owns the worker and
// the models), so this header must stay free of any AI dependency. A miss (or
// an out-of-range instant) must return false so the caller falls through to
// RepairFrameSource and finally to the original frame.
class LiveRepairProvider {
public:
    virtual ~LiveRepairProvider() = default;
    virtual bool patchFor(const QString &layerId, int64_t sourceTimeUs,
                          LiveRepairPatch *patch) const = 0;
};

using LiveRepairProviderPtr = std::shared_ptr<LiveRepairProvider>;

} // namespace VideoEditor

// Queued worker calls copy the shared provider across the thread boundary.
Q_DECLARE_METATYPE(VideoEditor::LiveRepairProviderPtr)
