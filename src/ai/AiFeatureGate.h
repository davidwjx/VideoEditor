// src/ai/AiFeatureGate.h
#pragma once

#include <optional>

#include <QString>

#include "ModelRegistry.h"

namespace VideoEditor {

// Product-wide switch for the AI removal feature. It is fail-closed: only a
// manifest with commercial approval, legal sign-off, at least one approved
// tracking model and one approved inpaint model whose files all hash correctly
// turns it on.
class AiFeatureGate {
public:
    bool enabled() const { return m_enabled; }
    QString disabledReason() const { return m_reason; }
    bool reload(const QString &modelsDir, QString *error = nullptr);

    // Internal technical validation only. Requires VIDEOEDITOR_M4_TECHNICAL=1,
    // loads/validates the manifest and hashes the tracking and inpaint records
    // that the engine will actually load. It never changes the manifest or sets
    // commercial approval; technicalPreview() lets QML show the disclaimer.
    bool enableTechnicalPreview(QString *error = nullptr);
    bool technicalPreview() const { return m_technicalPreview; }
    static QString technicalPreviewDisclaimer();

    const ModelRegistry *registry() const
    {
        return m_registry ? &*m_registry : nullptr;
    }
    // Test-only override; production paths can only open through reload().
    void setEnabledForTest(bool enabled);
    // Test-only registry injection so controllers can build cache keys without
    // touching the filesystem. It does not enable the gate.
    void setRegistryForTest(ModelRegistry registry);

private:
    bool m_enabled = false;
    bool m_technicalPreview = false;
    QString m_reason = QStringLiteral("AI 消除功能未启用：未加载模型清单。");
    QString m_modelsDir;
    std::optional<ModelRegistry> m_registry;
};

} // namespace VideoEditor
