#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include "AppController.h"
#include "AppInfo.h"
#include "AudioEngine.h"
#include "PreviewController.h"
#include "TimelineController.h"
#include "VideoPreviewItem.h"
#include "WaveformImageProvider.h"

int main(int argc, char *argv[])
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("VideoEditor"));
    QGuiApplication::setApplicationName(QStringLiteral("VideoEditor"));

    QCommandLineParser parser;
    parser.addOption({QStringLiteral("selftest"),
                      QStringLiteral("Open a file and exit after first frame"),
                      QStringLiteral("path")});
    parser.process(app);

    VideoEditor::AppController controller;

    // A clean quit must not leave an autosave behind: a later launch would
    // offer an older snapshot and could roll back edits saved after it.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller,
                     &VideoEditor::AppController::discardRecoveryIfClean);

    qmlRegisterType<VideoEditor::VideoPreviewItem>("VideoEditor", 1, 0,
                                                  "VideoPreview");

    QQmlApplicationEngine engine;
    engine.addImageProvider(
        QStringLiteral("waveform"),
        new VideoEditor::WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             VideoEditor::appVersion());
    engine.loadFromModule(QStringLiteral("VideoEditor"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty())
        return -1;

    if (parser.isSet(QStringLiteral("selftest"))) {
        QObject::connect(&controller,
                         &VideoEditor::AppController::selftestFramePresented,
                         &app, [] {
                             qInfo("SELFTEST_FRAME_OK");
                             QCoreApplication::exit(0);
                         });
        QTimer::singleShot(0, &controller, [&controller, &parser] {
            controller.selftest(parser.value(QStringLiteral("selftest")));
        });
        QTimer::singleShot(15000, &app, [] {
            qCritical("SELFTEST_TIMEOUT");
            QCoreApplication::exit(2);
        });
    }

    return app.exec();
}
