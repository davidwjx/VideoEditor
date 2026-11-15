// src/ai/LocalAiTransport.h
#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QString>
#include <QTimer>

#include "AiTransport.h"

namespace VideoEditor {

// Runs VisionAiEngine.exe as a child process and talks NDJSON over a private
// QLocalServer socket. The job spec is passed through a UUID-named temp file;
// proxy environment variables are stripped so the engine cannot reach the
// network.
class LocalAiTransport : public AiTransport {
    Q_OBJECT
public:
    explicit LocalAiTransport(QObject *parent = nullptr);
    ~LocalAiTransport() override;

    bool start(const AiJobSpec &spec, QString *error = nullptr) override;
    void cancel() override;
    void sendCommand(const QJsonObject &command) override;

private:
    void onNewConnection();
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onCancelTimeout();
    void failProtocol(const QString &message);
    void cleanup(bool removeJobFile);

    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;
    QProcess *m_process = nullptr;
    QTimer *m_cancelTimer = nullptr;
    QByteArray m_buffer;
    QString m_jobPath;
    QString m_serverName;
    bool m_finishedEmitted = false;
};

} // namespace VideoEditor
