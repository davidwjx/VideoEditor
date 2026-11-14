#include "ProxyPlanner.h"

#include <QFileInfo>

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

bool needsProxyGeneration(const MediaInfo &info, const QString &proxyPath)
{
    if (!planProxy(info).needed)
        return false;
    return proxyPath.isEmpty() || !QFileInfo::exists(proxyPath);
}

} // namespace VideoEditor
