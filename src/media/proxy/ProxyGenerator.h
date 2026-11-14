#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace VideoEditor {

class ProxyGenerator : public QObject {
    Q_OBJECT

public:
    explicit ProxyGenerator(QString ffmpegPath, QObject *parent = nullptr);

    void start(const QString &source, const QString &output, int height);
    void cancel();

signals:
    void progress(int percent);
    void finished(QString outputPath);
    void failed(QString message);

private:
    QString m_ffmpegPath;
    QProcess *m_process = nullptr;
    QString m_output;
    QString m_temp;
};

} // namespace VideoEditor
