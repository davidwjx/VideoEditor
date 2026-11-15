#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <QString>

#include "Project.h"

namespace VideoEditor {

class TimelineCompositor;

// Renders timeline instants into an offscreen D3D11 render target and reads the
// result back as tightly packed RGBA8 (top-down, width*height*4). M3 export
// uses this instead of the preview's shared-texture ring, but the actual
// composition runs through the same TimelineCompositor the preview uses.
class OfflineFrameRenderer {
public:
    OfflineFrameRenderer();
    ~OfflineFrameRenderer();

    OfflineFrameRenderer(const OfflineFrameRenderer &) = delete;
    OfflineFrameRenderer &operator=(const OfflineFrameRenderer &) = delete;

    bool open(const Project &project, int width, int height,
              QString *error = nullptr);
    void close();

    // timelineUs is clamped to >= 0. rgba may be non-null only after open().
    bool renderFrame(int64_t timelineUs, std::vector<uint8_t> *rgba,
                     QString *error = nullptr);

    bool isValid() const;
    int width() const { return m_width; }
    int height() const { return m_height; }
    void setWarningCallback(std::function<void(const QString &)> warning)
    {
        m_warning = std::move(warning);
    }

private:
    bool ensureTarget(QString *error);

    std::unique_ptr<TimelineCompositor> m_compositor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_target;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    int m_width = 0;
    int m_height = 0;
    std::function<void(const QString &)> m_warning;
};

} // namespace VideoEditor

#endif
