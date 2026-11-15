#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointF>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "AiJobManager.h"
#include "AiProtocol.h"
#include "AppController.h"
#include "AudioSink.h"
#include "ExportController.h"
#include "ExportEncoderCatalog.h"
#include "ProjectDocument.h"
#include "RemovalController.h"
#include "TimelineController.h"

using namespace VideoEditor;

namespace {

AudioEngine::SinkFactory nullSinkFactory()
{
    return [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
}

class TechnicalEnvGuard {
public:
    TechnicalEnvGuard()
        : m_old(qgetenv("VIDEOEDITOR_M4_TECHNICAL"))
    {
        qputenv("VIDEOEDITOR_M4_TECHNICAL", "1");
    }
    ~TechnicalEnvGuard()
    {
        if (m_old.isNull())
            qunsetenv("VIDEOEDITOR_M4_TECHNICAL");
        else
            qputenv("VIDEOEDITOR_M4_TECHNICAL", m_old);
    }

private:
    QByteArray m_old;
};

} // namespace

// Internal technical validation only: runs the real engine against a tiny
// trimmed clip. It skips unless VIDEOEDITOR_M4_MODELS points at an installed
// manifest, so the normal unit-test suite never needs model binaries.
TEST(RemovalTechnicalE2E, TracksAndGeneratesWithInstalledModels)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    if (modelsDir.isEmpty())
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS is not set";
    if (!QFileInfo::exists(
            QDir(modelsDir).filePath(QStringLiteral("models.manifest.json"))))
        GTEST_SKIP() << "VIDEOEDITOR_M4_MODELS has no manifest";
    const QString fixture =
        QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
            .filePath(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(fixture))
        GTEST_SKIP() << "missing h264.mp4";

    TechnicalEnvGuard env;
    QTemporaryDir sidecarDir;
    ASSERT_TRUE(sidecarDir.isValid());

    AppController controller(
        nullptr, sidecarDir.filePath(QStringLiteral("recovery.json")),
        nullSinkFactory(), sidecarDir.path());
    ASSERT_TRUE(controller.aiFeatureEnabled())
        << "technical preview must enable the gate when models are present";
    ASSERT_TRUE(controller.aiTechnicalPreview());

    controller.importUrls({QUrl::fromLocalFile(fixture)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;
    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());
    // Keep the run small: four frames at 30 fps.
    controller.timeline()->trimEnd(clipId, 4 * 1'000'000 / 30);
    controller.timeline()->seekUs(0);
    controller.timeline()->selectClip(clipId);

    RemovalController *removal = controller.removal();
    removal->beginMask(clipId);
    ASSERT_TRUE(removal->maskMode()) << removal->error().toStdString();
    removal->beginStroke(24, false);
    removal->continueStroke(QPointF(0.4, 0.4), QPointF(0.6, 0.6));
    removal->endStroke();
    ASSERT_FALSE(controller.document()->project().removalLayers.isEmpty())
        << removal->error().toStdString();
    ASSERT_FALSE(removal->maskKeyframes().isEmpty());

    QSignalSpy finished(controller.aiJobs(),
                        &AiJobManager::jobFinished);
    ASSERT_TRUE(removal->track()) << removal->error().toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 180000);
    const AiJobResult trackResult =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    ASSERT_TRUE(trackResult.ok) << trackResult.errorMessage.toStdString();
    ASSERT_EQ(controller.document()->project().removalLayers.size(), 1);
    const RemovalLayer &tracked =
        controller.document()->project().removalLayers.first();
    EXPECT_FALSE(tracked.trackingConfidenceHash.isEmpty());
    EXPECT_NE(tracked.trackingState, RemovalTrackingState::Failed);
    EXPECT_EQ(tracked.cacheKey.size(), 64);

    finished.clear();
    ASSERT_TRUE(removal->generate()) << removal->error().toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 180000);
    const AiJobResult generateResult =
        qvariant_cast<AiJobResult>(finished.at(0).at(0));
    ASSERT_TRUE(generateResult.ok)
        << generateResult.errorMessage.toStdString();

    const RemovalLayer &generated =
        controller.document()->project().removalLayers.first();
    EXPECT_EQ(generated.cacheKey.size(), 64);
    EXPECT_EQ(generated.cachePath,
              QStringLiteral("removal/") + generated.cacheKey);
    EXPECT_TRUE(generated.trackingState == RemovalTrackingState::Complete
                || generated.trackingState
                       == RemovalTrackingState::LowConfidence);
    EXPECT_TRUE(removal->cacheStatusText() == QStringLiteral("已生成"))
        << removal->cacheStatusText().toStdString();

    // The technical preview must not loop: export preflight still bypasses the
    // commercial license gate and now accepts the freshly generated cache.
    ExportController *exportController = controller.exportController();
    exportController->setCatalogForTest(
        EncoderCatalog::withoutHardwareForTest());
    exportController->refreshCapabilities();
    const QString validation = exportController->validate();
    EXPECT_FALSE(exportController->removalRequired())
        << exportController->removalMessage().toStdString();
    EXPECT_FALSE(exportController->removalMessage().contains(
        QStringLiteral("许可证")))
        << exportController->removalMessage().toStdString();
    EXPECT_FALSE(validation.contains(QStringLiteral("许可证")))
        << validation.toStdString();
    EXPECT_TRUE(validation.isEmpty()) << validation.toStdString();
}
