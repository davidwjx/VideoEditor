#pragma once

#include <cstdint>
#include <memory>

#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>

#include "Project.h"

namespace VideoEditor {

class SharedTextureRing;
class TimelineCompositor;

// Runs on its own thread with its own D3D11 device: evaluates the timeline at
// a requested instant, composes every video layer plus the rasterized subtitle
// image into one shared-texture ring slot, and hands that slot to the scene
// graph bridge. Composition is delegated to TimelineCompositor, the same class
// M3 export drives, so nothing here is preview-only.
class TimelineRenderWorker : public QObject {
    Q_OBJECT

public:
    explicit TimelineRenderWorker(QObject *parent = nullptr);
    ~TimelineRenderWorker() override;

public slots:
    // useProxy selects the asset's proxy file when one exists; false forces the
    // original source even if a proxy is cached.
    void setProject(VideoEditor::Project project, QString projectPath,
                    quint64 generation, bool useProxy);
    void renderFrame(qint64 timelineUs, quint64 generation);
    // int instead of the C++ enum so QML can forward the mode across a queued
    // connection without registering a new metatype.
    void setRemovalCompare(int mode, int splitPermille);
    void setRemovalCacheRoot(QString root);
    void close();

signals:
    // layerCount is the number of video layers actually decoded and drawn into
    // this frame; the self-tests use it to catch a black composite that still
    // presents at a healthy frame rate.
    void frameReady(int slot, QSize size, qint64 timelineUs, quintptr handle0,
                    quintptr handle1, quintptr handle2, int layerCount,
                    quint64 generation);
    // The ring slot is still held by the scene graph; the request was dropped
    // so the controller can release its single-in-flight gate.
    void frameSkipped(qint64 timelineUs, quint64 generation);
    void error(QString message);

private:
    void reset();

    Project m_project;
    QString m_projectPath;
    quint64 m_generation = 0;
    std::unique_ptr<TimelineCompositor> m_compositor;
    std::unique_ptr<SharedTextureRing> m_ring;
    int m_width = 1920;
    int m_height = 1080;
    bool m_useProxy = true;
    bool m_valid = false;
    QString m_removalCacheRoot;
    // 0=Repair, 1=Original, 2=SplitWipe. Persisted so a compositor rebuilt
    // after close() starts in the user's current comparison mode.
    int m_removalCompare = 0;
    int m_splitPermille = 500;
};

} // namespace VideoEditor
