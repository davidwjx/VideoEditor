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
#include "ExportController.h"
#include "MaskImageProvider.h"
#include "PreviewController.h"
#include "RemovalController.h"
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
    parser.addOption(
        {QStringLiteral("selftest-timeline"),
         QStringLiteral("Play the multi-track timeline fixture and exit after "
                        "30 presented frames")});
    parser.addOption(
        {QStringLiteral("selftest-perf"),
         QStringLiteral("Measure presented timeline frames and their video "
                        "layer count for five seconds; exit 0 at >=24 fps "
                        "with all three layers")});
    parser.addOption({QStringLiteral("selftest-export"),
                      QStringLiteral("Export the multi-track fixture project "
                                     "with every available codec and exit")});
    parser.addOption(
        {QStringLiteral("selftest-removal"),
         QStringLiteral("Run one real Fast/MI-GAN removal job on the removal "
                        "acceptance fixture, verify the cache and exit")});
    parser.process(app);

    VideoEditor::AppController controller;

    // A clean quit must not leave an autosave behind: a later launch would
    // offer an older snapshot and could roll back edits saved after it.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller,
                     &VideoEditor::AppController::discardRecoveryIfClean);

    // The export self-test is a headless CLI gate: it renders through its own
    // D3D11 device and never touches the QML scene graph, so not creating a
    // window keeps it independent of an interactive desktop session.
    if (parser.isSet(QStringLiteral("selftest-export"))) {
        QTimer::singleShot(0, &controller, [&controller] {
            controller.selftestExport();
        });
        return app.exec();
    }

    // The removal self-test decodes with FFmpeg and runs the engine as a child
    // process; it needs no QML scene graph or window.
    if (parser.isSet(QStringLiteral("selftest-removal"))) {
        QTimer::singleShot(0, &controller, [&controller] {
            controller.selftestRemoval();
        });
        return app.exec();
    }

    qmlRegisterType<VideoEditor::VideoPreviewItem>("VideoEditor", 1, 0,
                                                  "VideoPreview");

    QQmlApplicationEngine engine;
    engine.addImageProvider(
        QStringLiteral("waveform"),
        new VideoEditor::WaveformImageProvider(controller.waveforms()));
    // The engine owns the provider; the controller keeps a non-owning pointer
    // so every mask edit can replace the overlay image.
    auto *maskProvider = new VideoEditor::MaskImageProvider();
    controller.removal()->setMaskImageProvider(maskProvider);
    engine.addImageProvider(QStringLiteral("mask"), maskProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Removal"),
                                             controller.removal());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("Export"),
                                             controller.exportController());
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

    if (parser.isSet(QStringLiteral("selftest-timeline"))) {
        int presentedFrames = 0;
        QObject::connect(&controller,
                         &VideoEditor::AppController::timelineFramePresented,
                         &app, [&app, &presentedFrames] {
                             if (++presentedFrames >= 30) {
                                 qInfo("TIMELINE_SELFTEST_OK frames=%d",
                                       presentedFrames);
                                 QCoreApplication::exit(0);
                             }
                         });
        QTimer::singleShot(0, &controller, [&controller] {
            controller.selftestTimeline();
        });
        QTimer::singleShot(15000, &app, [] {
            qCritical("TIMELINE_SELFTEST_TIMEOUT");
            QCoreApplication::exit(2);
        });
    }

    if (parser.isSet(QStringLiteral("selftest-perf"))) {
        // AppController::selftestPerf owns the clock, frame count, threshold
        // and exit code so the gate can also be driven from tests.
        QTimer::singleShot(0, &controller, [&controller] {
            controller.selftestPerf();
        });
    }

    return app.exec();
}
