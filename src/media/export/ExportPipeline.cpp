#include "ExportPipeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

#include "AudioEncoder.h"
#include "AudioTrackWriter.h"
#include "ExportDiskSpace.h"
#include "Mp4Assembler.h"
#include "OfflineFrameRenderer.h"
#include "SegmentRouter.h"
#include "Srt.h"
#include "TimelineEvaluator.h"
#include "TimelineMixer.h"
#include "VideoEncoder.h"

namespace VideoEditor {

namespace {

constexpr int kSampleRate = 48'000;
constexpr int kChannels = 2;
constexpr int kAudioBitrateKbps = 192;
constexpr int kAudioBlockSamples = 480;
constexpr int kAacFrameSamples = 1024;

} // namespace

ExportResult ExportPipeline::run(const Project &project,
                                 const ExportSettings &settings,
                                 const EncoderCatalog &catalog, bool resume,
                                 const Hooks &hooks)
{
    ExportResult result;
    result.outputPath = normalizedOutputPath(settings.outputPath);
    ExportSettings frozenSettings = settings;
    frozenSettings.outputPath = result.outputPath;
    QString error;

    const QString validationError =
        validateExportSettings(frozenSettings, project.timeline);
    if (!validationError.isEmpty()) {
        result.error = validationError;
        return result;
    }
    const int64_t durationUs = TimelineEvaluator(project).durationUs();
    if (durationUs <= 0) {
        result.error = QStringLiteral("工程时间线为空，无法导出。");
        return result;
    }
    const ExportTiming timing = computeExportTiming(
        durationUs, frozenSettings.fps, kSampleRate,
        frozenSettings.segmentTargetUs);

    // Encoder choice happens before anything is written so an unavailable
    // encoder (notably hardware-only H.265) never leaves an output or a parts
    // directory behind.
    bool hardware = false;
    QString reason;
    QString encoderId;
    if (!frozenSettings.encoderId.isEmpty()) {
        const EncoderDescriptor *descriptor =
            catalog.findByKey(frozenSettings.encoderId);
        // encoderId accepts both the checkpoint key and the FFmpeg name; fall
        // back to automatic selection when it cannot be resolved.
        if (!descriptor) {
            for (const EncoderDescriptor &candidate : catalog.encoders) {
                if (candidate.id == frozenSettings.encoderId
                    && candidate.available) {
                    descriptor = &candidate;
                    break;
                }
            }
        }
        if (descriptor && descriptor->available) {
            encoderId = descriptor->id;
            hardware = descriptor->hardware;
        }
    }
    if (encoderId.isEmpty()) {
        encoderId = catalog.selectEncoder(frozenSettings.codec,
                                          frozenSettings.preferHardware,
                                          &hardware, &reason);
    }
    if (encoderId.isEmpty()) {
        result.error = reason.isEmpty()
                           ? QStringLiteral("没有可用的视频编码器。")
                           : reason;
        return result;
    }
    const EncoderDescriptor *selected = nullptr;
    for (const EncoderDescriptor &candidate : catalog.encoders) {
        if (candidate.id == encoderId && candidate.hardware == hardware
            && candidate.available) {
            selected = &candidate;
            break;
        }
    }
    if (!selected) {
        result.error = QStringLiteral("编码器 %1 不可用。").arg(encoderId);
        return result;
    }
    result.encoderId = selected->key();
    result.usedHardware = hardware;
    if (frozenSettings.codec == ExportVideoCodec::H264 && !hardware
        && frozenSettings.preferHardware) {
        result.warning = QStringLiteral(
            "未找到可用的 H.264 硬件编码器，已使用软件编码（速度较慢）。");
    }

    // Freeze the project once: later edits cannot change this export.
    Project frozen = project;
    // Keep subtitle cues in `frozen` so the SRT sidecar still works when
    // burn-in is off; only the render snapshot hides them.
    Project renderProject = frozen;
    if (!frozenSettings.burnSubtitles) {
        for (Track &track : renderProject.tracks) {
            if (track.type == TrackType::Subtitle)
                track.enabled = false;
        }
    }
    int orphanClipCount = 0;
    for (const Track &track : frozen.tracks) {
        for (const Clip &clip : track.clips) {
            if (!frozen.assetById(clip.assetId))
                ++orphanClipCount;
        }
    }

    const QString outputDir = QFileInfo(result.outputPath).absolutePath();
    if (!QDir().mkpath(outputDir)) {
        result.error = QStringLiteral("无法创建输出目录：%1").arg(outputDir);
        return result;
    }
    const qint64 required = estimateExportBytes(frozenSettings, timing);
    FreeBytesFn freeBytes = hooks.freeBytes;
    if (!freeBytes) {
        freeBytes = [](const QString &path) {
            return availableBytesFor(path);
        };
    }
    auto space = evaluateDiskSpace(freeBytes(outputDir), required);
    if (!space.ok) {
        result.error = space.error;
        return result;
    }

    const QString partsDir = partsDirectoryFor(result.outputPath);
    const QString checkpointPath =
        QDir(partsDir).filePath(QStringLiteral("checkpoint.json"));
    const QString hash = exportProjectHash(frozen, frozenSettings);
    ExportCheckpoint checkpoint;
    checkpoint.version = ExportCheckpoint::kVersion;
    checkpoint.projectHash = hash;
    checkpoint.encoderKey = selected->key();
    checkpoint.width = frozenSettings.width;
    checkpoint.height = frozenSettings.height;
    checkpoint.fps = frozenSettings.fps;
    checkpoint.sampleRate = kSampleRate;
    checkpoint.totalFrames = timing.totalFrames;
    checkpoint.totalSamples = timing.totalSamples;
    checkpoint.segmentSamples = timing.segmentSamples;
    checkpoint.segmentCount = timing.segmentCount;
    checkpoint.completedSegments = 0;
    int startSegment = 0;
    if (resume) {
        const auto loaded = ExportCheckpoint::load(checkpointPath, &error);
        if (loaded && loaded->projectHash == hash
            && loaded->encoderKey == selected->key()
            && loaded->totalFrames == timing.totalFrames
            && loaded->totalSamples == timing.totalSamples
            && loaded->segmentSamples == timing.segmentSamples
            && loaded->width == frozenSettings.width
            && loaded->height == frozenSettings.height
            && loaded->fps == frozenSettings.fps) {
            checkpoint = *loaded;
            checkpoint.version = ExportCheckpoint::kVersion;
            startSegment = std::min(checkpoint.completedSegments,
                                    timing.segmentCount);
            // A checkpoint is a claim, not proof: verify every claimed segment
            // and fall back to the first segment whose content does not match.
            for (int i = 0; i < startSegment; ++i) {
                SegmentCounts counts;
                int64_t vBegin = 0;
                int64_t vEnd = 0;
                videoFrameRangeForSegment(timing, frozenSettings.fps, i,
                                          &vBegin, &vEnd);
                if (!inspectSegment(segmentPath(partsDir, i), &counts, &error)
                    || counts.videoPackets != vEnd - vBegin) {
                    startSegment = i;
                    break;
                }
            }
            for (int i = startSegment; i < timing.segmentCount; ++i)
                QFile::remove(segmentPath(partsDir, i));
            result.resumed = startSegment > 0;
            checkpoint.completedSegments = startSegment;
        } else {
            clearParts(partsDir);
        }
    } else {
        // A fresh export must never mix in parts from an older run.
        clearParts(partsDir);
    }
    if (!QDir().mkpath(partsDir)) {
        result.error = QStringLiteral("无法创建导出临时目录：%1").arg(partsDir);
        return result;
    }

    // The SRT sidecar is written atomically before rendering: a failed write
    // aborts the export instead of producing "video ok, subtitles lost".
    if (frozenSettings.writeSrt) {
        const QString srtPath = frozenSettings.srtPath.isEmpty()
                                    ? defaultSrtPathFor(result.outputPath)
                                    : frozenSettings.srtPath;
        QList<SubtitleCue> cues;
        for (const Track &track : frozen.tracks) {
            if (track.type == TrackType::Subtitle && track.enabled)
                cues.append(track.cues);
        }
        std::sort(cues.begin(), cues.end(),
                  [](const SubtitleCue &a, const SubtitleCue &b) {
                      return a.startUs < b.startUs;
                  });
        const QByteArray srtBytes = writeSrt(cues);
        QSaveFile srt(srtPath);
        if (!srt.open(QIODevice::WriteOnly)
            || srt.write(srtBytes) != srtBytes.size() || !srt.commit()) {
            result.error = QStringLiteral("无法写入 SRT 字幕文件：%1").arg(srtPath);
            return result;
        }
    }

    if (!checkpoint.save(checkpointPath, &error)) {
        result.error = error;
        return result;
    }

    VideoEncoder videoEncoder;
    AudioEncoder audioEncoder;
    OfflineFrameRenderer renderer;
    QStringList warnings;
    if (!videoEncoder.open(*selected, frozenSettings, &error)
        || !audioEncoder.open(kSampleRate, kChannels, kAudioBitrateKbps, &error)
        || !renderer.open(renderProject, frozenSettings.width,
                          frozenSettings.height, &error)) {
        result.error = error;
        return result;
    }
    renderer.setWarningCallback([&warnings](const QString &message) {
        if (!warnings.contains(message))
            warnings.append(message);
    });

    SegmentRouter router(partsDir, timing, frozenSettings.fps,
                         videoEncoder.context());
    TimelineMixer mixer(kSampleRate, kChannels);
    mixer.setProject(frozen);
    float peak = 0.0f;
    const int64_t startedMs = QDateTime::currentMSecsSinceEpoch();
    bool canceled = false;
    // Declared before the progress lambda because the lambda reads them.
    int64_t encodedFrames = 0;
    int64_t fedSamples = 0; // audio always starts at sample 0, even on resume
    int64_t paddedSamples = 0;
    if (startSegment < timing.segmentCount) {
        int64_t vBegin = 0;
        int64_t vEnd = 0;
        videoFrameRangeForSegment(timing, frozenSettings.fps, startSegment,
                                  &vBegin, &vEnd);
        encodedFrames = vBegin;
    }
    int currentSegmentFramesDone = 0;
    int currentSegmentFrameCount = 1;

    const auto report = [&](ExportStage stage) {
        if (!hooks.onProgress)
            return;
        ExportProgress progress;
        progress.stage = stage;
        progress.completedFrames =
            std::min<int64_t>(encodedFrames, timing.totalFrames);
        progress.totalFrames = timing.totalFrames;
        progress.completedSegments =
            std::min(timing.segmentCount,
                     startSegment + router.completedSegments());
        progress.totalSegments = timing.segmentCount;
        if (stage == ExportStage::Rendering) {
            const double withinSegment =
                double(std::clamp(currentSegmentFramesDone, 0,
                                  currentSegmentFrameCount))
                / double(std::max(1, currentSegmentFrameCount));
            const double done = double(progress.completedSegments)
                                + withinSegment;
            progress.percent = int(std::clamp(
                5.0 + 85.0 * done / double(std::max(1, timing.segmentCount)),
                5.0, 90.0));
        } else {
            progress.percent = stage == ExportStage::Preparing ? 2
                : stage == ExportStage::EncodingAudio            ? 3
                : stage == ExportStage::Finalizing               ? 92
                : stage == ExportStage::Validating               ? 96
                : stage == ExportStage::Done                     ? 100
                                                                 : 0;
        }
        progress.elapsedMs = QDateTime::currentMSecsSinceEpoch() - startedMs;
        progress.encodeFps = progress.elapsedMs > 0
                                 ? double(encodedFrames) * 1000.0
                                       / double(progress.elapsedMs)
                                 : 0.0;
        progress.etaMs = progress.encodeFps > 0.0
                             ? int64_t(double(timing.totalFrames
                                              - progress.completedFrames)
                                       * 1000.0 / progress.encodeFps)
                             : -1;
        progress.outputBytes = router.bytesWritten();
        progress.resumed = result.resumed;
        hooks.onProgress(progress);
    };
    report(ExportStage::Preparing);

    std::vector<float> mixBlock(size_t(kAudioBlockSamples) * kChannels, 0.0f);
    std::vector<float> aacBlock(size_t(kAacFrameSamples) * kChannels, 0.0f);
    int aacFill = 0;

    // Audio pass: always from sample 0, even on resume. This keeps one AAC
    // encoder delay at the start of the file and avoids inserting 1024 priming
    // samples at every resume boundary. Audio encoding is far cheaper than
    // video rendering, so redoing it is acceptable.
    const QString audioPartPath =
        QDir(partsDir).filePath(QStringLiteral("audio.mp4"));
    QFile::remove(audioPartPath);
    AudioTrackWriter audioTrack;
    if (!audioTrack.open(audioPartPath, kSampleRate, audioEncoder.context(),
                         timing.totalSamples, &error)) {
        result.error = error;
        return result;
    }
    mixer.setPositionSamples(0);
    for (int64_t sample = 0;
         sample < timing.totalSamples && !canceled && result.error.isEmpty();
         sample += kAudioBlockSamples) {
        if (hooks.isCanceled && hooks.isCanceled()) {
            canceled = true;
            break;
        }
        const int frames = int(
            std::min<int64_t>(kAudioBlockSamples, timing.totalSamples - sample));
        MixStats stats;
        mixer.render(mixBlock.data(), frames, &stats);
        peak = std::max(peak, stats.peak);
        for (int i = 0; i < frames && result.error.isEmpty(); ++i) {
            aacBlock[size_t(aacFill) * kChannels] =
                mixBlock[size_t(i) * kChannels];
            aacBlock[size_t(aacFill) * kChannels + 1] =
                mixBlock[size_t(i) * kChannels + 1];
            if (++aacFill == kAacFrameSamples) {
                std::vector<EncodedPacket> packets;
                if (!audioEncoder.encode(aacBlock.data(), kAacFrameSamples,
                                         fedSamples, &packets, &error)) {
                    result.error = QStringLiteral("音频编码失败：%1").arg(error);
                    break;
                }
                for (const EncodedPacket &packet : packets) {
                    if (!audioTrack.write(packet, &error)) {
                        result.error = QStringLiteral("音频写入失败：%1").arg(error);
                        break;
                    }
                }
                fedSamples += kAacFrameSamples;
                aacFill = 0;
            }
        }
        if (sample % (kAudioBlockSamples * 100) == 0)
            report(ExportStage::EncodingAudio);
    }
    if (!canceled && result.error.isEmpty()) {
        if (aacFill > 0) {
            std::fill(aacBlock.begin() + aacFill * kChannels, aacBlock.end(),
                      0.0f);
            paddedSamples = kAacFrameSamples - aacFill;
            std::vector<EncodedPacket> packets;
            if (!audioEncoder.encode(aacBlock.data(), kAacFrameSamples,
                                     fedSamples, &packets, &error)) {
                result.error =
                    QStringLiteral("音频尾帧编码失败：%1").arg(error);
            } else {
                for (const EncodedPacket &packet : packets) {
                    if (!audioTrack.write(packet, &error)) {
                        result.error =
                            QStringLiteral("音频写入失败：%1").arg(error);
                        break;
                    }
                }
                fedSamples += kAacFrameSamples;
            }
        }
        std::vector<EncodedPacket> audioTail;
        if (result.error.isEmpty() && !audioEncoder.flush(&audioTail, &error))
            result.error = error;
        for (const EncodedPacket &packet : audioTail) {
            if (!audioTrack.write(packet, &error)) {
                result.error = QStringLiteral("音频写入失败：%1").arg(error);
                break;
            }
        }
        if (result.error.isEmpty() && !audioTrack.finish(&error))
            result.error = error;
    }
    if (canceled || !result.error.isEmpty()) {
        audioTrack.finish(nullptr);
        QFile::remove(audioPartPath);
        checkpoint.lastError = result.error;
        checkpoint.save(checkpointPath, nullptr);
        result.canceled = canceled;
        result.resumable = checkpoint.completedSegments > 0;
        result.audioPeak = peak;
        return result;
    }

    for (int segment = startSegment; segment < timing.segmentCount; ++segment) {
        if (hooks.isCanceled && hooks.isCanceled()) {
            canceled = true;
            break;
        }
        if (hooks.failAtSegment == segment) {
            result.error = QStringLiteral("模拟分段写入失败（测试注入）。");
            break;
        }
        // Re-check free space before every segment: an export can fill the disk
        // long after start. Keep completed segments and the checkpoint.
        const double remainingFraction =
            double(timing.segmentCount - segment)
            / double(std::max(1, timing.segmentCount));
        space = evaluateDiskSpace(
            freeBytes(outputDir),
            std::max<qint64>(64LL << 20,
                             qint64(double(required) * remainingFraction)));
        if (!space.ok) {
            result.error = space.error;
            break;
        }
        int64_t vBegin = 0;
        int64_t vEnd = 0;
        videoFrameRangeForSegment(timing, frozenSettings.fps, segment, &vBegin,
                                  &vEnd);
        currentSegmentFrameCount = int(std::max<int64_t>(1, vEnd - vBegin));
        currentSegmentFramesDone = 0;

        for (int64_t frame = vBegin; frame < vEnd; ++frame) {
            if (hooks.isCanceled && hooks.isCanceled()) {
                canceled = true;
                break;
            }
            const int64_t ptsUs = framePtsUs(frame, frozenSettings.fps);
            std::vector<uint8_t> rgba;
            if (!renderer.renderFrame(ptsUs, &rgba, &error)) {
                result.error = QStringLiteral("第 %1 帧渲染失败：%2")
                                   .arg(frame)
                                   .arg(error);
                break;
            }
            std::vector<EncodedPacket> packets;
            if (!videoEncoder.encodeRgba(rgba.data(), frame, frame == vBegin,
                                         &packets, &error)) {
                result.error = QStringLiteral("第 %1 帧编码失败：%2")
                                   .arg(frame)
                                   .arg(error);
                break;
            }
            for (const EncodedPacket &packet : packets) {
                if (!router.routeVideo(packet, &error)) {
                    result.error = error;
                    break;
                }
            }
            if (!result.error.isEmpty())
                break;
            ++encodedFrames;
            ++currentSegmentFramesDone;
            if (encodedFrames % 10 == 0)
                report(ExportStage::Rendering);
        }
        if (canceled || !result.error.isEmpty())
            break;

        if (segment + 4 < timing.segmentCount && segment >= 3) {
            if (!router.closeThrough(segment - 3, &error)) {
                result.error = error;
                break;
            }
        }
        report(ExportStage::Rendering);
    }

    std::vector<EncodedPacket> videoTail;
    if (!canceled && result.error.isEmpty()) {
        if (!videoEncoder.flush(&videoTail, &error)) {
            result.error = error;
        }
        for (const EncodedPacket &packet : videoTail) {
            if (!router.routeVideo(packet, &error)) {
                result.error = error;
                break;
            }
        }
    }

    // Always close file handles. On cancel/failure the partially written last
    // segment is deleted; only fully validated segments count as complete.
    const bool successSoFar = !canceled && result.error.isEmpty();
    const bool success = router.finishAll(successSoFar, &error);
    if (!success && result.error.isEmpty())
        result.error = error;
    checkpoint.completedSegments =
        success ? timing.segmentCount
                : std::min(timing.segmentCount,
                           startSegment + router.completedSegments());
    checkpoint.bytesWritten = router.bytesWritten();
    checkpoint.lastError = result.error;
    checkpoint.save(checkpointPath, nullptr);

    if (canceled) {
        result.canceled = true;
        result.resumable = checkpoint.completedSegments > 0;
        result.audioPeak = peak;
        return result;
    }
    if (!result.error.isEmpty()) {
        result.resumable = checkpoint.completedSegments > 0;
        result.audioPeak = peak;
        return result;
    }

    report(ExportStage::Finalizing);
    const QString temp = assemblyTempPathFor(result.outputPath);
    if (!Mp4Assembler::assemble(partsDir, audioPartPath, timing,
                                frozenSettings.fps, videoEncoder.context(),
                                audioEncoder.context(),
                                audioEncoder.encoderDelaySamples(),
                                paddedSamples, temp, &error)) {
        result.error = error;
        result.resumable = true;
        result.audioPeak = peak;
        return result;
    }
    report(ExportStage::Validating);
    result.validation = validateExportFile(temp, frozenSettings, timing);
    if (!result.validation.ok) {
        result.error =
            QStringLiteral("导出文件校验失败：%1").arg(result.validation.error);
        result.resumable = true;
        result.audioPeak = peak;
        QFile::remove(temp);
        return result;
    }
    if (QFileInfo::exists(result.outputPath)
        && !QFile::remove(result.outputPath)) {
        result.error =
            QStringLiteral("无法覆盖已存在的导出文件：%1").arg(result.outputPath);
        // The assembled temp and every part are still intact; a retry can
        // finish without re-rendering.
        result.resumable = true;
        result.audioPeak = peak;
        return result;
    }
    if (!QFile::rename(temp, result.outputPath)) {
        result.error =
            QStringLiteral("无法写入最终导出文件：%1").arg(result.outputPath);
        result.resumable = true;
        result.audioPeak = peak;
        return result;
    }
    clearParts(partsDir);
    result.ok = true;
    result.audioPeak = peak;
    QStringList allWarnings;
    const QString hardwareWarning = result.warning; // set before rendering
    if (!hardwareWarning.isEmpty())
        allWarnings.append(hardwareWarning);
    if (orphanClipCount > 0) {
        allWarnings.append(
            QStringLiteral("有 %1 个片段引用的素材缺失，已按黑场处理。")
                .arg(orphanClipCount));
    }
    if (!warnings.isEmpty()) {
        allWarnings.append(
            QStringLiteral("部分素材无法解码，已按黑场处理：\n")
            + warnings.join(QStringLiteral("\n")));
    }
    result.warning = allWarnings.join(QStringLiteral("\n"));
    report(ExportStage::Done);
    return result;
}

} // namespace VideoEditor
