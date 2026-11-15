#include <gtest/gtest.h>

#include "AiProtocol.h"

using namespace VideoEditor;

namespace {

AiJobSpec validSpec()
{
    AiJobSpec spec;
    spec.jobId = QStringLiteral("job-1");
    spec.clipId = QStringLiteral("clip-1");
    spec.assetPath = QStringLiteral("C:/media/asset.mp4");
    spec.sidecarRoot = QStringLiteral("C:/sidecars");
    spec.cacheDir = QStringLiteral("C:/cache");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    spec.modelsManifestPath = QStringLiteral("C:/models/models.manifest.json");
    spec.trackingModelId = QStringLiteral("track-1");
    spec.inpaintModelId = QStringLiteral("inpaint-1");
    spec.sourceInUs = 0;
    spec.sourceOutUs = 1'000'000;
    spec.width = 640;
    spec.height = 360;
    spec.fps = Rational{30, 1};
    spec.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('b')), 4, 1});
    return spec;
}

} // namespace

TEST(AiProtocol, TrackingOnlyDoesNotRequireInpaintModel)
{
    AiJobSpec spec = validSpec();
    spec.trackingOnly = true;
    spec.inpaintModelId.clear();

    QString error;
    EXPECT_TRUE(AiProtocol::validateJobSpec(spec, &error))
        << error.toStdString();
}

TEST(AiProtocol, RepairJobRequiresInpaintModel)
{
    AiJobSpec spec = validSpec();
    spec.inpaintModelId.clear();

    QString error;
    EXPECT_FALSE(AiProtocol::validateJobSpec(spec, &error));
    EXPECT_NE(error.indexOf(QStringLiteral("inpaintModelId")), -1)
        << error.toStdString();
}
