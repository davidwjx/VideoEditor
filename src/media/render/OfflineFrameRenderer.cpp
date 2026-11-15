#include "OfflineFrameRenderer.h"

#include <algorithm>
#include <cstring>

#include "D3D11Context.h"
#include "TimelineCompositor.h"

namespace VideoEditor {

OfflineFrameRenderer::OfflineFrameRenderer() = default;

OfflineFrameRenderer::~OfflineFrameRenderer()
{
    close();
}

void OfflineFrameRenderer::close()
{
    // Render targets must die before the device that created them.
    m_rtv.Reset();
    m_staging.Reset();
    m_target.Reset();
    m_compositor.reset();
    m_width = 0;
    m_height = 0;
}

bool OfflineFrameRenderer::open(const Project &project, int width, int height,
                                QString *error)
{
    close();
    if (width <= 0 || height <= 0) {
        if (error)
            *error = QStringLiteral("导出分辨率无效。");
        return false;
    }
    m_width = width;
    m_height = height;
    m_compositor = std::make_unique<TimelineCompositor>();
    m_compositor->setWarningCallback([this](const QString &message) {
        if (m_warning)
            m_warning(message);
    });
    if (!m_compositor->open(project, width, height, false, error))
        return false;
    if (!ensureTarget(error)) {
        close();
        return false;
    }
    return true;
}

bool OfflineFrameRenderer::ensureTarget(QString *error)
{
    D3D11Context *context = m_compositor->context();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(m_width);
    desc.Height = UINT(m_height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(context->device()->CreateTexture2D(&desc, nullptr, &m_target))) {
        if (error)
            *error = QStringLiteral("无法创建导出合成纹理。");
        return false;
    }
    if (FAILED(context->device()->CreateRenderTargetView(
            m_target.Get(), nullptr, &m_rtv))) {
        if (error)
            *error = QStringLiteral("无法创建导出渲染目标。");
        return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(
            context->device()->CreateTexture2D(&desc, nullptr, &m_staging))) {
        if (error)
            *error = QStringLiteral("无法创建导出回读纹理。");
        return false;
    }
    return true;
}

bool OfflineFrameRenderer::isValid() const
{
    return m_compositor && m_compositor->isValid() && m_target && m_rtv
           && m_staging;
}

bool OfflineFrameRenderer::renderFrame(int64_t timelineUs,
                                       std::vector<uint8_t> *rgba,
                                       QString *error)
{
    if (!isValid() || !rgba) {
        if (error)
            *error = QStringLiteral("导出渲染器未初始化。");
        return false;
    }

    QString drawError;
    m_compositor->drawFrame(std::max<int64_t>(0, timelineUs), m_rtv.Get(),
                            true, &drawError);
    if (!drawError.isEmpty()) {
        if (error)
            *error = drawError;
        return false;
    }

    D3D11Context *context = m_compositor->context();
    context->context()->CopyResource(m_staging.Get(), m_target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->context()->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0,
                                       &mapped))) {
        if (error)
            *error = QStringLiteral("无法回读导出帧。");
        return false;
    }

    rgba->resize(size_t(m_width) * size_t(m_height) * 4);
    for (int y = 0; y < m_height; ++y) {
        std::memcpy(rgba->data() + size_t(y) * size_t(m_width) * 4,
                    static_cast<const uint8_t *>(mapped.pData)
                        + size_t(y) * mapped.RowPitch,
                    size_t(m_width) * 4);
    }
    context->context()->Unmap(m_staging.Get(), 0);
    return true;
}

} // namespace VideoEditor
