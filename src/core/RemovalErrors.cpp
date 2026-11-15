#include "RemovalErrors.h"

namespace VideoEditor {

QString removalErrorMessage(const QString &code, const QString &detail)
{
    if (code == QStringLiteral("vram"))
        return QStringLiteral("显存不足，已降低处理分辨率并分块处理。预计耗时增加。可在高级设置中进一步降低分辨率。");
    if (code == QStringLiteral("device_lost"))
        return QStringLiteral("显卡设备已重置，AI 任务已中断。已完成的分段已保留，可重试或改用 CPU。");
    if (code == QStringLiteral("model_missing")
        || code == QStringLiteral("model_unavailable"))
        return QStringLiteral("AI 模型文件缺失或校验失败，请重新安装应用或修复模型包。");
    if (code == QStringLiteral("cache_missing"))
        return QStringLiteral("AI 修复结果缺失或已过期。请先生成修复，再导出。");
    if (code == QStringLiteral("low_confidence"))
        return QStringLiteral("跟踪置信度低，请修正遮罩后从此帧重新传播。");
    if (code == QStringLiteral("disk_full"))
        return QStringLiteral("磁盘空间不足，已停止写入并保留进度。请清理缓存或更换缓存目录。");
    if (code == QStringLiteral("cancelled"))
        return QStringLiteral("已取消，已完成的分段已保留，可继续生成。");
    return detail.isEmpty() ? QStringLiteral("AI 任务失败，请重试或查看日志。")
                            : detail;
}

} // namespace VideoEditor
