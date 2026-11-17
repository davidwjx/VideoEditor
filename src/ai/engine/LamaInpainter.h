// src/ai/engine/LamaInpainter.h
#pragma once

#include <memory>

#include "InpaintPipeline.h"

namespace VideoEditor {

// LaMa 512 ONNX graph with the allowzero=0 patch. Input is float32
// [1,4,512,512]: RGB in [0,1] with the hole zeroed plus a hole mask (1 = hole).
// Output is float32 [1,3,512,512] in [0,1]. DirectML is slower than CPU for
// this graph and is therefore never selected. Crops larger than 512 are
// repaired as 512 tiles with a 64 px overlap and feather blending.
class LamaInpainter : public Inpainter {
public:
    explicit LamaInpainter(const QString &modelPath, int deviceIndex = 0,
                           bool allowDml = false);
    ~LamaInpainter() override;
    LamaInpainter(const LamaInpainter &) = delete;
    LamaInpainter &operator=(const LamaInpainter &) = delete;

    std::optional<PatchResult> repair(const QImage &crop,
                                      const BinaryMask &maskCrop,
                                      QString *error) override;

    bool isReady() const override;
    QString lastError() const override;
    QString provider() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
