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
    const ModelRegistry *registry() const
    {
        return m_registry ? &*m_registry : nullptr;
    }
    // Test-only override; production paths can only open through reload().
    void setEnabledForTest(bool enabled);

private:
    bool m_enabled = false;
    QString m_reason = QStringLiteral("AI 消除功能未启用：未加载模型清单。");
    std::optional<ModelRegistry> m_registry;
};

} // namespace VideoEditor
