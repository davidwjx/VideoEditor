#include "ExportSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QDir>
#include <QFileInfo>
#include <QJsonValue>

namespace VideoEditor {

namespace {

int64_t roundDiv(int64_t num, int64_t den)
{
    if (den <= 0)
        return 0;
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

int evenDimension(int value)
{
    return std::max(2, (value / 2) * 2);
}

int clampToInt(int64_t value)
{
    if (value < 0)
        return 0;
    if (value > int64_t(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return int(value);
}

QString rateControlToString(ExportRateControl rateControl)
{
    return rateControl == ExportRateControl::Bitrate ? QStringLiteral("bitrate")
                                                     : QStringLiteral("quality");
}

bool rateControlFromString(const QString &text, ExportRateControl *rateControl)
{
    if (text.compare(QStringLiteral("bitrate"), Qt::CaseInsensitive) == 0) {
        if (rateControl)
            *rateControl = ExportRateControl::Bitrate;
        return true;
    }
    if (text.compare(QStringLiteral("quality"), Qt::CaseInsensitive) == 0) {
        if (rateControl)
            *rateControl = ExportRateControl::Quality;
        return true;
    }
    return false;
}

QString qualityToString(ExportQuality quality)
{
    switch (quality) {
    case ExportQuality::Low:
        return QStringLiteral("low");
    case ExportQuality::Medium:
        return QStringLiteral("medium");
    case ExportQuality::High:
        return QStringLiteral("high");
    }
    return QStringLiteral("medium");
}

bool qualityFromString(const QString &text, ExportQuality *quality)
{
    if (text.compare(QStringLiteral("low"), Qt::CaseInsensitive) == 0) {
        if (quality)
            *quality = ExportQuality::Low;
        return true;
    }
    if (text.compare(QStringLiteral("medium"), Qt::CaseInsensitive) == 0) {
        if (quality)
            *quality = ExportQuality::Medium;
        return true;
    }
    if (text.compare(QStringLiteral("high"), Qt::CaseInsensitive) == 0) {
        if (quality)
            *quality = ExportQuality::High;
        return true;
    }
    return false;
}

} // namespace

QString exportCodecToString(ExportVideoCodec codec)
{
    switch (codec) {
    case ExportVideoCodec::Mpeg4Part2:
        return QStringLiteral("mpeg4");
    case ExportVideoCodec::H264:
        return QStringLiteral("h264");
    case ExportVideoCodec::H265:
        return QStringLiteral("h265");
    }
    return QStringLiteral("h264");
}

QString exportCodecDisplayName(ExportVideoCodec codec)
{
    switch (codec) {
    case ExportVideoCodec::Mpeg4Part2:
        return QStringLiteral("MPEG-4 Part 2");
    case ExportVideoCodec::H264:
        return QStringLiteral("H.264");
    case ExportVideoCodec::H265:
        return QStringLiteral("H.265");
    }
    return QStringLiteral("H.264");
}

QString exportCodecFfmpegName(ExportVideoCodec codec)
{
    // Values match avcodec_get_name() output, which MediaProbe stores.
    switch (codec) {
    case ExportVideoCodec::Mpeg4Part2:
        return QStringLiteral("mpeg4");
    case ExportVideoCodec::H264:
        return QStringLiteral("h264");
    case ExportVideoCodec::H265:
        return QStringLiteral("hevc");
    }
    return QStringLiteral("h264");
}

bool exportCodecFromString(const QString &text, ExportVideoCodec *codec)
{
    const QString trimmed = text.trimmed();
    if (trimmed.compare(QStringLiteral("mpeg4"), Qt::CaseInsensitive) == 0) {
        if (codec)
            *codec = ExportVideoCodec::Mpeg4Part2;
        return true;
    }
    if (trimmed.compare(QStringLiteral("h264"), Qt::CaseInsensitive) == 0) {
        if (codec)
            *codec = ExportVideoCodec::H264;
        return true;
    }
    if (trimmed.compare(QStringLiteral("h265"), Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("hevc"), Qt::CaseInsensitive) == 0) {
        if (codec)
            *codec = ExportVideoCodec::H265;
        return true;
    }
    return false;
}

QList<ExportPreset> builtinExportPresets()
{
    return {
        {QStringLiteral("match"), QStringLiteral("匹配工程（H.264）"),
         ExportVideoCodec::H264, 0, 0, Rational{0, 1}, true,
         ExportQuality::High, 0},
        {QStringLiteral("h264-1080p30"), QStringLiteral("H.264 1080p30"),
         ExportVideoCodec::H264, 1920, 1080, Rational{30, 1}, false,
         ExportQuality::High, 12'000},
        {QStringLiteral("h264-720p30"), QStringLiteral("H.264 720p30"),
         ExportVideoCodec::H264, 1280, 720, Rational{30, 1}, false,
         ExportQuality::Medium, 6'000},
        {QStringLiteral("h265-1080p30"), QStringLiteral("H.265 1080p30（硬件）"),
         ExportVideoCodec::H265, 1920, 1080, Rational{30, 1}, false,
         ExportQuality::High, 8'000},
        {QStringLiteral("mpeg4-1080p30"), QStringLiteral("MPEG-4 Part 2 1080p30"),
         ExportVideoCodec::Mpeg4Part2, 1920, 1080, Rational{30, 1}, false,
         ExportQuality::High, 10'000},
    };
}

ExportSettings settingsFromPreset(const ExportPreset &preset,
                                  const TimelineSettings &timeline)
{
    ExportSettings settings;
    settings.codec = preset.codec;
    settings.width = preset.matchTimeline ? timeline.width : preset.width;
    settings.height = preset.matchTimeline ? timeline.height : preset.height;
    settings.fps = preset.matchTimeline ? timeline.fps : preset.fps;
    settings.quality = preset.quality;
    settings.bitrateKbps = preset.bitrateKbps;
    // A preset bitrate is an explicit target; otherwise the quality tier
    // derives the bitrate from resolution and frame rate. ExportRateControl::
    // Quality would ignore a non-zero preset bitrate, so presets that carry
    // one select Bitrate mode. The brief does not pin this field explicitly.
    settings.rateControl = preset.bitrateKbps > 0 ? ExportRateControl::Bitrate
                                                  : ExportRateControl::Quality;
    settings.width = evenDimension(settings.width);
    settings.height = evenDimension(settings.height);
    return settings;
}

int effectiveBitrateKbps(const ExportSettings &settings)
{
    if (settings.rateControl == ExportRateControl::Bitrate
        && settings.bitrateKbps > 0) {
        return std::clamp(settings.bitrateKbps, 100, 200'000);
    }
    double bitsPerPixel = 0.10;
    switch (settings.quality) {
    case ExportQuality::Low: bitsPerPixel = 0.06; break;
    case ExportQuality::Medium: bitsPerPixel = 0.10; break;
    case ExportQuality::High: bitsPerPixel = 0.16; break;
    }
    const double fps = settings.fps.isValid() ? settings.fps.toDouble() : 30.0;
    const double kbps = double(settings.width) * settings.height * fps
                        * bitsPerPixel / 1000.0;
    const int64_t rounded = std::llround(kbps);
    return clampToInt(std::clamp<int64_t>(rounded, 500, 100'000));
}

int64_t framePtsUs(int64_t frame, const Rational &fps)
{
    if (!fps.isValid())
        return 0;
    return roundDiv(frame * 1'000'000LL * fps.den, fps.num);
}

int64_t firstVideoFrameAtOrAfter(int64_t timeUs, const Rational &fps)
{
    if (!fps.isValid())
        return 0;
    if (timeUs <= 0)
        return 0;
    // ceil(timeUs * fps.num / (1e6 * fps.den)), then correct for PTS rounding.
    int64_t frame = (timeUs * fps.num + int64_t(fps.den) * 1'000'000LL - 1)
                    / (int64_t(fps.den) * 1'000'000LL);
    while (frame > 0 && framePtsUs(frame - 1, fps) >= timeUs)
        --frame;
    while (framePtsUs(frame, fps) < timeUs)
        ++frame;
    return frame;
}

ExportTiming computeExportTiming(int64_t durationUs, const Rational &fps,
                                 int sampleRate, int segmentTargetUs)
{
    ExportTiming timing;
    timing.sampleRate = sampleRate;
    if (!fps.isValid() || durationUs <= 0 || sampleRate <= 0)
        return timing;
    timing.totalFrames = std::max<int64_t>(
        1, roundDiv(durationUs * fps.num, int64_t(fps.den) * 1'000'000LL));
    timing.totalSamples = std::max<int64_t>(
        1, roundDiv(timing.totalFrames * sampleRate * fps.den, fps.num));

    const int64_t targetSamples = std::max<int64_t>(
        1024, roundDiv(int64_t(segmentTargetUs) * sampleRate, 1'000'000LL));
    const int64_t aacFrames = std::max<int64_t>(1, (targetSamples + 512) / 1024);
    timing.segmentSamples = aacFrames * 1024;
    timing.segmentCount = clampToInt(
        (timing.totalSamples + timing.segmentSamples - 1) / timing.segmentSamples);
    // An audio tail shorter than one video frame can push the last sample
    // boundary past the last frame PTS (for example a 10.032 s MP3 under a
    // 10 s video at 30 fps lives 26.7 ms into its own segment). Such a
    // trailing segment contains no video frame and can never produce a part.
    // Fold empty trailing segments into the previous one; the audio track is
    // encoded continuously from totalSamples and is unaffected.
    while (timing.segmentCount > 1) {
        int64_t begin = 0;
        int64_t end = 0;
        videoFrameRangeForSegment(timing, fps, timing.segmentCount - 1, &begin,
                                  &end);
        if (end > begin)
            break;
        --timing.segmentCount;
    }
    return timing;
}

int64_t segmentStartUs(const ExportTiming &timing, int segmentIndex)
{
    return roundDiv(int64_t(segmentIndex) * timing.segmentSamples * 1'000'000LL,
                    timing.sampleRate);
}

void audioSampleRangeForSegment(const ExportTiming &timing, int segmentIndex,
                                int64_t *sampleBegin, int64_t *sampleEnd)
{
    const int64_t begin = int64_t(segmentIndex) * timing.segmentSamples;
    // The final segment owns the whole remaining audio tail, including any
    // samples that were folded in when an empty trailing segment was removed.
    const bool lastSegment =
        timing.segmentCount > 0 && segmentIndex == timing.segmentCount - 1;
    const int64_t end =
        lastSegment ? timing.totalSamples
                    : std::min(timing.totalSamples,
                               begin + timing.segmentSamples);
    if (sampleBegin)
        *sampleBegin = begin;
    if (sampleEnd)
        *sampleEnd = std::max(begin, end);
}

void videoFrameRangeForSegment(const ExportTiming &timing, const Rational &fps,
                               int segmentIndex, int64_t *frameBegin,
                               int64_t *frameEnd)
{
    int64_t begin =
        firstVideoFrameAtOrAfter(segmentStartUs(timing, segmentIndex), fps);
    if (begin > timing.totalFrames)
        begin = timing.totalFrames;
    int64_t end = timing.totalFrames;
    if (segmentIndex + 1 < timing.segmentCount) {
        end = firstVideoFrameAtOrAfter(segmentStartUs(timing, segmentIndex + 1),
                                       fps);
    }
    end = std::max(begin, std::min(end, timing.totalFrames));
    if (frameBegin)
        *frameBegin = begin;
    if (frameEnd)
        *frameEnd = end;
}

QString validateExportSettings(const ExportSettings &settings,
                               const TimelineSettings &timeline)
{
    Q_UNUSED(timeline);
    if (settings.outputPath.trimmed().isEmpty())
        return QStringLiteral("请选择导出文件路径。");
    if (settings.width < 16 || settings.height < 16
        || settings.width > 7680 || settings.height > 4320) {
        return QStringLiteral("导出分辨率超出范围（16×16 到 7680×4320）。");
    }
    if (settings.width % 2 != 0 || settings.height % 2 != 0)
        return QStringLiteral("导出宽高必须是偶数。");
    // Validate the actual rate, not the raw numerator: NTSC rates such as
    // 30000/1001 and 24000/1001 have num > 120, and unreduced rationals such
    // as 240/2 are equivalent to 120 fps.
    if (!settings.fps.isValid() || settings.fps.toDouble() < 1.0
        || settings.fps.toDouble() > 120.0) {
        return QStringLiteral("导出帧率必须在 1 到 120 fps 之间。");
    }
    if (settings.rateControl == ExportRateControl::Bitrate
        && (settings.bitrateKbps < 100 || settings.bitrateKbps > 200'000)) {
        return QStringLiteral("码率必须在 100 到 200000 kbps 之间。");
    }
    if (settings.segmentTargetUs < 100'000 || settings.segmentTargetUs > 60'000'000)
        return QStringLiteral("分段长度必须在 0.1 到 60 秒之间。");
    return {};
}

QString normalizedOutputPath(QString path)
{
    path = path.trimmed();
    if (path.isEmpty())
        return path;
    const QFileInfo info(path);
    if (info.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) != 0)
        path += QStringLiteral(".mp4");
    return path;
}

QString defaultSrtPathFor(const QString &outputPath)
{
    const QFileInfo info(outputPath);
    // cleanPath keeps relative paths relative while removing the "./" that
    // QFileInfo::dir() yields for a bare file name.
    return QDir::cleanPath(
        info.dir().filePath(info.completeBaseName() + QStringLiteral(".srt")));
}

QJsonObject exportSettingsToJson(const ExportSettings &settings)
{
    QJsonObject json;
    json.insert(QStringLiteral("codec"), exportCodecToString(settings.codec));
    json.insert(QStringLiteral("width"), settings.width);
    json.insert(QStringLiteral("height"), settings.height);
    json.insert(QStringLiteral("fpsNum"), settings.fps.num);
    json.insert(QStringLiteral("fpsDen"), settings.fps.den);
    json.insert(QStringLiteral("rateControl"),
                rateControlToString(settings.rateControl));
    json.insert(QStringLiteral("quality"), qualityToString(settings.quality));
    json.insert(QStringLiteral("bitrateKbps"), settings.bitrateKbps);
    json.insert(QStringLiteral("preferHardware"), settings.preferHardware);
    json.insert(QStringLiteral("burnSubtitles"), settings.burnSubtitles);
    json.insert(QStringLiteral("writeSrt"), settings.writeSrt);
    json.insert(QStringLiteral("outputPath"), settings.outputPath);
    json.insert(QStringLiteral("srtPath"), settings.srtPath);
    json.insert(QStringLiteral("encoderId"), settings.encoderId);
    json.insert(QStringLiteral("segmentTargetUs"), settings.segmentTargetUs);
    return json;
}

std::optional<ExportSettings> exportSettingsFromJson(const QJsonObject &json,
                                                     QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &message)
        -> std::optional<ExportSettings> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    const QString codecText =
        json.value(QStringLiteral("codec")).toString().trimmed();
    if (codecText.isEmpty())
        return fail(QStringLiteral("导出设置缺少视频编码字段。"));
    ExportVideoCodec codec = ExportVideoCodec::H264;
    if (!exportCodecFromString(codecText, &codec))
        return fail(QStringLiteral("无法识别的视频编码：%1。").arg(codecText));

    ExportSettings settings;
    settings.codec = codec;
    settings.width = json.value(QStringLiteral("width")).toInt(settings.width);
    settings.height = json.value(QStringLiteral("height")).toInt(settings.height);
    settings.fps.num = json.value(QStringLiteral("fpsNum")).toInt(settings.fps.num);
    settings.fps.den = json.value(QStringLiteral("fpsDen")).toInt(settings.fps.den);
    if (!settings.fps.isValid())
        return fail(QStringLiteral("导出帧率无效。"));

    const QJsonValue rateControlValue =
        json.value(QStringLiteral("rateControl"));
    if (!rateControlValue.isUndefined()
        && !rateControlFromString(rateControlValue.toString(),
                                  &settings.rateControl)) {
        return fail(QStringLiteral("无法识别的码率控制方式：%1。")
                        .arg(rateControlValue.toString()));
    }
    const QJsonValue qualityValue = json.value(QStringLiteral("quality"));
    if (!qualityValue.isUndefined()
        && !qualityFromString(qualityValue.toString(), &settings.quality)) {
        return fail(QStringLiteral("无法识别的画质档位：%1。")
                        .arg(qualityValue.toString()));
    }

    settings.bitrateKbps =
        json.value(QStringLiteral("bitrateKbps")).toInt(settings.bitrateKbps);
    settings.preferHardware =
        json.value(QStringLiteral("preferHardware")).toBool(settings.preferHardware);
    settings.burnSubtitles =
        json.value(QStringLiteral("burnSubtitles")).toBool(settings.burnSubtitles);
    settings.writeSrt =
        json.value(QStringLiteral("writeSrt")).toBool(settings.writeSrt);
    settings.outputPath = json.value(QStringLiteral("outputPath")).toString();
    settings.srtPath = json.value(QStringLiteral("srtPath")).toString();
    settings.encoderId = json.value(QStringLiteral("encoderId")).toString();
    settings.segmentTargetUs =
        json.value(QStringLiteral("segmentTargetUs")).toInt(settings.segmentTargetUs);
    return settings;
}

} // namespace VideoEditor
