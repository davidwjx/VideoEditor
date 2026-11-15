// src/ai/engine/OnnxExecutor.h
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QMap>
#include <QString>
#include <QStringList>

namespace VideoEditor {

struct EpConfig {
    int deviceIndex = 0;
    bool allowDml = false;
    bool allowCpu = true;
    // When true, a DML session is created with CPU EP fallback disabled so a
    // successful session proves the whole graph runs on DirectML. MI-GAN has
    // a few CPU-only nodes, so it must leave this false and allow node-level
    // CPU fallback inside a DML session (whole-session CPU retry still needs
    // allowCpu).
    bool requirePureDml = false;
};

struct OnnxTensor {
    enum class DataType { Float32, Int32, UInt8 };

    DataType type = DataType::Float32;
    std::vector<int64_t> shape;
    std::vector<float> floatData;
    std::vector<int32_t> int32Data;
    std::vector<uint8_t> uint8Data;
};

struct OnnxOutput {
    enum class DataType { Float32, Int32, UInt8 };

    DataType type = DataType::Float32;
    std::vector<int64_t> shape;
    std::vector<float> floatData;
    std::vector<int32_t> int32Data;
    std::vector<uint8_t> uint8Data;
};

// Thin CPU-first ONNX Runtime session wrapper. DirectML is optional and only
// used when the caller opts in; a failed DML session falls back to CPU and
// records the reason. The ORT types stay inside the .cpp so consumers do not
// depend on the runtime headers.
class OnnxExecutor {
public:
    OnnxExecutor();
    ~OnnxExecutor();
    OnnxExecutor(const OnnxExecutor &) = delete;
    OnnxExecutor &operator=(const OnnxExecutor &) = delete;

    bool initialize(const QStringList &partNames,
                    const QMap<QString, QString> &paths, const EpConfig &config,
                    QString *error = nullptr);
    bool createSession(const QString &partName, const QString &path,
                       const EpConfig &config, QString *error = nullptr);
    bool hasSession(const QString &partName) const;

    QString provider() const;
    bool usingCpuFallback() const;
    QString cpuFallbackReason() const;

    int inputCount(const QString &partName) const;
    int outputCount(const QString &partName) const;
    QString inputName(const QString &partName, int index) const;
    QString outputName(const QString &partName, int index) const;
    bool inputShape(const QString &partName, int index,
                    std::vector<int64_t> *shape) const;
    bool outputShape(const QString &partName, int index,
                     std::vector<int64_t> *shape) const;

    bool run(const QString &partName,
             const QMap<QString, OnnxTensor> &inputs,
             const QStringList &outputNames,
             QMap<QString, OnnxOutput> *outputs, QString *error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor
