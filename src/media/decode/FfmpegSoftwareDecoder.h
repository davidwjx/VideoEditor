#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;

class FfmpegSoftwareDecoder final : public IVideoDecoder {
public:
    static std::unique_ptr<FfmpegSoftwareDecoder> open(const QString &path,
                                                       D3D11Context &context,
                                                       const MediaInfo &info,
                                                       QString *error = nullptr);
    ~FfmpegSoftwareDecoder() override;

    bool seek(int64_t ptsUs, QString *error = nullptr) override;
    bool decodeNext(DecodedFrame &frame, QString *error = nullptr) override;
    const MediaInfo &info() const override;
    bool isHardware() const override { return false; }

private:
    FfmpegSoftwareDecoder();
    bool decodeRawFrame(AVFrame **frame, QString *error);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor

#endif
