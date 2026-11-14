#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <QString>

namespace VideoEditor {

class D3D11Context;

std::vector<uint8_t> readbackSharedTexture(D3D11Context &context,
                                           ID3D11Texture2D *texture,
                                           QString *error = nullptr);

} // namespace VideoEditor

#endif
