#include "AiJobManager.h"

#include "LocalAiTransport.h"

#include <QDebug>
#include <QJsonValue>
#include <QMetaType>

namespace VideoEditor {

AiJobManager::AiJobManager(AiFeatureGate &gate, QObject *parent)
    : QObject(parent)
    , m_gate(gate)
{
    qRegisterMetaType<VideoEditor::AiJobSpec>("VideoEditor::AiJobSpec");
    qRegisterMetaType<VideoEditor::AiJobResult>("VideoEditor::AiJobResult");
}

AiJobManager::~AiJobManager()
{
    // A transport being destroyed may emit finished() (LocalAiTransport kills
    // and waits for its child). Detach it first so the manager never emits
    // jobFinished() while its own members are being torn down.
    m_destroying = true;
    m_running = false;
    if (m_transport) {
        QObject::disconnect(m_transport.get(), nullptr, this, nullptr);
        m_transport.reset();
    }
}

void AiJobManager::setTransportFactoryForTest(
    std::function<std::unique_ptr<AiTransport>()> factory)
{
    m_factory = std::move(factory);
}

bool AiJobManager::start(const AiJobSpec &spec, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!m_gate.enabled())
        return fail(m_gate.disabledReason());
    if (m_running)
        return fail(QStringLiteral("已有 AI 任务在运行。"));
    QString specError;
    if (!AiProtocol::validateJobSpec(spec, &specError))
        return fail(specError);

    m_running = true;
    m_paused = false;
    m_cancelRequested = false;
    m_progress = 0.0;
    m_stageText.clear();
    m_etaText.clear();
    m_errorCode.clear();
    m_errorMessage.clear();
    m_activeJobId = spec.jobId;
    m_cacheKey = spec.cacheKey;
    setErrorText(QString());
    emit runningChanged();
    emit pausedChanged();
    emit progressChanged();
    emit stageTextChanged();
    emit etaTextChanged();

    m_transport = m_factory ? m_factory() : std::make_unique<LocalAiTransport>();
    if (!m_transport) {
        m_running = false;
        m_activeJobId.clear();
        emit runningChanged();
        return fail(QStringLiteral("AI 任务参数无效。"));
    }
    connect(m_transport.get(), &AiTransport::message, this,
            &AiJobManager::handleMessage);
    connect(m_transport.get(), &AiTransport::finished, this,
            &AiJobManager::handleTransportFinished);

    QString startError;
    if (!m_transport->start(spec, &startError)) {
        if (m_transport) {
            AiTransport *failed = m_transport.get();
            QObject::disconnect(failed, nullptr, this, nullptr);
            m_transport.release();
            failed->deleteLater();
        }
        m_running = false;
        m_paused = false;
        m_activeJobId.clear();
        m_cacheKey.clear();
        m_errorCode.clear();
        m_errorMessage.clear();
        setErrorText(QString());
        emit runningChanged();
        emit pausedChanged();
        return fail(startError.isEmpty()
                        ? QStringLiteral("AI 任务参数无效。")
                        : startError);
    }

    if (error)
        error->clear();
    return true;
}

void AiJobManager::cancel()
{
    if (!m_running || !m_transport)
        return;
    m_cancelRequested = true;
    m_stageText = QStringLiteral("正在取消 AI 任务…");
    emit stageTextChanged();
    // The transport owns the cancel command and the kill timeout.
    m_transport->cancel();
}

void AiJobManager::pause()
{
    if (!m_running || m_paused || !m_transport)
        return;
    m_transport->sendCommand(
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("pause")}});
    m_paused = true;
    emit pausedChanged();
}

void AiJobManager::resume()
{
    if (!m_running || !m_paused || !m_transport)
        return;
    m_transport->sendCommand(
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("resume")}});
    m_paused = false;
    emit pausedChanged();
}

void AiJobManager::handleMessage(const QJsonObject &message)
{
    if (m_destroying || !m_running)
        return;
    // Only the transport belonging to the active job may drive this manager.
    // A superseded transport's trailing signals must not abort a job started
    // synchronously from jobFinished().
    if (sender() && sender() != m_transport.get())
        return;

    const QJsonValue jobIdValue = message.value(QStringLiteral("jobId"));
    if (jobIdValue.isString() && !jobIdValue.toString().isEmpty()
        && jobIdValue.toString() != m_activeJobId) {
        return; // message from a superseded job
    }

    const QString event = message.value(QStringLiteral("event")).toString();
    if (event == QLatin1String("progress")) {
        const double percent =
            message.value(QStringLiteral("percent")).toDouble(m_progress);
        const double bounded = qBound(0.0, percent, 100.0);
        if (!qFuzzyCompare(bounded + 1.0, m_progress + 1.0)) {
            m_progress = bounded;
            emit progressChanged();
        }
        const QString stage =
            message.value(QStringLiteral("stage")).toString();
        if (stage != m_stageText) {
            m_stageText = stage;
            emit stageTextChanged();
        }
        QString eta = message.value(QStringLiteral("etaText")).toString();
        if (eta.isEmpty())
            eta = message.value(QStringLiteral("eta")).toString();
        if (eta != m_etaText) {
            m_etaText = eta;
            emit etaTextChanged();
        }
    } else if (event == QLatin1String("confidence")) {
        emit confidenceReady(message);
    } else if (event == QLatin1String("warning")) {
        emit warning(message.value(QStringLiteral("message")).toString());
    } else if (event == QLatin1String("chunkDone")) {
        qInfo("AI chunkDone chunk=%d frame=%d",
              message.value(QStringLiteral("chunk")).toInt(),
              message.value(QStringLiteral("frame")).toInt());
    } else if (event == QLatin1String("error")) {
        m_errorCode = message.value(QStringLiteral("code")).toString();
        m_errorMessage = message.value(QStringLiteral("message")).toString();
        setErrorText(m_errorMessage.isEmpty()
                         ? QStringLiteral("AI 任务失败。")
                         : m_errorMessage);
    } else if (event == QLatin1String("cancelled")) {
        AiJobResult result;
        result.ok = false;
        result.cancelled = true;
        result.cacheKey = m_cacheKey;
        result.completedFrames =
            message.value(QStringLiteral("completedFrames")).toInt();
        result.confidenceHash =
            message.value(QStringLiteral("confidenceHash")).toString();
        const QJsonValue trackingValue =
            message.value(QStringLiteral("trackingState"));
        if (trackingValue.isString()) {
            bool ok = false;
            result.trackingState =
                removalTrackingStateFromString(trackingValue.toString(), &ok);
            if (!ok)
                result.trackingState = RemovalTrackingState::Failed;
        }
        result.errorCode = message.value(QStringLiteral("code")).toString();
        result.errorMessage =
            message.value(QStringLiteral("message")).toString();
        finish(result);
    } else if (event == QLatin1String("done")) {
        const QJsonValue cacheValue =
            message.value(QStringLiteral("cacheKey"));
        if (!cacheValue.isUndefined()
            && (!cacheValue.isString()
                || cacheValue.toString() != m_cacheKey)) {
            // Never let an engine-chosen string become the result cache key.
            AiJobResult result;
            result.ok = false;
            result.cacheKey = m_cacheKey;
            result.errorCode = QStringLiteral("protocol");
            result.errorMessage =
                QStringLiteral("AI 任务结果与当前任务不匹配。");
            finish(result);
            return;
        }

        AiJobResult result;
        result.cacheKey = m_cacheKey;
        result.completedFrames =
            message.value(QStringLiteral("completedFrames")).toInt();
        result.confidenceHash =
            message.value(QStringLiteral("confidenceHash")).toString();
        const QJsonValue trackingValue =
            message.value(QStringLiteral("trackingState"));
        if (trackingValue.isString()) {
            bool ok = false;
            result.trackingState =
                removalTrackingStateFromString(trackingValue.toString(), &ok);
            if (!ok)
                result.trackingState = RemovalTrackingState::Failed;
        }
        // A fatal error event wins over a later done. The Task 4 stub engine
        // reports model_unavailable and then done, so this must fail.
        if (!m_errorCode.isEmpty() || !m_errorMessage.isEmpty()) {
            result.ok = false;
            result.errorCode = m_errorCode;
            result.errorMessage = m_errorMessage;
        } else {
            result.ok = true;
        }
        finish(result);
    }
}

void AiJobManager::handleTransportFinished(int exitCode,
                                           QProcess::ExitStatus status)
{
    if (m_destroying || !m_running)
        return;
    if (sender() && sender() != m_transport.get())
        return;

    AiJobResult result;
    result.cacheKey = m_cacheKey;
    if (m_cancelRequested) {
        result.ok = false;
        result.cancelled = true;
        result.errorCode = m_errorCode;
        result.errorMessage = m_errorMessage;
    } else if (!m_errorCode.isEmpty()) {
        // A protocol/engine error was already reported. Do not overwrite it
        // with the generic crash text when the child is killed after the
        // error (kill() makes QProcess report CrashExit).
        result.ok = false;
        result.errorCode = m_errorCode;
        result.errorMessage = m_errorMessage;
    } else if (!m_errorMessage.isEmpty()) {
        result.ok = false;
        result.errorCode = QStringLiteral("engine");
        result.errorMessage = m_errorMessage;
    } else if (status == QProcess::CrashExit) {
        result.ok = false;
        result.errorCode = QStringLiteral("crash");
        result.errorMessage = QStringLiteral(
            "AI 推理进程已退出，已完成的分段已保留，可重试。");
    } else {
        result.ok = false;
        result.errorCode = QStringLiteral("engine");
        result.errorMessage =
            QStringLiteral("AI 任务失败（退出码 %1）。").arg(exitCode);
    }
    finish(result);
}

void AiJobManager::finish(const AiJobResult &result)
{
    if (m_destroying || !m_running)
        return;
    m_running = false;
    m_paused = false;
    m_cancelRequested = false;
    if (result.ok)
        m_progress = 100.0;

    // The transport is emitting its own signal when finish() runs. Detach it
    // before deferring deletion so a trailing done/finished from the old
    // transport can never touch the next job.
    if (m_transport) {
        AiTransport *transport = m_transport.get();
        QObject::disconnect(transport, nullptr, this, nullptr);
        m_transport.release();
        transport->deleteLater();
    }
    m_activeJobId.clear();
    m_cacheKey.clear();

    emit runningChanged();
    emit pausedChanged();
    emit progressChanged();
    emit jobFinished(result);
}

void AiJobManager::setErrorText(const QString &message)
{
    if (message == m_error)
        return;
    m_error = message;
    emit errorChanged();
}

} // namespace VideoEditor
