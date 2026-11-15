// src/ai/AiEngineMain.cpp
//
// Task 4 stub: it exists so the parent process can exercise the real
// subprocess protocol. It reads only the AiJobSpec, connects to the local
// socket and reports that models are not installed yet. It never opens the
// project file and never touches the network.

#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocalSocket>

#include "AiProtocol.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("job"), QStringLiteral("job spec"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("socket"), QStringLiteral("local socket"),
                      QStringLiteral("name")});
    parser.process(app);

    const QString jobPath = parser.value(QStringLiteral("job"));
    const QString socketName = parser.value(QStringLiteral("socket"));
    QFile jobFile(jobPath);
    if (!jobFile.open(QIODevice::ReadOnly))
        return 2;
    QString error;
    const auto spec = VideoEditor::aiJobSpecFromJson(
        QJsonDocument::fromJson(jobFile.readAll()).object(), &error);
    if (!spec)
        return 2;

    QLocalSocket socket;
    socket.connectToServer(socketName);
    if (!socket.waitForConnected(5000))
        return 3;

    const auto send = [&socket](const QJsonObject &message) {
        socket.write(VideoEditor::AiProtocol::encodeLine(message));
        socket.flush();
    };

    // Task 4 only validates the protocol; Tasks 5/6 add the ONNX pipelines.
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("error")},
                     {QStringLiteral("code"), QStringLiteral("model_unavailable")},
                     {QStringLiteral("message"),
                      QStringLiteral("AI 模型尚未安装。")}});
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                     {QStringLiteral("cacheKey"), spec->cacheKey},
                     {QStringLiteral("completedFrames"), 0}});
    // flush() only queues the bytes; the process must not exit (closing the
    // pipe) before the parent has actually received them.
    socket.waitForBytesWritten(5000);
    socket.disconnectFromServer();
    return 3;
}
