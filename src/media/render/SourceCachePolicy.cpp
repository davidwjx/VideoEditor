#include "SourceCachePolicy.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace VideoEditor {

QStringList evictSourceKeys(const QHash<QString, quint64> &lastUsedFrame,
                            const QSet<QString> &usedThisFrame, int maxCount)
{
    if (maxCount < 0)
        maxCount = 0;
    const int excess = lastUsedFrame.size() - maxCount;
    if (excess <= 0)
        return {};

    std::vector<std::pair<QString, quint64>> candidates;
    candidates.reserve(size_t(lastUsedFrame.size()));
    for (auto it = lastUsedFrame.cbegin(); it != lastUsedFrame.cend(); ++it) {
        if (!usedThisFrame.contains(it.key()))
            candidates.emplace_back(it.key(), it.value());
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto &lhs, const auto &rhs) {
                  if (lhs.second != rhs.second)
                      return lhs.second < rhs.second;
                  return lhs.first < rhs.first;
              });

    const size_t count =
        std::min(candidates.size(), size_t(std::max(0, excess)));
    QStringList evicted;
    evicted.reserve(int(count));
    for (size_t i = 0; i < count; ++i)
        evicted.append(candidates[i].first);
    return evicted;
}

} // namespace VideoEditor
