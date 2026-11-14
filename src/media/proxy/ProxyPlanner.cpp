#include "ProxyPlanner.h"

namespace VideoEditor {

ProxyPlan planProxy(const MediaInfo &info)
{
    ProxyPlan plan;
    if (info.width > 1920 || info.height > 1080) {
        plan.needed = true;
        plan.height = 540;
        plan.reason = QStringLiteral("素材分辨率高于 1080p，已生成代理。");
    }
    return plan;
}

} // namespace VideoEditor
