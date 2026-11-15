#include "AiFeatureGate.h"

#include <QDir>

namespace VideoEditor {

bool AiFeatureGate::reload(const QString &modelsDir, QString *error)
{
    const QString manifestPath =
        QDir(modelsDir).filePath(QStringLiteral("models.manifest.json"));
    QString loadError;
    auto registry = ModelRegistry::load(manifestPath, &loadError);
    if (!registry) {
        m_enabled = false;
        m_reason = QStringLiteral("AI 消除功能未启用：未加载模型清单。");
        m_registry.reset();
        if (error)
            *error = loadError;
        return false;
    }

    m_registry = std::move(*registry);
    QString reason;
    m_enabled = m_registry->featureEnabled(&reason);
    m_reason = m_enabled ? QString() : reason;
    if (error) {
        if (m_enabled)
            error->clear();
        else
            *error = m_reason;
    }
    return m_enabled;
}

void AiFeatureGate::setEnabledForTest(bool enabled)
{
    m_enabled = enabled;
    if (!enabled)
        m_reason = QStringLiteral("AI 消除功能未启用：未加载模型清单。");
    else
        m_reason.clear();
}

} // namespace VideoEditor
