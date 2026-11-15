// Test-only subprocess used by LocalAiTransport tests. It speaks the same
// NDJSON protocol as VisionAiEngine but can emit malformed or oversized data.
// The mode is encoded in the job's assetPath so LocalAiTransport needs no
// extra arguments.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStringList>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("job"), QStringLiteral("job spec"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("socket"), QStringLiteral("local socket"),
                      QStringLiteral("name")});
    parser.process(app);

    const QString jobPath = parser.value(QStringLiteral("job"));
    const QString socketName = parser.value(QStringLiteral("socket"));

    QString mode;
    QFile jobFile(jobPath);
    if (jobFile.open(QIODevice::ReadOnly)) {
        mode = QJsonDocument::fromJson(jobFile.readAll())
                   .object()
                   .value(QStringLiteral("assetPath"))
                   .toString();
    }

    QLocalSocket socket;
    socket.connectToServer(socketName);
    if (!socket.waitForConnected(5000))
        return 3;

    QByteArray payload;
    if (mode.contains(QStringLiteral("oversized"))) {
        payload = QByteArray(2 * 1024 * 1024, 'x');
        payload.append('\n');
    } else if (mode.contains(QStringLiteral("invalid"))) {
        payload = QByteArrayLiteral("{not json}\n");
    } else if (mode.contains(QStringLiteral("partial"))) {
        payload = QByteArrayLiteral("{\"event\":\"done\"");
    } else {
        const QJsonObject done{
            {QStringLiteral("event"), QStringLiteral("done")},
            {QStringLiteral("cacheKey"), QString(64, QLatin1Char('a'))}};
        payload = QJsonDocument(done).toJson(QJsonDocument::Compact);
        payload.append('\n');
    }

    socket.write(payload);
    socket.waitForBytesWritten(5000);
    socket.disconnectFromServer();
    return 0;
}
