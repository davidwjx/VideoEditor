#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>
#include <QUrl>

#include "AppController.h"
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

} // namespace

TEST(ExportQml, DialogLoadsAndShowsH265DisabledReason)
{
    registerPreviewType();

    AppController controller;
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
    const double compactHeight = dialog->property("height").toDouble();
    EXPECT_GT(compactHeight, 200.0);

    dialog->setProperty("advancedOpen", true);
    QTest::qWait(150); // let the dialog relayout with the expanded panel
    EXPECT_TRUE(advanced->property("visible").toBool());
    EXPECT_GT(dialog->property("height").toDouble(), compactHeight);

    // H.265 without hardware blocks the start button, and switching back to
    // H.264 immediately restores it.
    QObject *start = window->findChild<QObject *>(
        QStringLiteral("startExportButton"));
    ASSERT_NE(start, nullptr);
    EXPECT_FALSE(start->property("enabled").toBool());
    exportController->setCodec(QStringLiteral("h264"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(start->property("enabled").toBool());
}
