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

} // namespace VideoEditor
