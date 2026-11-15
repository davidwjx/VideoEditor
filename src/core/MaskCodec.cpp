#include "MaskCodec.h"

#include <cstring>
#include <QCryptographicHash>

namespace VideoEditor {

namespace {

constexpr int kMaxMaskDimension = 8192;
constexpr char kMagic[8] = {'V', 'E', 'M', 'A', 'S', 'K', '0', '1'};

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

void appendUint32(QByteArray &out, quint32 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 24) & 0xff));
}

quint32 readUint32(const QByteArray &data, int offset)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return quint32(bytes[0])
        | (quint32(bytes[1]) << 8)
        | (quint32(bytes[2]) << 16)
        | (quint32(bytes[3]) << 24);
}

} // namespace

QByteArray MaskCodec::encode(const BinaryMask &mask)
{
    const QByteArray invalid;
    if (!mask.isValid())
        return invalid;

    const qint64 pixelCount = qint64(mask.width) * qint64(mask.height);

    // Runs alternate 0/255 and always start with the 0 run. A mask whose first
    // pixel is set therefore begins with a zero-length run.
    std::vector<quint32> runs{0};
    int current = 0;
    for (qint64 i = 0; i < pixelCount; ++i) {
        const int value = mask.bits[size_t(i)] != 0 ? 1 : 0;
        if (value != current) {
            runs.push_back(0);
            current = value;
        }
        ++runs.back();
    }

    QByteArray out;
    out.reserve(20 + int(runs.size()) * 4);
    out.append(kMagic, 8);
    appendUint32(out, quint32(mask.width));
    appendUint32(out, quint32(mask.height));
    appendUint32(out, quint32(runs.size()));
    for (quint32 run : runs)
        appendUint32(out, run);
    return out;
}

std::optional<BinaryMask> MaskCodec::decode(const QByteArray &data,
                                            QString *error)
{
    if (data.size() < 20) {
        setError(error, QStringLiteral("遮罩数据被截断，无法读取文件头。"));
        return std::nullopt;
    }
    if (std::memcmp(data.constData(), kMagic, 8) != 0) {
        setError(error, QStringLiteral("遮罩数据格式无效：文件头不匹配。"));
        return std::nullopt;
    }

    const quint32 width = readUint32(data, 8);
    const quint32 height = readUint32(data, 12);
    const quint32 runCount = readUint32(data, 16);
    if (width == 0 || height == 0
        || width > quint32(kMaxMaskDimension)
        || height > quint32(kMaxMaskDimension)) {
        setError(error, QStringLiteral("遮罩尺寸无效，必须在 1–8192 像素之间。"));
        return std::nullopt;
    }

    const qint64 remaining = data.size() - 20;
    if (qint64(runCount) * 4 > remaining) {
        setError(error, QStringLiteral("遮罩数据被截断：run 数量超出文件长度。"));
        return std::nullopt;
    }

    const qint64 pixelCount = qint64(width) * qint64(height);
    qint64 total = 0;
    for (quint32 i = 0; i < runCount; ++i)
        total += readUint32(data, 20 + int(i) * 4);
    if (total != pixelCount) {
        setError(error, QStringLiteral("遮罩数据无效：run 总长度与宽高不一致。"));
        return std::nullopt;
    }

    BinaryMask mask;
    mask.width = int(width);
    mask.height = int(height);
    mask.bits.assign(size_t(pixelCount), 0);

    qint64 index = 0;
    for (quint32 i = 0; i < runCount; ++i) {
        const quint32 run = readUint32(data, 20 + int(i) * 4);
        const uint8_t value = (i % 2 == 0) ? uint8_t(0) : uint8_t(255);
        for (quint32 n = 0; n < run; ++n)
            mask.bits[size_t(index++)] = value;
    }
    return mask;
}

QString MaskCodec::hash(const BinaryMask &mask)
{
    const QByteArray encoded = encode(mask);
    if (encoded.isEmpty())
        return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
}

} // namespace VideoEditor
