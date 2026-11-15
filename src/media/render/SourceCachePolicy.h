#pragma once

#include <QHash>
#include <QSet>
#include <QStringList>

namespace VideoEditor {

// Chooses clip sources to evict so a worker keeps at most maxCount decoders.
// Sources used by the current frame are never evicted; the remaining slots go
// to the least-recently-used sources first.
QStringList evictSourceKeys(const QHash<QString, quint64> &lastUsedFrame,
                            const QSet<QString> &usedThisFrame, int maxCount);

} // namespace VideoEditor
