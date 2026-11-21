#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QTest>
#include <QUrl>

#include "AppController.h"
#include "AudioSink.h"
#include "ExportController.h"
#include "ExportEncoderCatalog.h"
#include "PreviewController.h"
#include "TimelineController.h"
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

namespace {

int clipCount(const Project &project)
{
    int count = 0;
    for (const Track &track : project.tracks)
        count += int(track.clips.size());
    return count;
}

QQuickItem *findItem(QQuickWindow *window, const QString &name)
{
    if (auto *item = window->findChild<QQuickItem *>(name))
        return item;
    return nullptr;
}

QObject *findShortcut(QQuickWindow *window, const QString &sequence)
{
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->inherits("QQuickShortcut")
            && object->property("sequence").toString() == sequence) {
            return object;
        }
    }
    return nullptr;
}

} // namespace

// Window-scoped Shortcuts stay armed while a modal/popup owns the keyboard, so
// Delete / Ctrl+B / Space used to edit the timeline behind the export dialog
// and the play shortcut stole Space from dialog buttons. The modal gate must
// block editing while leaving the focused button able to activate itself.
TEST(ExportQml, ModalDialogGatesEditingShortcuts)
{
    const QString path =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    if (QGuiApplication::platformName().contains(QStringLiteral("offscreen")))
        GTEST_SKIP() << "needs a desktop window for key delivery";

    registerPreviewType();

    AppController controller(nullptr, QString(), nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;
    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());
    controller.refreshTimelinePreview();
    controller.timeline()->seekUs(1'000'000);
    controller.timeline()->selectClip(clipId);

    ExportController *exportController = controller.exportController();
    ASSERT_NE(exportController, nullptr);
    exportController->setCatalogForTest(EncoderCatalog::withoutHardwareForTest());
    exportController->refreshCapabilities();
    exportController->applyPreset(QStringLiteral("h264-720p30"));

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

    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(QStringLiteral(VIDEOEDITOR_SOURCE_DIR
                                           "/src/app/Main.qml")));
    QScopedPointer<QObject> root(component.create());
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    ASSERT_NE(root.data(), nullptr);
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    ASSERT_NE(window, nullptr);
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000))
        GTEST_SKIP() << "window was not exposed (no desktop/D3D session)";
    (void)window->requestActivate();
    (void)QTest::qWaitForWindowActive(window, 2000);
    QTest::qWait(200);

    QObject *dialog = window->findChild<QObject *>(
        QStringLiteral("exportDialog"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openDialog"));
    QTest::qWait(150);
    ASSERT_TRUE(dialog->property("opened").toBool());

    // Focus a button (not a text editor) inside the modal dialog.
    QQuickItem *advancedToggle =
        findItem(window, QStringLiteral("advancedToggle"));
    ASSERT_NE(advancedToggle, nullptr);
    advancedToggle->forceActiveFocus();
    QTest::qWait(50);
    ASSERT_TRUE(advancedToggle->hasActiveFocus());

    // The gate itself must disable every editing/transport shortcut. Assert
    // the Shortcut objects directly as well as the key delivery below: the
    // test harness's modal focus handling must not be what hides the bug.
    const auto expectBlocked = [&](bool blocked) {
        EXPECT_EQ(window->property("shortcutsBlocked").toBool(), blocked);
        for (const QString &sequence :
             {QStringLiteral("Delete"), QStringLiteral("Ctrl+B"),
              QStringLiteral("Space"), QStringLiteral("Right")}) {
            QObject *shortcut = findShortcut(window, sequence);
            ASSERT_NE(shortcut, nullptr) << sequence.toStdString();
            EXPECT_EQ(shortcut->property("enabled").toBool(), !blocked)
                << sequence.toStdString();
        }
    };
    expectBlocked(true);

    const int clipsBefore = clipCount(controller.document()->project());
    const int undoBefore = controller.document()->undoStack()->index();
    const qint64 positionBefore = controller.timeline()->positionUs();
    const double zoomBefore = controller.timeline()->pixelsPerSecond();
    const bool playingBefore = controller.preview()->playing();

    QTest::keyClick(window, Qt::Key_Delete);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Plus);
    QTest::qWait(100);

    EXPECT_EQ(clipCount(controller.document()->project()), clipsBefore)
        << "Delete / Ctrl+B must not edit the timeline behind a modal dialog";
    EXPECT_EQ(controller.document()->undoStack()->index(), undoBefore);
    EXPECT_EQ(controller.timeline()->positionUs(), positionBefore);
    EXPECT_DOUBLE_EQ(controller.timeline()->pixelsPerSecond(), zoomBefore);
    EXPECT_EQ(controller.preview()->playing(), playingBefore);

    // Space belongs to the focused dialog button, not the global play toggle.
    const bool advancedBefore =
        dialog->property("advancedOpen").toBool();
    QTest::keyClick(window, Qt::Key_Space);
    QTest::qWait(100);
    EXPECT_NE(dialog->property("advancedOpen").toBool(), advancedBefore)
        << "Space must activate the focused dialog button";
    EXPECT_FALSE(controller.preview()->playing())
        << "Space must not toggle timeline playback while the dialog is open";

    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
    QTest::qWait(120);
    ASSERT_FALSE(dialog->property("opened").toBool());
    expectBlocked(false);

    // Sanity check that the same key would edit the timeline when no modal is
    // open, so the assertions above are not vacuous.
    if (QQuickItem *focusSink =
            findItem(window, QStringLiteral("import-button"))) {
        focusSink->forceActiveFocus();
        QTest::qWait(50);
    }
    controller.timeline()->selectClip(clipId);
    controller.timeline()->seekUs(1'000'000);
    const int videoClipsBefore = int(
        controller.document()->trackById(QStringLiteral("track-v1"))
            ->clips.size());
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(100);
    EXPECT_EQ(int(controller.document()->trackById(QStringLiteral("track-v1"))
                      ->clips.size()),
              videoClipsBefore + 1)
        << "Ctrl+B must split once no modal is open";

    // A context menu is a popup too: its open state must gate the shortcuts.
    const int clipsWithMenu = clipCount(controller.document()->project());
    QObject *projectButton = window->findChild<QObject *>(
        QStringLiteral("project-menu-button"));
    ASSERT_NE(projectButton, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(projectButton, "clicked"));
    QTest::qWait(120);
    ASSERT_EQ(controller.openPopupCount(), 1)
        << "the project menu must register itself with the modal gate";
    expectBlocked(true);
    QTest::keyClick(window, Qt::Key_Delete);
    QTest::qWait(100);
    EXPECT_EQ(clipCount(controller.document()->project()), clipsWithMenu)
        << "Delete must not edit the timeline while a menu is open";
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(120);
    EXPECT_EQ(controller.openPopupCount(), 0);
    expectBlocked(false);
}

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
    // Encoding details live in 高级 now; the first screen explains the
    // missing H.265 hardware next to the preset instead.
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
    EXPECT_GT(dialog->property("height").toDouble(), 100.0);
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
