// src/ai/AiEngineMain.cpp
//
// Local inference subprocess. It reads only the AiJobSpec, talks to the parent
// over a local socket and never opens the project file or the network.
//
// With ONNX Runtime and OpenCV available (VIDEOEDITOR_HAVE_ORT=1) it runs the
// Task 5 tracking pipeline (EfficientTAM keyframes + DIS propagation). The
// repair pipeline belongs to Task 6: a non-tracking-only job reports
// inpaint_unavailable after tracking. Without the runtimes this file keeps the
// Task 4 stub behaviour so the protocol tests still have a real executable.

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QThread>

#include <functional>

#include "AiProtocol.h"

#if VIDEOEDITOR_HAVE_ORT
#include "ModelRegistry.h"
#include "engine/Sam2Tracker.h"
#include "engine/TrackingPipeline.h"
#endif

namespace {

using namespace VideoEditor;

// "SendMessage" is a Windows macro, so the sink alias deliberately avoids it.
using MessageSink = std::function<void(const QJsonObject &)>;

#if VIDEOEDITOR_HAVE_ORT

// Reads newline-delimited commands from the parent without installing an event
// loop, so the pipeline can poll once per frame.
class CommandPoller {
public:
    explicit CommandPoller(QLocalSocket &socket)
        : m_socket(socket)
    {
    }

    // Returns true when a cancel arrived. Pause/resume update `paused` in
    // arrival order, so a pause+resume batch ends unpaused. Cancel always
    // wins. Reading without an event loop needs an explicit wait: on Windows a
    // named pipe may report 0 bytesAvailable() until the read is attempted.
    bool poll(bool &paused)
    {
        if (m_socket.state() != QLocalSocket::ConnectedState)
            return true;
        if (m_socket.bytesAvailable() == 0)
            m_socket.waitForReadyRead(1);
        if (m_socket.bytesAvailable() > 0)
            m_buffer.append(m_socket.readAll());
        if (m_buffer.size() > AiProtocol::kMaxLineBytes) {
            m_buffer.clear();
            return true;
        }

        bool cancel = false;
        int newline = -1;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            QJsonObject message;
            if (!AiProtocol::parseLine(line, &message, nullptr))
                continue;
            const QString cmd = message.value(QStringLiteral("cmd")).toString();
            if (cmd == QLatin1String("cancel"))
                cancel = true;
            else if (cmd == QLatin1String("pause"))
                paused = true;
            else if (cmd == QLatin1String("resume"))
                paused = false;
        }
        return cancel;
    }

private:
    QLocalSocket &m_socket;
    QByteArray m_buffer;
};

int runRealEngine(const AiJobSpec &spec, QLocalSocket &socket,
                  const MessageSink &send)
{
    const bool technical =
        qEnvironmentVariable("VIDEOEDITOR_M4_TECHNICAL") == QLatin1String("1");
    if (!technical) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("model_unavailable")},
            {QStringLiteral("message"), QStringLiteral("AI 模型尚未安装。")}});
        send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                         {QStringLiteral("cacheKey"), spec.cacheKey},
                         {QStringLiteral("completedFrames"), 0}});
        return 3;
    }

    const auto modelUnavailable = [&](const QString &message) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("model_unavailable")},
            {QStringLiteral("message"), message}});
        send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                         {QStringLiteral("cacheKey"), spec.cacheKey},
                         {QStringLiteral("completedFrames"), 0}});
        return 3;
    };

    QString error;
    const auto registry = ModelRegistry::load(spec.modelsManifestPath, &error);
    if (!registry) {
        return modelUnavailable(
            QStringLiteral("无法加载模型清单：%1").arg(error));
    }
    const auto offlineCapable = [](const AiModelRecord &model) {
        return model.files.contains(QStringLiteral("image_encoder"))
            && model.files.contains(QStringLiteral("prompt_encoder"))
            && model.files.contains(QStringLiteral("mask_decoder"));
    };

    const AiModelRecord *record = registry->model(spec.trackingModelId);
    if (!record || !record->isTracking()) {
        return modelUnavailable(QStringLiteral("跟踪模型不可用：%1。")
                                    .arg(spec.trackingModelId));
    }
    if (!offlineCapable(*record)) {
        // The manifest's first/default tracker is the SAM 2 bundle, which has
        // no prompt_encoder. Fall back to the offline-capable default (fp32
        // EfficientTAM) with a warning instead of failing the job.
        const AiModelRecord *fallback = registry->defaultTrackingModel();
        if (!fallback || fallback == record || !offlineCapable(*fallback)) {
            return modelUnavailable(
                QStringLiteral("跟踪模型 %1 缺少离线关键帧组件。")
                    .arg(record->id));
        }
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("warning")},
            {QStringLiteral("message"),
             QStringLiteral("请求的跟踪模型 %1 缺少离线关键帧组件，已改用 %2。")
                 .arg(record->id, fallback->id)}});
        record = fallback;
    }

    // Hash only the record the engine will load, not every manifest model.
    if (!registry->verifyFiles({record->id}, &error)) {
        return modelUnavailable(
            QStringLiteral("模型文件校验失败：%1").arg(error));
    }

    TrackingConfig config;
    config.io = record->io;
    config.allowDml = false;
    config.allowCpu = true;
    for (auto it = record->files.constBegin(); it != record->files.constEnd();
         ++it) {
        if (!it.value().path.endsWith(QLatin1String(".onnx"),
                                      Qt::CaseInsensitive)) {
            continue;
        }
        config.partPaths.insert(
            it.key(), QDir(registry->rootDir()).filePath(it.value().path));
    }
    for (const QString &part :
         {QStringLiteral("image_encoder"), QStringLiteral("prompt_encoder"),
          QStringLiteral("mask_decoder")}) {
        if (!config.partPaths.contains(part)) {
            return modelUnavailable(
                QStringLiteral("跟踪模型缺少组件：%1。").arg(part));
        }
    }

    Sam2Tracker tracker;
    if (!tracker.initialize(config, &error)) {
        return modelUnavailable(
            QStringLiteral("无法加载跟踪模型：%1").arg(error));
    }

    TrackingPipeline pipeline;
    pipeline.setTracker(&tracker);

    CommandPoller poller(socket);
    bool paused = false;
    TrackingRunCallbacks callbacks;
    callbacks.event = [&send](const QJsonObject &message) { send(message); };
    callbacks.cancel = [&]() -> bool {
        if (poller.poll(paused))
            return true;
        while (paused) {
            if (poller.poll(paused))
                return true;
            if (!paused)
                break;
            QThread::msleep(20);
        }
        return false;
    };

    const TrackingRunResult result = pipeline.run(spec, callbacks);
    if (result.cancelled) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("cancelled")},
            {QStringLiteral("completedFrames"), result.completedFrames},
            {QStringLiteral("confidenceHash"), result.confidenceHash},
            {QStringLiteral("trackingState"),
             removalTrackingStateToString(result.state)}});
        return 0;
    }
    if (!result.ok) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("tracking_failed")},
            {QStringLiteral("message"), result.error}});
        return 4;
    }
    if (!spec.trackingOnly) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("inpaint_unavailable")},
            {QStringLiteral("message"),
             QStringLiteral("修复流水线尚未实现。")}});
        return 5;
    }

    send(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("cacheKey"), spec.cacheKey},
        {QStringLiteral("confidenceHash"), result.confidenceHash},
        {QStringLiteral("trackingState"),
         removalTrackingStateToString(result.state)},
        {QStringLiteral("completedFrames"), result.completedFrames}});
    return 0;
}

#endif // VIDEOEDITOR_HAVE_ORT

} // namespace

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
    if (!VideoEditor::AiProtocol::validateJobSpec(*spec, &error))
        return 2;

    QLocalSocket socket;
    socket.connectToServer(socketName);
    if (!socket.waitForConnected(5000))
        return 3;

    const auto send = [&socket](const QJsonObject &message) {
        socket.write(VideoEditor::AiProtocol::encodeLine(message));
        socket.flush();
    };

    int exitCode = 3;
#if VIDEOEDITOR_HAVE_ORT
    exitCode = runRealEngine(*spec, socket, send);
#else
    // Task 4 stub: the real ONNX runtime is not available in this build.
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("error")},
                     {QStringLiteral("code"),
                      QStringLiteral("model_unavailable")},
                     {QStringLiteral("message"),
                      QStringLiteral("AI 模型尚未安装。")}});
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                     {QStringLiteral("cacheKey"), spec->cacheKey},
                     {QStringLiteral("completedFrames"), 0}});
    exitCode = 3;
#endif

    // flush() only queues the bytes; the process must not exit (closing the
    // pipe) before the parent has actually received them.
    socket.waitForBytesWritten(5000);
    socket.disconnectFromServer();
    return exitCode;
}
