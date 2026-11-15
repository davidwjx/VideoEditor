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
class VideoConverters;
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
    struct SourceEntry {
        std::unique_ptr<ClipVideoSource> source;
        quint64 lastUsedFrame = 0;
    };

    ClipVideoSource *sourceFor(const VideoLayer &layer);
    QImage subtitleImageFor(const QList<SubtitleLayer> &layers);
    bool ensureConverters(QString *error);
    void reportOnce(const QString &key, const QString &message);
    void reset();

    Project m_project;
    QString m_projectPath;
    quint64 m_generation = 0;
    quint64 m_frameSerial = 0;
    std::unique_ptr<D3D11Context> m_context;
    std::unique_ptr<SharedTextureRing> m_ring;
    std::unique_ptr<LayerCompositor> m_compositor;
    std::unique_ptr<VideoConverters> m_converters;
    std::map<QString, SourceEntry> m_sources;
    QSet<QString> m_failedClips;
    QSet<QString> m_reportedErrors;
    QString m_subtitleKey;
    QImage m_subtitleImage;
    int m_width = 1920;
    int m_height = 1080;
    bool m_valid = false;
};

} // namespace VideoEditor
