#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;

class FfmpegD3D11Decoder final : public IVideoDecoder {
public:
    static std::unique_ptr<FfmpegD3D11Decoder> open(const QString &path,
                                                    D3D11Context &context,
                                                    const MediaInfo &info,
                                                    QString *error = nullptr);
    ~FfmpegD3D11Decoder() override;

    bool seek(int64_t ptsUs, QString *error = nullptr) override;
    bool decodeNext(DecodedFrame &frame, QString *error = nullptr) override;
    const MediaInfo &info() const override;
    bool isHardware() const override { return true; }

private:
    FfmpegD3D11Decoder();
    bool decodeRawFrame(AVFrame **frame, QString *error);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor

#endif
