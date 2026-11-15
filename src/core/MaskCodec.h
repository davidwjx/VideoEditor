#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

namespace VideoEditor {

// One byte per pixel (0 or 255). RLE sidecar files store the same bits in a
// deterministic little-endian run-length encoding.
struct BinaryMask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bits;

    bool isValid() const
    {
        return width > 0 && height > 0
            && bits.size() == size_t(width) * size_t(height);
    }

    bool at(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < width && y < height
            && bits[size_t(y) * width + x] != 0;
    }

    void set(int x, int y, bool value)
    {
        if (x >= 0 && y >= 0 && x < width && y < height)
            bits[size_t(y) * width + x] = value ? 255 : 0;
    }
};

class MaskCodec {
public:
    static QByteArray encode(const BinaryMask &mask);
    static std::optional<BinaryMask> decode(const QByteArray &data,
                                             QString *error = nullptr);
    static QString hash(const BinaryMask &mask);
};

class MaskEditor {
public:
    static void paintStroke(BinaryMask &mask, QPoint from, QPoint to,
                            int radiusPx, bool erase);
    static void paintBox(BinaryMask &mask, QRect rect, bool erase);
    static QRect contentBounds(const BinaryMask &mask);
    static QImage toOverlay(const BinaryMask &mask, QRgb inside = 0x80FF3B30);
};

} // namespace VideoEditor
