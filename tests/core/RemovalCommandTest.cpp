#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "ProjectDocument.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalCommand, UndoRedoRestoresLayerAndClipLink)
{
    Project p = projectWithClip();
    ProjectDocument doc;
    doc.setProject(p);

    RemovalLayer layer = sampleLayer();
    RemovalLayerEdit edit;
    edit.clipId = QStringLiteral("clip-1");
    edit.before = std::nullopt;
    edit.after = layer;
    doc.applyRemovalLayerEdit(edit, QStringLiteral("添加 AI 消除层"));

    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().tracks[0].clips[0].removalLayerId, layer.id);

    doc.undo();
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());
    EXPECT_TRUE(doc.project().tracks[0].clips[0].removalLayerId.isEmpty());

    doc.redo();
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().tracks[0].clips[0].removalLayerId, layer.id);
}

TEST(RemovalCommand, DeletingLayerRestoresOriginalAndUndoRestoresCacheKey)
{
    Project p = projectWithClip();
    RemovalLayer layer = sampleLayer();
    p.removalLayers.append(layer);
    p.tracks[0].clips[0].removalLayerId = layer.id;
    ProjectDocument doc;
    doc.setProject(p);

    RemovalLayerEdit remove;
    remove.clipId = layer.clipId;
    remove.before = layer;
    remove.after = std::nullopt;
    doc.applyRemovalLayerEdit(remove, QStringLiteral("删除 AI 消除层"));
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());

    doc.undo();
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().removalLayers[0].cacheKey, layer.cacheKey);
}
