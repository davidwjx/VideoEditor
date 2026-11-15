#include "OnnxExecutor.h"

#include <map>
#include <stdexcept>
#include <string>

#include <QFileInfo>

#include <onnxruntime_cxx_api.h>

#ifdef _WIN32
#include <dml_provider_factory.h>
#endif

namespace VideoEditor {

namespace {

QString ortError(const Ort::Exception &exception)
{
    return QString::fromUtf8(exception.what());
}

} // namespace

struct OnnxExecutor::Impl {
    struct SessionEntry {
        std::unique_ptr<Ort::Session> session;
        std::vector<std::string> inputNames;
        std::vector<std::string> outputNames;
        QString provider = QStringLiteral("cpu");
        bool gpu = false;
    };

    std::unique_ptr<Ort::Env> env;
    std::map<QString, SessionEntry> sessions;
    QString envError;
    bool cpuFallback = false;
    QString cpuFallbackReason;
    bool hasSessions = false;
    bool allSessionsDml = true;

    QString provider() const
    {
        return hasSessions && allSessionsDml ? QStringLiteral("dml")
                                             : QStringLiteral("cpu");
    }

    Ort::SessionOptions makeOptions(const EpConfig &config, bool enableDml,
                                    QString *dmlError)
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        // Keep memory usage predictable on the offline CPU path.
        options.DisableMemPattern();
        options.DisableCpuMemArena();
#ifdef _WIN32
        if (enableDml) {
            try {
                // ORT 1.20 has no session provider query. Callers that need
                // proof of pure DML disable CPU EP fallback; models with a few
                // CPU-only nodes (MI-GAN) leave it enabled so the DML session
                // can still be created.
                if (config.requirePureDml) {
                    options.AddConfigEntry("session.disable_cpu_ep_fallback",
                                           "1");
                }
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(
                    options, config.deviceIndex));
            } catch (const std::exception &exception) {
                if (dmlError)
                    *dmlError = QString::fromUtf8(exception.what());
            }
        }
#else
        Q_UNUSED(config)
        Q_UNUSED(enableDml)
        if (dmlError)
            *dmlError = QStringLiteral("当前平台不支持 DirectML。");
#endif
        return options;
    }

    std::unique_ptr<Ort::Session> create(const QString &path,
                                         const EpConfig &config, bool enableDml,
                                         QString *usedProvider,
                                         QString *error)
    {
        if (!env) {
            if (error) {
                *error = envError.isEmpty()
                    ? QStringLiteral("ONNX Runtime 环境未初始化。")
                    : envError;
            }
            return nullptr;
        }
        QString dmlError;
        try {
            Ort::SessionOptions options =
                makeOptions(config, enableDml, &dmlError);
            if (enableDml && !dmlError.isEmpty()) {
                if (error)
                    *error = dmlError;
                return nullptr;
            }
#ifdef _WIN32
            const std::wstring widePath = path.toStdWString();
            const ORTCHAR_T *modelPath = widePath.c_str();
#else
            const QByteArray utf8 = path.toUtf8();
            const char *modelPath = utf8.constData();
#endif
            auto session =
                std::make_unique<Ort::Session>(*env, modelPath, options);
            if (usedProvider)
                *usedProvider = enableDml ? QStringLiteral("dml")
                                          : QStringLiteral("cpu");
            return session;
        } catch (const Ort::Exception &exception) {
            if (error)
                *error = ortError(exception);
            return nullptr;
        } catch (const std::exception &exception) {
            if (error)
                *error = QString::fromUtf8(exception.what());
            return nullptr;
        }
    }
};

OnnxExecutor::OnnxExecutor()
    : m_impl(std::make_unique<Impl>())
{
    try {
        m_impl->env = std::make_unique<Ort::Env>(
            ORT_LOGGING_LEVEL_WARNING, "VideoEditorM4");
    } catch (const std::exception &exception) {
        m_impl->envError = QStringLiteral("无法初始化 ONNX Runtime：%1")
                               .arg(QString::fromUtf8(exception.what()));
    }
}

OnnxExecutor::~OnnxExecutor() = default;

bool OnnxExecutor::initialize(const QStringList &partNames,
                              const QMap<QString, QString> &paths,
                              const EpConfig &config, QString *error)
{
    for (const QString &partName : partNames) {
        const QString path = paths.value(partName);
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            if (error) {
                *error = QStringLiteral("跟踪模型缺少组件：%1。").arg(partName);
            }
            return false;
        }
        if (!createSession(partName, path, config, error))
            return false;
    }
    if (error)
        error->clear();
    return true;
}

bool OnnxExecutor::createSession(const QString &partName, const QString &path,
                                 const EpConfig &config, QString *error)
{
    if (partName.isEmpty() || path.isEmpty()) {
        if (error)
            *error = QStringLiteral("跟踪模型组件名或路径为空。");
        return false;
    }

    if (!config.allowDml && !config.allowCpu) {
        if (error) {
            *error = QStringLiteral("跟踪模型未启用任何执行提供程序。");
        }
        return false;
    }

    bool useDml = config.allowDml;
    QString provider;
    QString sessionError;
    std::unique_ptr<Ort::Session> session =
        m_impl->create(path, config, useDml, &provider, &sessionError);
    if (!session && useDml) {
        if (!config.allowCpu) {
            if (error) {
                *error = QStringLiteral("DirectML 会话创建失败且未允许 CPU "
                                        "回退（%1）：%2")
                             .arg(partName, sessionError);
            }
            return false;
        }
        // Retry the same fp32 graph on CPU; DML failures are recoverable.
        m_impl->cpuFallback = true;
        m_impl->cpuFallbackReason =
            QStringLiteral("当前显卡无法加速该模型，已改用 CPU，预计耗时增加。");
        useDml = false;
        provider.clear();
        sessionError.clear();
        session = m_impl->create(path, config, useDml, &provider,
                                 &sessionError);
    }
    if (!session) {
        if (error) {
            *error = QStringLiteral("无法创建跟踪模型会话（%1）：%2")
                         .arg(partName,
                              sessionError.isEmpty()
                                  ? QStringLiteral("未知错误")
                                  : sessionError);
        }
        return false;
    }

    Impl::SessionEntry entry;
    // `useDml` is only true when the DML session was created with CPU EP
    // fallback disabled, so it reflects the provider actually in use.
    entry.provider = useDml ? QStringLiteral("dml") : QStringLiteral("cpu");
    entry.gpu = useDml;
    entry.session = std::move(session);

    try {
        Ort::AllocatorWithDefaultOptions allocator;
        const size_t inputCount = entry.session->GetInputCount();
        entry.inputNames.reserve(inputCount);
        for (size_t i = 0; i < inputCount; ++i) {
            auto name = entry.session->GetInputNameAllocated(i, allocator);
            entry.inputNames.emplace_back(name.get());
        }
        const size_t outputCount = entry.session->GetOutputCount();
        entry.outputNames.reserve(outputCount);
        for (size_t i = 0; i < outputCount; ++i) {
            auto name = entry.session->GetOutputNameAllocated(i, allocator);
            entry.outputNames.emplace_back(name.get());
        }
    } catch (const Ort::Exception &exception) {
        if (error) {
            *error = QStringLiteral("无法读取跟踪模型输入输出（%1）：%2")
                         .arg(partName, ortError(exception));
        }
        return false;
    } catch (const std::exception &exception) {
        if (error) {
            *error = QStringLiteral("无法读取跟踪模型输入输出（%1）：%2")
                         .arg(partName, QString::fromUtf8(exception.what()));
        }
        return false;
    }

    if (!m_impl->hasSessions) {
        m_impl->hasSessions = true;
        m_impl->allSessionsDml = entry.gpu;
    } else {
        m_impl->allSessionsDml = m_impl->allSessionsDml && entry.gpu;
    }
    m_impl->sessions[partName] = std::move(entry);

    if (error)
        error->clear();
    return true;
}

bool OnnxExecutor::hasSession(const QString &partName) const
{
    return m_impl->sessions.find(partName) != m_impl->sessions.end();
}

QString OnnxExecutor::provider() const { return m_impl->provider(); }

bool OnnxExecutor::usingCpuFallback() const { return m_impl->cpuFallback; }

QString OnnxExecutor::cpuFallbackReason() const
{
    return m_impl->cpuFallbackReason;
}

int OnnxExecutor::inputCount(const QString &partName) const
{
    const auto it = m_impl->sessions.find(partName);
    return it == m_impl->sessions.end() ? 0 : int(it->second.inputNames.size());
}

int OnnxExecutor::outputCount(const QString &partName) const
{
    const auto it = m_impl->sessions.find(partName);
    return it == m_impl->sessions.end() ? 0 : int(it->second.outputNames.size());
}

QString OnnxExecutor::inputName(const QString &partName, int index) const
{
    const auto it = m_impl->sessions.find(partName);
    if (it == m_impl->sessions.end() || index < 0
        || index >= int(it->second.inputNames.size())) {
        return {};
    }
    return QString::fromStdString(it->second.inputNames[size_t(index)]);
}

QString OnnxExecutor::outputName(const QString &partName, int index) const
{
    const auto it = m_impl->sessions.find(partName);
    if (it == m_impl->sessions.end() || index < 0
        || index >= int(it->second.outputNames.size())) {
        return {};
    }
    return QString::fromStdString(it->second.outputNames[size_t(index)]);
}

bool OnnxExecutor::inputShape(const QString &partName, int index,
                              std::vector<int64_t> *shape) const
{
    const auto it = m_impl->sessions.find(partName);
    if (it == m_impl->sessions.end() || index < 0
        || index >= int(it->second.inputNames.size())) {
        return false;
    }
    try {
        const auto info = it->second.session->GetInputTypeInfo(size_t(index))
                              .GetTensorTypeAndShapeInfo()
                              .GetShape();
        if (shape)
            *shape = info;
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

bool OnnxExecutor::outputShape(const QString &partName, int index,
                               std::vector<int64_t> *shape) const
{
    const auto it = m_impl->sessions.find(partName);
    if (it == m_impl->sessions.end() || index < 0
        || index >= int(it->second.outputNames.size())) {
        return false;
    }
    try {
        const auto info = it->second.session->GetOutputTypeInfo(size_t(index))
                              .GetTensorTypeAndShapeInfo()
                              .GetShape();
        if (shape)
            *shape = info;
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

bool OnnxExecutor::run(const QString &partName,
                       const QMap<QString, OnnxTensor> &inputs,
                       const QStringList &outputNames,
                       QMap<QString, OnnxOutput> *outputs, QString *error)
{
    const auto it = m_impl->sessions.find(partName);
    if (it == m_impl->sessions.end()) {
        if (error)
            *error = QStringLiteral("跟踪模型会话不存在：%1。").arg(partName);
        return false;
    }
    Impl::SessionEntry &entry = it->second;

    std::vector<std::string> inputNameStorage;
    std::vector<const char *> inputNamePointers;
    inputNameStorage.reserve(size_t(inputs.size()));
    inputNamePointers.reserve(size_t(inputs.size()));
    std::vector<std::vector<int64_t>> shapeStorage;
    shapeStorage.reserve(size_t(inputs.size()));
    std::vector<Ort::Value> inputValues;
    inputValues.reserve(size_t(inputs.size()));
    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    for (auto inputIt = inputs.constBegin(); inputIt != inputs.constEnd();
         ++inputIt) {
        const QString &name = inputIt.key();
        const OnnxTensor &tensor = inputIt.value();
        inputNameStorage.push_back(name.toStdString());
        inputNamePointers.push_back(inputNameStorage.back().c_str());
        shapeStorage.push_back(tensor.shape);
        const std::vector<int64_t> &shape = shapeStorage.back();
        try {
            if (tensor.type == OnnxTensor::DataType::Int32) {
                inputValues.push_back(Ort::Value::CreateTensor<int32_t>(
                    memoryInfo,
                    const_cast<int32_t *>(tensor.int32Data.data()),
                    tensor.int32Data.size(), shape.data(), shape.size()));
            } else if (tensor.type == OnnxTensor::DataType::UInt8) {
                inputValues.push_back(Ort::Value::CreateTensor<uint8_t>(
                    memoryInfo, const_cast<uint8_t *>(tensor.uint8Data.data()),
                    tensor.uint8Data.size(), shape.data(), shape.size()));
            } else {
                inputValues.push_back(Ort::Value::CreateTensor<float>(
                    memoryInfo, const_cast<float *>(tensor.floatData.data()),
                    tensor.floatData.size(), shape.data(), shape.size()));
            }
        } catch (const Ort::Exception &exception) {
            if (error)
                *error = ortError(exception);
            return false;
        }
    }

    std::vector<std::string> outputNameStorage;
    std::vector<const char *> outputNamePointers;
    outputNameStorage.reserve(size_t(outputNames.size()));
    outputNamePointers.reserve(size_t(outputNames.size()));
    for (const QString &name : outputNames) {
        outputNameStorage.push_back(name.toStdString());
        outputNamePointers.push_back(outputNameStorage.back().c_str());
    }

    std::vector<Ort::Value> runOutputs;
    try {
        runOutputs = entry.session->Run(
            Ort::RunOptions{nullptr}, inputNamePointers.data(),
            inputValues.data(), inputValues.size(), outputNamePointers.data(),
            outputNamePointers.size());
    } catch (const Ort::Exception &exception) {
        if (error)
            *error = QStringLiteral("跟踪模型推理失败：%1")
                         .arg(ortError(exception));
        return false;
    }

    if (runOutputs.size() != size_t(outputNames.size())) {
        if (error)
            *error = QStringLiteral("跟踪模型输出数量不符合预期。");
        return false;
    }

    for (size_t i = 0; i < runOutputs.size(); ++i) {
        OnnxOutput output;
        try {
            const auto info = runOutputs[i].GetTensorTypeAndShapeInfo();
            const ONNXTensorElementDataType elementType =
                info.GetElementType();
            output.shape = info.GetShape();
            if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
                output.type = OnnxOutput::DataType::Int32;
                const int32_t *data = runOutputs[i].GetTensorData<int32_t>();
                const size_t count = info.GetElementCount();
                output.int32Data.assign(data, data + count);
            } else if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                output.type = OnnxOutput::DataType::Float32;
                const float *data = runOutputs[i].GetTensorData<float>();
                const size_t count = info.GetElementCount();
                output.floatData.assign(data, data + count);
            } else if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {
                output.type = OnnxOutput::DataType::UInt8;
                const uint8_t *data = runOutputs[i].GetTensorData<uint8_t>();
                const size_t count = info.GetElementCount();
                output.uint8Data.assign(data, data + count);
            } else {
                if (error) {
                    *error = QStringLiteral("跟踪模型输出类型不符合预期。");
                }
                return false;
            }
        } catch (const Ort::Exception &exception) {
            if (error)
                *error = ortError(exception);
            return false;
        }
        if (outputs)
            outputs->insert(outputNames.at(int(i)), output);
    }

    if (error)
        error->clear();
    return true;
}

} // namespace VideoEditor
