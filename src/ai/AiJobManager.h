// src/ai/AiJobManager.h
#pragma once

#include <functional>
#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "AiProtocol.h"
#include "AiTransport.h"
#include "AiFeatureGate.h"

namespace VideoEditor {

// Runs at most one removal job at a time. It validates the feature gate and
// cache key, forwards transport messages into QML-friendly properties and
// produces exactly one jobFinished() per started job.
class AiJobManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stageText READ stageText NOTIFY stageTextChanged)
    Q_PROPERTY(QString etaText READ etaText NOTIFY etaTextChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit AiJobManager(AiFeatureGate &gate, QObject *parent = nullptr);
    ~AiJobManager() override;

    bool running() const { return m_running; }
    bool paused() const { return m_paused; }
    double progress() const { return m_progress; }
    QString stageText() const { return m_stageText; }
    QString etaText() const { return m_etaText; }
    QString error() const { return m_error; }

    bool start(const AiJobSpec &spec, QString *error = nullptr);
    void cancel();
    void pause();
    void resume();
    void setTransportFactoryForTest(
        std::function<std::unique_ptr<AiTransport>()> factory);

signals:
    void runningChanged();
    void pausedChanged();
    void progressChanged();
    void stageTextChanged();
    void etaTextChanged();
    void errorChanged();
    void warning(const QString &message);
    void confidenceReady(const QJsonObject &confidence);
    void jobFinished(const VideoEditor::AiJobResult &result);

private:
    void handleMessage(const QJsonObject &message);
    void handleTransportFinished(int exitCode, QProcess::ExitStatus status);
    void finish(const AiJobResult &result);
    void setErrorText(const QString &message);

    AiFeatureGate &m_gate;
    std::unique_ptr<AiTransport> m_transport;
    std::function<std::unique_ptr<AiTransport>()> m_factory;
    QString m_activeJobId;
    QString m_cacheKey;
    bool m_running = false;
    bool m_paused = false;
    bool m_cancelRequested = false;
    double m_progress = 0.0;
    QString m_stageText;
    QString m_etaText;
    QString m_error;
    QString m_errorCode;
    QString m_errorMessage;
};

} // namespace VideoEditor
