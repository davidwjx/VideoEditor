#include "AiJobManager.h"

#include "LocalAiTransport.h"

#include <QDebug>
#include <QJsonValue>
#include <QMetaType>
#include <QRegularExpression>

namespace VideoEditor {

namespace {

bool isLowerHex64(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(text).hasMatch();
}

} // namespace

AiJobManager::AiJobManager(AiFeatureGate &gate, QObject *parent)
    : QObject(parent)
    , m_gate(gate)
{
    qRegisterMetaType<VideoEditor::AiJobSpec>("VideoEditor::AiJobSpec");
    qRegisterMetaType<VideoEditor::AiJobResult>("VideoEditor::AiJobResult");
}

AiJobManager::~AiJobManager() = default;

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
    if (!isLowerHex64(spec.cacheKey))
        return fail(QStringLiteral("AI 任务参数无效。"));

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
            AiTransport *failed = m_transport.release();
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
    if (!m_running)
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
        result.errorCode = message.value(QStringLiteral("code")).toString();
        result.errorMessage =
            message.value(QStringLiteral("message")).toString();
        finish(result);
    } else if (event == QLatin1String("done")) {
        AiJobResult result;
        result.cacheKey = message.value(QStringLiteral("cacheKey"))
                              .toString(m_cacheKey);
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
    if (!m_running)
        return;

    AiJobResult result;
    result.cacheKey = m_cacheKey;
    if (m_cancelRequested) {
        result.ok = false;
        result.cancelled = true;
        result.errorCode = m_errorCode;
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
            !m_errorMessage.isEmpty()
                ? m_errorMessage
                : QStringLiteral("AI 任务失败（退出码 %1）。").arg(exitCode);
    }
    finish(result);
}

void AiJobManager::finish(const AiJobResult &result)
{
    if (!m_running)
        return;
    m_running = false;
    m_paused = false;
    m_cancelRequested = false;
    if (result.ok)
        m_progress = 100.0;

    // The transport is emitting its own signal when finish() runs, so defer
    // its deletion instead of destroying it inside the callback.
    if (m_transport) {
        AiTransport *transport = m_transport.release();
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
