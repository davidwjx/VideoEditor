#include "AiFeatureGate.h"

#include <utility>

#include <QDir>
#include <QSet>
#include <QStringList>

namespace VideoEditor {

QString AiFeatureGate::technicalPreviewDisclaimer()
{
    return QStringLiteral(
        "内部技术预览：模型未取得商业许可，仅用于内部技术验证，禁止分发。");
}

bool AiFeatureGate::reload(const QString &modelsDir, QString *error)
{
    m_modelsDir = modelsDir;
    m_technicalPreview = false;
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

bool AiFeatureGate::enableTechnicalPreview(QString *error)
{
    m_technicalPreview = false;
    const auto fail = [this, error](const QString &message) {
        m_enabled = false;
        m_reason = message;
        if (error)
            *error = message;
        return false;
    };

    if (qEnvironmentVariable("VIDEOEDITOR_M4_TECHNICAL") != QLatin1String("1")) {
        return fail(QStringLiteral(
            "内部技术预览未启用：需要设置 VIDEOEDITOR_M4_TECHNICAL=1。"));
    }

    // AppController calls reload() first, so the registry is normally present.
    // Re-load on demand so the technical preview is still reachable when the
    // manifest parse succeeded but the normal gate refused approval.
    if (!m_registry && !m_modelsDir.isEmpty()) {
        const QString manifestPath =
            QDir(m_modelsDir).filePath(QStringLiteral("models.manifest.json"));
        QString loadError;
        auto registry = ModelRegistry::load(manifestPath, &loadError);
        if (!registry) {
            return fail(QStringLiteral("AI 消除功能未启用：未加载模型清单（%1）。")
                            .arg(loadError));
        }
        m_registry = std::move(*registry);
    }
    if (!m_registry) {
        return fail(QStringLiteral("AI 消除功能未启用：未加载模型清单。"));
    }

    const AiModelRecord *tracking = m_registry->defaultTrackingModel();
    if (!tracking) {
        return fail(QStringLiteral(
            "内部技术预览不可用：模型清单中没有可用的跟踪模型。"));
    }
    QStringList verifyIds{tracking->id};
    QSet<QString> seen{tracking->id};
    const AiModelRecord *fast =
        m_registry->preferredInpaintModel(RepairMode::Fast);
    const AiModelRecord *quality =
        m_registry->preferredInpaintModel(RepairMode::Quality);
    if (!fast && !quality) {
        return fail(QStringLiteral(
            "内部技术预览不可用：模型清单中没有可用的修复模型。"));
    }
    for (const AiModelRecord *record : {fast, quality}) {
        if (!record || seen.contains(record->id))
            continue;
        seen.insert(record->id);
        verifyIds.append(record->id);
    }

    QString verifyError;
    if (!m_registry->verifyFiles(verifyIds, &verifyError)) {
        return fail(QStringLiteral("AI 消除功能未启用：模型文件缺失或校验失败（%1）。")
                        .arg(verifyError));
    }

    m_enabled = true;
    m_technicalPreview = true;
    m_reason = technicalPreviewDisclaimer();
    if (error)
        error->clear();
    return true;
}

void AiFeatureGate::setEnabledForTest(bool enabled)
{
    m_enabled = enabled;
    if (!enabled) {
        m_reason = QStringLiteral("AI 消除功能未启用：未加载模型清单。");
        m_technicalPreview = false;
    } else {
        m_reason.clear();
    }
}

void AiFeatureGate::setRegistryForTest(ModelRegistry registry)
{
    m_registry = std::move(registry);
}

} // namespace VideoEditor
