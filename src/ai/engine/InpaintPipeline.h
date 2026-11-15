// src/ai/engine/InpaintPipeline.h
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

#include "MaskCodec.h"
#include "Removal.h"

namespace VideoEditor {

// Model locations and device selection for the repair stage. MI-GAN and LaMa
// are separate ONNX graphs; only the path matching the selected mode needs to
// be set on the production path. Tile dimensions are fixed by the model
// contracts (MI-GAN's internal canvas and LaMa's 512 graph), so they are not
// configurable here; chooseTileSize() remains the tested VRAM-budget helper.
struct InpaintConfig {
    QString miganPath;
    QString lamaPath;
    int deviceIndex = 0;
    bool allowDml = true;
    bool allowCpu = true;
};

// Alpha-bearing patch above `crop` in the full display frame. `PatchResult`
// returned by an Inpainter may leave `crop` empty; InpaintPipeline fills it in.
struct PatchResult {
    QImage rgba;
    QRect crop;
};

// One model tile. Implementations receive the full crop and a mask where true
// means "remove" (hole); they return an ARGB32 image of the same crop size.
class Inpainter {
public:
    virtual ~Inpainter() = default;
    virtual std::optional<PatchResult> repair(const QImage &crop,
                                              const BinaryMask &maskCrop,
                                              QString *error) = 0;
    virtual bool isReady() const { return true; }
    virtual QString lastError() const { return QString(); }
};

// Turns per-frame masks into feathered, colour-matched repair patches. The
// production path selects MI-GAN for Fast and LaMa for Quality; tests inject a
// deterministic Inpainter through setInpainterForTest().
class InpaintPipeline {
public:
    InpaintPipeline();
    ~InpaintPipeline();
    InpaintPipeline(const InpaintPipeline &) = delete;
    InpaintPipeline &operator=(const InpaintPipeline &) = delete;

    // Test injection: never creates an ONNX session.
    void setInpainterForTest(std::unique_ptr<Inpainter> inpainter);
    // Production injection (the engine resolves the model record itself).
    void setInpainter(std::unique_ptr<Inpainter> inpainter);
    void setConfig(const InpaintConfig &config);

    // Repairs every frame whose mask is non-empty. Frames with an empty mask
    // yield a fully transparent patch. `featherPx` is constant for the chunk.
    // The progress callback receives (frameIndexInChunk, message); returning
    // false cancels the chunk (wasCancelled() becomes true).
    std::optional<QList<PatchResult>> repairChunk(
        const QList<QImage> &frames, const QList<BinaryMask> &masks,
        const RemovalParameters &params, int featherPx,
        const std::function<bool(int, const QString &)> &progress = {},
        QString *error = nullptr);

    bool wasCancelled() const { return m_cancelled; }

    // Largest patch size (512/384/256/128) whose working set fits the budget.
    static int chooseTileSize(int cropW, int cropH, qint64 vramBudgetBytes);

    // Per-channel mean matching over an 8 px ring outside the hole. Returns
    // true when the ring had at least 32 pixels and the patch was adjusted.
    static bool colorMatch(const QImage &sourceCrop, QImage *patch,
                           const BinaryMask &maskCrop);

    // Builds a feathered alpha mask (0 outside, 255 at the hole centre) from
    // `maskCrop`. Exposed for tests and the post-stabilization second pass.
    static QImage featherAlpha(const BinaryMask &maskCrop, int featherPx);

private:
    Inpainter *activeInpainter(RepairMode mode, QString *error);

    std::unique_ptr<Inpainter> m_injected;
    std::unique_ptr<Inpainter> m_owned;
    RepairMode m_ownedMode = RepairMode::Fast;
    bool m_hasOwnedMode = false;
    InpaintConfig m_config;
    bool m_cancelled = false;
};

} // namespace VideoEditor
