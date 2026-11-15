#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#include <QImage>
#include <QSet>
#include <QString>

#include "Project.h"

struct ID3D11RenderTargetView;

namespace VideoEditor {

class D3D11Context;
class LayerCompositor;
class ClipVideoSource;
class VideoConverters;
struct VideoLayer;
struct SubtitleLayer;

// Owns one D3D11 device, the decoders and the layer compositor that turn a
// timeline instant into a rendered frame. TimelineRenderWorker (preview) and
// OfflineFrameRenderer (M3 export) both drive this class, so preview and export
// share exactly one composition path and no mutable global state.
//
// The compositor is not thread-safe: one instance belongs to the thread that
// calls drawFrame(). It exposes no CPU readback of its own; an offline caller
// supplies its own render target and reads that back after drawFrame().
class TimelineCompositor {
public:
    TimelineCompositor();
    ~TimelineCompositor();

    TimelineCompositor(const TimelineCompositor &) = delete;
    TimelineCompositor &operator=(const TimelineCompositor &) = delete;

    // Opens (or reuses) the device and composition pipeline for this project
    // size. Reopening with the same size keeps the device and compositor and
    // only drops clip decoders and the subtitle cache; a resolution change
    // rebuilds everything.
    bool open(const Project &project, int width, int height, bool useProxy,
              QString *error = nullptr);
    void close();
    void setUseProxy(bool useProxy) { m_useProxy = useProxy; }

    // Composes the frame covering timelineUs into target and returns the number
    // of video layers actually drawn. clearBlack clears the target first.
    int drawFrame(int64_t timelineUs, ID3D11RenderTargetView *target,
                  bool clearBlack, QString *error = nullptr);

    bool isValid() const { return m_valid; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    D3D11Context *context() const { return m_context.get(); }
    int openSourceCountForTest() const { return int(m_sources.size()); }
    void setWarningCallback(std::function<void(const QString &)> warning)
    {
        m_warning = std::move(warning);
    }

private:
    struct SourceEntry {
        std::unique_ptr<ClipVideoSource> source;
        quint64 lastUsedFrame = 0;
    };

    ClipVideoSource *sourceFor(const VideoLayer &layer);
    QImage subtitleImageFor(const QList<SubtitleLayer> &layers);
    bool ensureConverters(QString *error);
    void reportOnce(const QString &key, const QString &message);

    Project m_project;
    std::unique_ptr<D3D11Context> m_context;
    std::unique_ptr<LayerCompositor> m_compositor;
    std::unique_ptr<VideoConverters> m_converters;
    std::map<QString, SourceEntry> m_sources;
    QSet<QString> m_failedClips;
    QSet<QString> m_reportedErrors;
    QString m_subtitleKey;
    QImage m_subtitleImage;
    quint64 m_frameSerial = 0;
    int m_width = 1920;
    int m_height = 1080;
    bool m_useProxy = true;
    bool m_valid = false;
    std::function<void(const QString &)> m_warning;
};

} // namespace VideoEditor

#endif
