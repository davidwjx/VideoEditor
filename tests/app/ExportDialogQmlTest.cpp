#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QUrl>

#include "AppController.h"
#include "AudioSink.h"
#include "ExportController.h"
#include "ExportEncoderCatalog.h"
#include "PreviewController.h"
#include "VideoPreviewItem.h"
#include "WaveformImageProvider.h"

using namespace VideoEditor;

namespace {

// Main.qml instantiates VideoPreview, which main.cpp registers at runtime.
void registerPreviewType()
{
    static const bool registered = [] {
        qmlRegisterType<VideoPreviewItem>("VideoEditor", 1, 0,
                                          "VideoPreview");
        return true;
    }();
    Q_UNUSED(registered);
}

AudioEngine::SinkFactory nullSinkFactory()
{
    return [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
}

} // namespace

TEST(ExportQml, DialogLoadsAndShowsH265DisabledReason)
{
    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    ExportController *exportController = controller.exportController();
    ASSERT_NE(exportController, nullptr);
    exportController->setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    exportController->refreshCapabilities();
    exportController->setCodec(QStringLiteral("h265"));

    QQmlEngine engine;
    engine.addImageProvider(
        QStringLiteral("waveform"),
        new WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Export"),
                                             exportController);
    engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                             controller.preview());
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QStringLiteral("test"));

    // Load Main.qml from the source tree, like the timeline QML tests: the
    // module resources are linked into the app executable, not this test.
    // ExportDialog.qml resolves as a sibling component.
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(VIDEOEDITOR_SOURCE_DIR
                                           "/src/app/Main.qml")));
    QScopedPointer<QObject> root(component.create());
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    ASSERT_NE(root.data(), nullptr);
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    ASSERT_NE(window, nullptr);

    // The top-right entry opens the export dialog without disturbing the
    // editing toolbar.
    QObject *exportButton = window->findChild<QObject *>(
        QStringLiteral("exportButton"));
    ASSERT_NE(exportButton, nullptr);
    EXPECT_EQ(exportButton->property("text").toString(), QStringLiteral("导出"));

    QObject *dialog = window->findChild<QObject *>(
        QStringLiteral("exportDialog"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openDialog"));
    QCoreApplication::processEvents();

    QObject *combo = window->findChild<QObject *>(QStringLiteral("codecCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->property("currentIndex").toInt(), 1); // H.265 row
    QObject *reason = window->findChild<QObject *>(QStringLiteral("h265Reason"));
    ASSERT_NE(reason, nullptr);
    EXPECT_TRUE(reason->property("visible").toBool());
    EXPECT_NE(reason->property("text").toString().indexOf(QStringLiteral("H.265")), -1);
    EXPECT_TRUE(exportController->h265UnavailableReason()
                    .contains(QStringLiteral("H.265")));

    // The dialog keeps only the common presets visible and collapses advanced
    // encoding controls until asked.
    QObject *presetCombo = window->findChild<QObject *>(
        QStringLiteral("presetCombo"));
    ASSERT_NE(presetCombo, nullptr);
    EXPECT_EQ(presetCombo->property("count").toInt(), 4);
    EXPECT_FALSE(presetCombo->property("currentText").toString().isEmpty());
    QObject *advanced = window->findChild<QObject *>(
        QStringLiteral("advancedPanel"));
    ASSERT_NE(advanced, nullptr);
    EXPECT_FALSE(advanced->property("visible").toBool());
    EXPECT_GT(dialog->property("height").toDouble(), 200.0);
    dialog->setProperty("advancedOpen", true);
    QCoreApplication::processEvents();
    EXPECT_TRUE(advanced->property("visible").toBool());

    // H.265 without hardware blocks the start button.
    QObject *start = window->findChild<QObject *>(
        QStringLiteral("startExportButton"));
    ASSERT_NE(start, nullptr);
    EXPECT_FALSE(start->property("enabled").toBool());

    // Regression: choosing H.265 and then applying an H.264 preset must leave
    // the visible combo and the encoded settings consistent. Exercise both
    // directions: user selection writes the controller, and a preset write
    // updates the visible selection again.
    exportController->setCodec(QStringLiteral("h264"));
    QCoreApplication::processEvents();
    EXPECT_EQ(combo->property("currentIndex").toInt(), 0);
    combo->setProperty("currentIndex", 1);
    QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1));
    QCoreApplication::processEvents();
    EXPECT_EQ(exportController->codec(), QStringLiteral("h265"));
    EXPECT_FALSE(start->property("enabled").toBool());
    exportController->applyPreset(QStringLiteral("h264-720p30"));
    QCoreApplication::processEvents();
    EXPECT_EQ(combo->property("currentIndex").toInt(), 0);
    EXPECT_EQ(exportController->codec(), QStringLiteral("h264"));
    EXPECT_TRUE(start->property("enabled").toBool());
    EXPECT_TRUE(presetCombo->property("currentText").toString()
                    .contains(QStringLiteral("720p")));

    // Regression: a path chosen through "浏览…" replaces typed text instead of
    // leaving the field showing the stale value.
    QObject *outputField = window->findChild<QObject *>(
        QStringLiteral("outputPathField"));
    ASSERT_NE(outputField, nullptr);
    outputField->setProperty("text", QStringLiteral("D:/typed/out.mp4"));
    exportController->setOutputPath(QStringLiteral("D:/browsed/out.mp4"));
    QCoreApplication::processEvents();
    EXPECT_EQ(outputField->property("text").toString(),
              QStringLiteral("D:/browsed/out.mp4"));

    // Same rule for the advanced bitrate field.
    QObject *bitrateField = window->findChild<QObject *>(
        QStringLiteral("bitrateField"));
    ASSERT_NE(bitrateField, nullptr);
    bitrateField->setProperty("text", QStringLiteral("1234"));
    exportController->setBitrateKbps(2222);
    QCoreApplication::processEvents();
    EXPECT_EQ(bitrateField->property("text").toString(), QStringLiteral("2222"));

    // Fractional rates must not be flattened to a rounded integer.
    exportController->setFpsNum(30000);
    exportController->setFpsDen(1001);
    QCoreApplication::processEvents();
    QObject *fpsValue = window->findChild<QObject *>(QStringLiteral("fpsValue"));
    ASSERT_NE(fpsValue, nullptr);
    EXPECT_TRUE(fpsValue->property("text").toString()
                    .contains(QStringLiteral("30000/1001")));

    // R1: closing the window and Esc-dismissing the dialog are both blocked
    // while the export runs. The injected runner observes the state mid-run.
    window->show();
    QCoreApplication::processEvents();
    const int idlePolicy = dialog->property("closePolicy").toInt();
    bool wasRunning = false;
    bool windowStayedOpen = false;
    int runningPolicy = -1;
    exportController->setSyncRunnerForTest(
        [&](const Project &, const ExportSettings &, bool,
            const std::function<void(const ExportProgress &)> &) {
            wasRunning = exportController->running();
            QCoreApplication::processEvents();
            runningPolicy = dialog->property("closePolicy").toInt();
            window->close();
            windowStayedOpen = window->isVisible();
            ExportResult result;
            result.ok = true;
            result.outputPath = exportController->outputPath();
            result.validation.ok = true;
            return result;
        });
    exportController->startExport();
    ASSERT_TRUE(wasRunning);
    EXPECT_NE(runningPolicy, idlePolicy);
    EXPECT_TRUE(windowStayedOpen);
    EXPECT_FALSE(exportController->running());
    QCoreApplication::processEvents();
    EXPECT_EQ(dialog->property("closePolicy").toInt(), idlePolicy);

    // With the export finished, the same close request is accepted.
    window->close();
    QCoreApplication::processEvents();
    EXPECT_FALSE(window->isVisible());
}
