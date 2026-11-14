#include "ProxyGenerator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>

namespace VideoEditor {

ProxyGenerator::ProxyGenerator(QString ffmpegPath, QObject *parent)
    : QObject(parent)
    , m_ffmpegPath(std::move(ffmpegPath))
{
}

void ProxyGenerator::start(const QString &source, const QString &output, int height)
{
    if (m_process) {
        emit failed(QStringLiteral("代理任务正在进行。"));
        return;
    }

    m_output = output;
    m_temp = output + QStringLiteral(".tmp.mp4");
    QDir().mkpath(QFileInfo(output).absolutePath());
    QFile::remove(m_temp);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                emit failed(QStringLiteral("无法启动 ffmpeg：%1")
                                .arg(m_process->errorString()));
                m_process->deleteLater();
                m_process = nullptr;
            });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
                if (!m_process)
                    return;
                const QString temp = m_temp;
                const QString outputPath = m_output;
                m_process->deleteLater();
                m_process = nullptr;

                if (status != QProcess::NormalExit || exitCode != 0) {
                    QFile::remove(temp);
                    emit failed(QStringLiteral("代理生成失败（ffmpeg 退出码 %1）。")
                                    .arg(exitCode));
                    return;
                }
                // The generated proxy is written to a temp file first so an
                // interrupted run never leaves a truncated file that the cache
                // would treat as complete.
                QFile::remove(outputPath);
                if (!QFile::rename(temp, outputPath)) {
                    QFile::remove(temp);
                    emit failed(QStringLiteral("无法移动代理文件到缓存目录。"));
                    return;
                }
                emit finished(outputPath);
            });

    // All-intra MJPEG keeps frame-accurate seeking usable while staying small
    // enough for the 540p preview; -an drops audio because M1 has none.
    const QStringList arguments = {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-i"), source,
        QStringLiteral("-vf"), QStringLiteral("scale=-2:%1").arg(height),
        QStringLiteral("-c:v"), QStringLiteral("mjpeg"),
        QStringLiteral("-q:v"), QStringLiteral("3"),
        QStringLiteral("-an"),
        QStringLiteral("-g"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("mp4"),
        m_temp,
    };

    m_process->start(m_ffmpegPath, arguments);
}

void ProxyGenerator::cancel()
{
    if (!m_process)
        return;
    m_process->kill();
}

} // namespace VideoEditor
