#pragma once

#include <QString>

#include "MediaInfo.h"

namespace VideoEditor {

struct ProxyPlan {
    bool needed = false;
    int height = 540;
    QString reason;
};

ProxyPlan planProxy(const MediaInfo &info);

// True when the source needs a proxy and the recorded proxy path is either
// unset or no longer present on disk (cache cleared, undo/redo, project saved
// before generation finished).
bool needsProxyGeneration(const MediaInfo &info, const QString &proxyPath);

} // namespace VideoEditor
