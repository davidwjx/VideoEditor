#include "VideoDecoderFactory.h"

#ifdef _WIN32

#include "D3D11Context.h"
#include "FfmpegD3D11Decoder.h"
#include "FfmpegSoftwareDecoder.h"

namespace VideoEditor {

DecoderCreationResult createVideoDecoder(const QString &path,
                                         D3D11Context &context,
                                         const MediaInfo &info)
{
    DecoderCreationResult result;

    if (context.supportsVideoDecoding()) {
        QString hardwareError;
        if (auto decoder = FfmpegD3D11Decoder::open(path, context, info,
                                                    &hardwareError)) {
            result.decoder = std::move(decoder);
            return result;
        }
        result.hardwareFallbackReason = hardwareError;
    } else {
        result.hardwareFallbackReason = QStringLiteral("D3D11 视频设备不可用。");
    }

    QString softwareError;
    if (auto decoder = FfmpegSoftwareDecoder::open(path, context, info,
                                                   &softwareError)) {
        result.decoder = std::move(decoder);
        return result;
    }

    result.error = QStringLiteral("无法解码视频。\n硬件解码：%1\n软件解码：%2")
                       .arg(result.hardwareFallbackReason, softwareError);
    return result;
}

} // namespace VideoEditor

#endif
