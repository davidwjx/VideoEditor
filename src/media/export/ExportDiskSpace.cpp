#include "ExportDiskSpace.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

namespace VideoEditor {

namespace {

constexpr qint64 kHeadroomBytes = 32LL << 20;
constexpr int kAudioBitrateKbps = 192;

QString formatBytes(qint64 bytes)
{
    constexpr double kKb = 1024.0;
    constexpr double kMb = kKb * 1024.0;
    constexpr double kGb = kMb * 1024.0;
    const double value = double(std::max<qint64>(0, bytes));
    if (value >= kGb)
        return QStringLiteral("%1 GB").arg(QString::number(value / kGb, 'f', 1));
    if (value >= kMb)
        return QStringLiteral("%1 MB").arg(QString::number(value / kMb, 'f', 1));
    if (value >= kKb)
        return QStringLiteral("%1 KB").arg(QString::number(value / kKb, 'f', 1));
    return QStringLiteral("%1 字节").arg(bytes);
}

} // namespace

qint64 estimateExportBytes(const ExportSettings &settings,
                           const ExportTiming &timing)
{
    if (timing.totalFrames <= 0 || !settings.fps.isValid())
        return kHeadroomBytes;
    const int64_t durationUs =
        (timing.totalFrames * 1'000'000LL * settings.fps.den
         + settings.fps.num / 2)
        / settings.fps.num;
    const int64_t videoBytes =
        int64_t(effectiveBitrateKbps(settings)) * 1000 / 8 * durationUs
        / 1'000'000LL;
    const int64_t audioBytes = int64_t(kAudioBitrateKbps * 1000 / 8) * durationUs
                               / 1'000'000LL;
    const int64_t payload = videoBytes + audioBytes;
    const int64_t overhead = payload / 10;
    return std::max<qint64>(kHeadroomBytes, payload + overhead + kHeadroomBytes);
}

DiskSpaceEstimate evaluateDiskSpace(qint64 freeBytes, qint64 requiredBytes)
{
    DiskSpaceEstimate result;
    result.freeBytes = freeBytes;
    result.requiredBytes = requiredBytes;
    result.ok = freeBytes >= requiredBytes;
    if (!result.ok) {
        result.error = QStringLiteral(
            "磁盘空间不足：导出至少需要 %1，当前可用 %2。请清理磁盘或更换输出位置。")
                           .arg(formatBytes(requiredBytes),
                                formatBytes(freeBytes));
    }
    return result;
}

qint64 availableBytesFor(const QString &outputPath)
{
    const QFileInfo info(outputPath);
    QString path = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    while (!path.isEmpty() && !QFileInfo::exists(path)) {
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path)
            break;
        path = parent;
    }
    const QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady())
        return 0;
    return storage.bytesAvailable();
}

} // namespace VideoEditor
