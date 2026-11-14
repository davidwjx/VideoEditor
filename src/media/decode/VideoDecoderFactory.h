#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;

struct DecoderCreationResult {
    std::unique_ptr<IVideoDecoder> decoder;
    QString error;
    QString hardwareFallbackReason;
};

DecoderCreationResult createVideoDecoder(const QString &path,
                                         D3D11Context &context,
                                         const MediaInfo &info);

} // namespace VideoEditor

#endif
