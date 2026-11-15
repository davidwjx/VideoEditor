#include <gtest/gtest.h>

#include <QPointF>
#include <QTemporaryDir>

#include "MaskSidecarStore.h"
#include "ProjectDocument.h"
#include "RemovalController.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalController, StrokeCreatesOneUndoCommandAndUndoRestoresMask)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(controller.maskMode());
    const QString before = controller.currentMaskHash();

    controller.beginStroke(20, false);
    controller.continueStroke(QPointF(0.4, 0.4), QPointF(0.6, 0.6));
    controller.endStroke();

    EXPECT_NE(controller.currentMaskHash(), before);
    EXPECT_TRUE(doc.canUndo());
    EXPECT_EQ(doc.project().removalLayers.size(), 1);

    doc.undo();
    EXPECT_EQ(controller.currentMaskHash(), before);
    doc.redo();
    EXPECT_NE(controller.currentMaskHash(), before);
}

TEST(RemovalController, FeatherChangeDoesNotChangeMaskHash)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    const QString maskHash = controller.currentMaskHash();

    controller.setFeatherPx(24);
    EXPECT_EQ(controller.currentMaskHash(), maskHash);
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().removalLayers[0].maskKeyframes[0].featherPx, 24);
}

TEST(RemovalController, AddAndDeleteKeyframesStaySorted)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();

    // 测试直接调用控制器的时间点版本，避免依赖 QML 播放头。
    controller.addMaskKeyframeAtSourceTime(0, QStringLiteral("mk-a"));
    controller.addMaskKeyframeAtSourceTime(2'000'000, QStringLiteral("mk-b"));
    controller.addMaskKeyframeAtSourceTime(1'000'000, QStringLiteral("mk-c"));
    const auto layer = doc.project().removalLayers.first();
    EXPECT_EQ(layer.maskKeyframes.size(), 3);
    EXPECT_EQ(layer.maskKeyframes[1].sourceTimeUs, 1'000'000);

    controller.removeMaskKeyframe(1);
    EXPECT_EQ(doc.project().removalLayers.first().maskKeyframes.size(), 2);
}
