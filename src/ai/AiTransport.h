// src/ai/AiTransport.h
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>

#include "AiProtocol.h"

namespace VideoEditor {

// Abstraction over the local inference subprocess. Tests substitute a fake
// implementation; production uses LocalAiTransport.
class AiTransport : public QObject {
    Q_OBJECT
public:
    explicit AiTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~AiTransport() override = default;

    virtual bool start(const AiJobSpec &spec, QString *error = nullptr) = 0;
    virtual void cancel() = 0;
    // Out-of-band command (pause/resume/cancel) sent over the job socket.
    // Default no-op so fakes that only exercise messages keep compiling.
    virtual void sendCommand(const QJsonObject &command)
    {
        Q_UNUSED(command)
    }

signals:
    void message(const QJsonObject &message);
    void finished(int exitCode, QProcess::ExitStatus status);
};

} // namespace VideoEditor
