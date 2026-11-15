#pragma once

#include <cmath>
#include <QtGlobal>

namespace VideoEditor {

// PSNR for 8-bit RGB. A perfect match returns 100 dB instead of infinity so
// the value stays printable and comparable across runs.
inline double psnrFromMse(double mse)
{
    if (!(mse > 1e-9))
        return 100.0;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

} // namespace VideoEditor
