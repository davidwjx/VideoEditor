#pragma once

#include <optional>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include "MaskCodec.h"

namespace VideoEditor {

// Project-owned mask/tracking data. It lives next to the project file
// (<project>.veproj.assets), never under the disposable cache root.
class MaskSidecarStore {
public:
    explicit MaskSidecarStore(QString projectFilePath = {});

    static QString rootForProject(const QString &projectFilePath,
                                  const QString &fallbackRoot = {});
    void setProjectPath(const QString &projectFilePath,
                        const QString &fallbackRoot = {});
    QString projectFilePath() const { return m_projectFilePath; }
    QString sidecarRoot() const;

    QString maskPath(const QString &maskHash) const;
    QString trackingPath(const QString &confidenceHash) const;

    bool ensureLayout(QString *error = nullptr);
    bool writeMask(const BinaryMask &mask, QString *hashOut,
                   QString *error = nullptr);
    std::optional<BinaryMask> readMask(const QString &maskHash,
                                       QString *error = nullptr) const;
    bool writeTracking(const QJsonObject &json, QString *hashOut,
                       QString *error = nullptr);
    std::optional<QJsonObject> readTracking(const QString &confidenceHash,
                                            QString *error = nullptr) const;

    bool relocateTo(const QString &newProjectFilePath, QString *error = nullptr);
    bool removeUnreferenced(const QSet<QString> &maskHashes,
                            const QSet<QString> &trackingHashes,
                            QString *error = nullptr);

private:
    QString m_projectFilePath;
    QString m_fallbackRoot;
};

} // namespace VideoEditor
