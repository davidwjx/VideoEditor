#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>

#include "Project.h"
#include "TimelineEvaluator.h"

namespace VideoEditor {

class D3D11Context;
class SharedTextureRing;
class LayerCompositor;
class ClipVideoSource;
struct VideoLayer;

// Runs on its own thread with its own D3D11 device: evaluates the timeline at
// a requested instant, composes every video layer plus the rasterized subtitle
// image into one shared-texture ring slot, and hands that slot to the scene
// graph bridge. The same LayerCompositor/ClipVideoSource pair is reusable by
// the M3 offline export, so nothing here is preview-only.
class TimelineRenderWorker : public QObject {
    Q_OBJECT

public:
    explicit TimelineRenderWorker(QObject *parent = nullptr);
    ~TimelineRenderWorker() override;

public slots:
    void setProject(VideoEditor::Project project, QString projectPath,
                    quint64 generation);
    void renderFrame(qint64 timelineUs, quint64 generation);
    void close();

signals:
    void frameReady(int slot, QSize size, qint64 timelineUs, quintptr handle0,
                    quintptr handle1, quintptr handle2, quint64 generation);
    void error(QString message);

private:
    ClipVideoSource *sourceFor(const VideoLayer &layer);
    QImage subtitleImageFor(const QList<SubtitleLayer> &layers);
    void reset();

    Project m_project;
    QString m_projectPath;
    quint64 m_generation = 0;
    std::unique_ptr<D3D11Context> m_context;
    std::unique_ptr<SharedTextureRing> m_ring;
    std::unique_ptr<LayerCompositor> m_compositor;
    std::map<QString, std::unique_ptr<ClipVideoSource>> m_sources;
    QSet<QString> m_failedClips;
    QSet<QString> m_reportedClips;
    QString m_subtitleKey;
    QImage m_subtitleImage;
    int m_width = 1920;
    int m_height = 1080;
    bool m_valid = false;
};

} // namespace VideoEditor
