#pragma once

#include <QObject>
#include <QQueue>
#include <QString>

#include "CacheStore.h"

namespace VideoEditor {

class ProxyGenerator;

class ProxyService : public QObject {
    Q_OBJECT

public:
    explicit ProxyService(QString cacheRoot, QString ffmpegPath,
                          QObject *parent = nullptr);

    void request(const QString &assetId, const QString &sourcePath,
                 int height = 540);
    bool isBusy() const { return m_generator != nullptr; }

signals:
    void ready(QString assetId, QString proxyPath);
    void failed(QString assetId, QString message);

private:
    struct Job {
        QString assetId;
        QString sourcePath;
        QString outputPath;
        int height = 540;
    };

    void startNext();

    CacheStore m_cache;
    QString m_ffmpegPath;
    QQueue<Job> m_queue;
    ProxyGenerator *m_generator = nullptr;
};

} // namespace VideoEditor
