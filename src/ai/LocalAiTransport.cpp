#include "LocalAiTransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QUuid>
#include <QVariant>

namespace VideoEditor {

namespace {

QString enginePath()
{
    const QString override = qEnvironmentVariable("VIDEOEDITOR_AI_ENGINE");
    if (!override.isEmpty())
        return override;
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("VisionAiEngine.exe"));
}

QString newServerName()
{
    return QStringLiteral("vem4-")
           + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void stripProxyEnvironment(QProcessEnvironment &environment)
{
    static const char *const names[] = {
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy",
    };
    for (const char *name : names)
        environment.remove(QString::fromLatin1(name));
}

bool keepTempFiles()
{
    return qEnvironmentVariable("VIDEOEDITOR_KEEP_AI_TEMP") == QLatin1String("1");
}

} // namespace

LocalAiTransport::LocalAiTransport(QObject *parent)
    : AiTransport(parent)
{
    m_cancelTimer = new QTimer(this);
    m_cancelTimer->setSingleShot(true);
    connect(m_cancelTimer, &QTimer::timeout, this,
            &LocalAiTransport::onCancelTimeout);
}

LocalAiTransport::~LocalAiTransport()
{
    // Killing the child makes QProcess emit finished(); never let that run
    // through this object during destruction.
    m_finishedEmitted = true;
    blockSignals(true);
    if (m_cancelTimer)
        m_cancelTimer->stop();
    if (m_process) {
        QObject::disconnect(m_process, nullptr, this, nullptr);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    if (m_socket)
        QObject::disconnect(m_socket, nullptr, this, nullptr);
    if (m_server)
        m_server->close();
    if (!keepTempFiles() && !m_jobPath.isEmpty())
        QFile::remove(m_jobPath);
}

bool LocalAiTransport::start(const AiJobSpec &spec, QString *error)
{
    m_buffer.clear();
    m_finishedEmitted = false;
    m_jobPath.clear();

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QString executable = enginePath();
    if (!QFileInfo::exists(executable))
        return fail(QStringLiteral("未找到 AI 推理引擎程序：%1。")
                        .arg(executable));

    const QString jobPath =
        QDir::temp().filePath(QStringLiteral("m4-job-%1.json")
                                  .arg(QUuid::createUuid().toString(
                                      QUuid::WithoutBraces)));
    QFile jobFile(jobPath);
    if (!jobFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail(QStringLiteral("无法写入 AI 任务文件：%1。")
                        .arg(jobFile.errorString()));
    const QByteArray payload =
        QJsonDocument(aiJobSpecToJson(spec)).toJson(QJsonDocument::Compact);
    if (jobFile.write(payload) != payload.size()) {
        const QString writeError = jobFile.errorString();
        jobFile.close();
        QFile::remove(jobPath);
        return fail(QStringLiteral("无法写入 AI 任务文件：%1。").arg(writeError));
    }
    jobFile.close();
    m_jobPath = jobPath;

    m_serverName = newServerName();
    if (!m_server) {
        m_server = new QLocalServer(this);
        connect(m_server, &QLocalServer::newConnection, this,
                &LocalAiTransport::onNewConnection);
    } else {
        m_server->close();
    }
    QLocalServer::removeServer(m_serverName);
    // Windows ignores the access option, but it keeps the socket private to
    // the current user on platforms that honour it.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_serverName)) {
        const QString listenError = m_server->errorString();
        cleanup(true);
        return fail(QStringLiteral("无法启动 AI 通信服务：%1。").arg(listenError));
    }

    if (!m_process) {
        m_process = new QProcess(this);
        connect(m_process, &QProcess::finished, this,
                &LocalAiTransport::onProcessFinished);
        connect(m_process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError processError) {
                    if (processError == QProcess::FailedToStart)
                        failProtocol(QStringLiteral("无法启动 AI 推理引擎。"));
                });
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    stripProxyEnvironment(environment);
    m_process->setProcessEnvironment(environment);
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    m_process->setProgram(executable);
    m_process->setArguments(
        {QStringLiteral("--job"), m_jobPath, QStringLiteral("--socket"),
         m_serverName});
    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        const QString startError = m_process->errorString();
        cleanup(true);
        return fail(QStringLiteral("无法启动 AI 推理引擎：%1。").arg(startError));
    }

    if (error)
        error->clear();
    return true;
}

void LocalAiTransport::cancel()
{
    if (m_socket && m_socket->state() == QLocalSocket::ConnectedState) {
        m_socket->write(AiProtocol::encodeLine(
            QJsonObject{{QStringLiteral("cmd"), QStringLiteral("cancel")}}));
        m_socket->flush();
    }
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_cancelTimer->start(5000);
}

void LocalAiTransport::sendCommand(const QJsonObject &command)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
        return;
    m_socket->write(AiProtocol::encodeLine(command));
    m_socket->flush();
}

void LocalAiTransport::onNewConnection()
{
    if (!m_server)
        return;
    while (m_server->hasPendingConnections()) {
        QLocalSocket *connection = m_server->nextPendingConnection();
        if (!connection)
            continue;
        if (m_socket) {
            connection->deleteLater();
            continue;
        }
        m_socket = connection;
        m_socket->setParent(this);
        m_buffer.clear();
        connect(m_socket, &QLocalSocket::readyRead, this,
                &LocalAiTransport::onReadyRead);
        connect(m_socket, &QObject::destroyed, this, [this] { m_socket = nullptr; });
    }
}

void LocalAiTransport::onReadyRead()
{
    if (!m_socket)
        return;
    m_buffer.append(m_socket->readAll());

    int newline = -1;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);

        QJsonObject message;
        QString error;
        if (!AiProtocol::parseLine(line, &message, &error)) {
            failProtocol(error);
            return;
        }
        emit this->message(message);
    }

    if (m_buffer.size() > AiProtocol::kMaxLineBytes)
        failProtocol(QStringLiteral("AI 引擎输出超出缓冲区上限。"));
}

void LocalAiTransport::onProcessFinished(int exitCode,
                                         QProcess::ExitStatus status)
{
    if (m_finishedEmitted)
        return;
    m_finishedEmitted = true;

    // A fast child (the Task 4 stub included) can connect, write its final
    // lines and exit before the parent event loop dispatches newConnection.
    // Accept the pending connection and drain the pipe before tearing down,
    // otherwise the trailing done/error would be lost.
    if (!m_socket && m_server) {
        if (!m_server->hasPendingConnections())
            m_server->waitForNewConnection(250);
        if (m_server->hasPendingConnections())
            onNewConnection();
    }
    if (m_socket) {
        QElapsedTimer drainClock;
        drainClock.start();
        while (m_socket) {
            if (m_socket->bytesAvailable() > 0) {
                onReadyRead();
                continue;
            }
            if (m_socket->state() == QLocalSocket::UnconnectedState)
                break;
            if (!m_socket->waitForReadyRead(200))
                break;
            if (drainClock.elapsed() > 2000)
                break;
        }
    }

    if (m_cancelTimer)
        m_cancelTimer->stop();
    cleanup(true);
    emit finished(exitCode, status);
}

void LocalAiTransport::onCancelTimeout()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->kill();
}

void LocalAiTransport::failProtocol(const QString &message)
{
    emit this->message(
        QJsonObject{{QStringLiteral("event"), QStringLiteral("error")},
                    {QStringLiteral("code"), QStringLiteral("protocol")},
                    {QStringLiteral("message"), message}});
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->kill();
}

void LocalAiTransport::cleanup(bool removeJobFile)
{
    if (m_cancelTimer)
        m_cancelTimer->stop();
    if (m_server)
        m_server->close();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_buffer.clear();
    if (removeJobFile && !m_jobPath.isEmpty() && !keepTempFiles()) {
        QFile::remove(m_jobPath);
        m_jobPath.clear();
    }
}

} // namespace VideoEditor
