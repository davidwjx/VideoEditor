// src/ai/engine/MiGanInpainter.h
#pragma once

#include <memory>

#include "InpaintPipeline.h"

namespace VideoEditor {

// MI-GAN 512 ONNX pipeline. The graph takes a uint8 RGB image [1,3,H,W] and a
// uint8 mask [1,1,H,W] where 255 = known and 0 = hole (the inverse of our
// BinaryMask) and returns the full input image with the hole filled. H/W must
// be multiples of 8; other sizes are reflect-padded and cropped back.
class MiGanInpainter : public Inpainter {
public:
    MiGanInpainter(const QString &modelPath, int deviceIndex = 0,
                   bool allowDml = true, bool allowCpu = true);
    ~MiGanInpainter() override;
    MiGanInpainter(const MiGanInpainter &) = delete;
    MiGanInpainter &operator=(const MiGanInpainter &) = delete;

    std::optional<PatchResult> repair(const QImage &crop,
                                      const BinaryMask &maskCrop,
                                      QString *error) override;

    bool isReady() const override;
    QString lastError() const override;
    QString provider() const override;
    bool usingCpuFallback() const;
    QString cpuFallbackReason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
