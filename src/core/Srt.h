#pragma once

#include <cstdint>
#include <QByteArray>
#include <QList>
#include <QString>

#include "Timeline.h"

namespace VideoEditor {

struct SrtParseResult {
    QList<SubtitleCue> cues;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

QString decodeTextFile(const QByteArray &bytes);
SrtParseResult parseSrt(const QByteArray &bytes,
                        const QString &defaultStyleId = QStringLiteral("default"));
QByteArray writeSrt(const QList<SubtitleCue> &cues);
bool parseSrtTime(const QString &text, int64_t *startUs, int64_t *endUs);
QString formatSrtTime(int64_t us);

} // namespace VideoEditor
