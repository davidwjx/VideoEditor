#pragma once

#include <QtGlobal>

#include <QString>

#include "ExportSettings.h"

namespace VideoEditor {

// Disk-space verdict for one export. `requiredBytes` includes the estimated
// payload plus encoder/muxer overhead and a fixed safety headroom so a small
// overrun does not fill the disk halfway through the last segment.
struct DiskSpaceEstimate {
    qint64 requiredBytes = 0;
    qint64 freeBytes = 0;
    bool ok = false;
    QString error;
};

// Estimated bytes the export needs: video at the effective bitrate, 192 kbps
// AAC, 10 % container/segment overhead and 32 MiB headroom. Never returns a
// negative value.
qint64 estimateExportBytes(const ExportSettings &settings,
                           const ExportTiming &timing);

// Compares the two values and produces a Chinese explanation that names both
// the required and the available size when the export does not fit.
DiskSpaceEstimate evaluateDiskSpace(qint64 freeBytes, qint64 requiredBytes);

// Free bytes on the volume that will hold `outputPath`. Missing paths walk up
// to the nearest existing parent; an unresolvable path returns 0.
qint64 availableBytesFor(const QString &outputPath);

} // namespace VideoEditor
