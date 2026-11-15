#include "ExportPipeline.h"

#include <algorithm>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "AudioEncoder.h"
#include "AudioTrackWriter.h"
#include "ExportDiskSpace.h"
#include "Mp4Assembler.h"
#include "OfflineFrameRenderer.h"
#include "SegmentRouter.h"
#include "SegmentWriter.h"
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

void appendWarning(ExportResult *result, const QString &message)
{
    if (result->warning.isEmpty())
        result->warning = message;
    else
        result->warning += QStringLiteral("\n") + message;
}

// Reads the video stream extradata of an MP4 part. extradata is empty for a
// stream that carries none.
bool readVideoExtradata(const QString &path, std::vector<uint8_t> *extradata)
{
    extradata->clear();
    AVFormatContext *in = nullptr;
    if (avformat_open_input(&in, path.toUtf8().constData(), nullptr, nullptr)
        < 0)
        return false;
    // Extradata normally comes straight from the container header, but a
    // one-frame probe file may need stream info before codecpar is complete.
    avformat_find_stream_info(in, nullptr);
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr,
                                       0);
    bool ok = false;
    if (vi >= 0) {
        const AVCodecParameters *par = in->streams[vi]->codecpar;
        if (par->extradata && par->extradata_size > 0) {
            extradata->assign(par->extradata,
                              par->extradata + par->extradata_size);
        }
        ok = true;
    }
    avformat_close_input(&in);
    return ok;
}

// Writes a throwaway one-frame part with a fresh encoder instance of the same
// descriptor and reads back the extradata the MP4 muxer publishes. The
// parameter sets then live in the container-native form (avcC/hvcC/VOL),
// directly comparable with the parts already on disk. A dedicated instance is
// used so the probe frame (and any packet the hardware encoder delays) can
// never leak into the real export stream with a stale pts.
bool probeSegmentExtradata(const EncoderDescriptor &descriptor,
                           const ExportSettings &settings, const Rational &fps,
                           const QString &scratchPath,
                           std::vector<uint8_t> *extradata)
{
    VideoEncoder probe;
    QString error;
    if (!probe.open(descriptor, settings, &error))
        return false;
    std::vector<EncodedPacket> packets;
    if (!probe.encodeBlack(0, true, &packets, &error))
        return false;
    std::vector<EncodedPacket> tail;
    if (!probe.flush(&tail, &error))
        return false;
    packets.insert(packets.end(), tail.begin(), tail.end());
    SegmentWriter writer;
    if (!writer.open(scratchPath, fps, probe.context(), 0, &error))
        return false;
    bool ok = true;
    for (const EncodedPacket &packet : packets) {
        if (!writer.writeVideo(packet, &error)) {
            ok = false;
            break;
        }
    }
    if (ok && !writer.finish(&error))
        ok = false;
    if (!ok) {
        writer.finish(nullptr);
        QFile::remove(scratchPath);
        return false;
    }
    std::vector<uint8_t> probeExtradata;
    const bool read = readVideoExtradata(scratchPath, &probeExtradata);
    QFile::remove(scratchPath);
    if (!read)
        return false;
    *extradata = std::move(probeExtradata);
    return true;
}

} // namespace

ExportResult ExportPipeline::run(const Project &project,
                                 const ExportSettings &settings,
                                 const EncoderCatalog &catalog, bool resume,
                                 const Hooks &hooks,
                                 const QString &projectPath)
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
    // This guard must run before mkpath/clearParts: writing parts or replacing
    // the destination would otherwise destroy an imported asset or project.
    const QString destinationError =
        validateExportDestinations(frozenSettings, project, projectPath);
    if (!destinationError.isEmpty()) {
        result.error = destinationError;
        return result;
    }
    const int64_t durationUs = TimelineEvaluator(project).durationUs();
    if (durationUs <= 0) {
        result.error = QStringLiteral("工程时间线为空，无法导出。");
        return result;
    }
    // Removal preflight runs before anything is created on disk. A missing or
    // expired cache would otherwise let the renderer encode the original
    // frame, which is exactly what the removal feature promises not to do.
    if (hooks.removalPreflight) {
        const RemovalPreflightResult preflight =
            hooks.removalPreflight(project);
        if (!preflight.ok) {
            result.error =
                preflight.message.isEmpty()
                    ? QStringLiteral(
                          "AI 修复结果缺失或已过期。请先生成修复，再导出。")
                    : preflight.message;
            return result;
        }
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
    bool encoderParametersChanged = false;
    // True when the parts directory belongs to this run only (fresh export, or
    // a resume whose checkpoint was rejected). An early failure may then clean
    // it up; a valid resume must keep its parts.
    bool freshPartsDir = false;
    // A part is usable only when it opens, starts on a keyframe and carries
    // exactly the packet count its timeline range requires.
    const auto segmentIsComplete = [&](int index, QString *inspectError) {
        int64_t vBegin = 0;
        int64_t vEnd = 0;
        videoFrameRangeForSegment(timing, frozenSettings.fps, index, &vBegin,
                                  &vEnd);
        SegmentCounts counts;
        if (!inspectSegment(segmentPath(partsDir, index), &counts,
                            inspectError)) {
            return false;
        }
        return counts.videoPackets == vEnd - vBegin;
    };
    if (resume) {
        error.clear();
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
                if (!segmentIsComplete(i, &error)) {
                    startSegment = i;
                    break;
                }
            }
            // A crash between closing a part and saving the checkpoint leaves
            // complete parts past the claim on disk. Adopt them instead of
            // re-rendering work that is already valid; resume at the first
            // segment that is missing or broken.
            if (startSegment == checkpoint.completedSegments) {
                while (startSegment < timing.segmentCount
                       && segmentIsComplete(startSegment, &error)) {
                    ++startSegment;
                }
            }
            // A reopened encoder may publish different SPS/PPS (hardware
            // encoders in particular). Such parts can never be joined by the
            // assembler, so detect it now and restart instead of failing
            // every retry.
            if (startSegment > 0) {
                std::vector<uint8_t> partExtradata;
                std::vector<uint8_t> encoderExtradata;
                const QString scratchPath =
                    QDir(partsDir).filePath(
                        QStringLiteral("parameter_probe.mp4"));
                const bool readPart =
                    readVideoExtradata(segmentPath(partsDir, 0), &partExtradata);
                const bool probed = probeSegmentExtradata(
                    *selected, frozenSettings, frozenSettings.fps, scratchPath,
                    &encoderExtradata);
                // probeSegmentExtradata removes its file on the known failure
                // paths; a failed writer open can still leave one behind.
                QFile::remove(scratchPath);
                if (readPart && probed && partExtradata != encoderExtradata) {
                    encoderParametersChanged = true;
                    startSegment = 0;
                    freshPartsDir = true;
                }
            }
            for (int i = startSegment; i < timing.segmentCount; ++i)
                QFile::remove(segmentPath(partsDir, i));
            result.resumed = startSegment > 0;
            checkpoint.completedSegments = startSegment;
        } else {
            clearParts(partsDir);
            freshPartsDir = true;
        }
    } else {
        // A fresh export must never mix in parts from an older run.
        clearParts(partsDir);
        freshPartsDir = true;
    }
    if (encoderParametersChanged) {
        appendWarning(
            &result,
            QStringLiteral("编码器重新打开后参数集（SPS/PPS）发生变化，"
                           "已从头重新导出。"));
    }
    if (!QDir().mkpath(partsDir)) {
        result.error = QStringLiteral("无法创建导出临时目录：%1").arg(partsDir);
        return result;
    }

    // Open every encoder only after the resume decision (the parameter probe
    // needs its own instance) and before writing the checkpoint or the SRT
    // sidecar: a hardware encoder that fails to open must not leave new resume
    // state or a subtitle file from a run that never produced video.
    VideoEncoder videoEncoder;
    AudioEncoder audioEncoder;
    OfflineFrameRenderer renderer;
    QStringList warnings;
    bool srtWritten = false;
    QString srtWrittenPath;
    const auto cleanEarlyArtifacts = [&] {
        // Only remove a parts directory this run owns: a valid resume's parts
        // are the remaining work and must survive a later early failure.
        if (freshPartsDir)
            clearParts(partsDir);
        if (srtWritten && !srtWrittenPath.isEmpty())
            QFile::remove(srtWrittenPath);
    };
    error.clear();
    renderer.setRemovalCacheRoot(hooks.removalCacheRoot);
    if (!videoEncoder.open(*selected, frozenSettings, &error)
        || !audioEncoder.open(kSampleRate, kChannels, kAudioBitrateKbps, &error)
        || !renderer.open(renderProject, frozenSettings.width,
                          frozenSettings.height, &error)) {
        result.error = error;
        cleanEarlyArtifacts();
        return result;
    }
    renderer.setWarningCallback([&warnings](const QString &message) {
        if (!warnings.contains(message))
            warnings.append(message);
    });

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
            cleanEarlyArtifacts();
            return result;
        }
        srtWritten = true;
        srtWrittenPath = srtPath;
    }

    error.clear();
    if (!checkpoint.save(checkpointPath, &error)) {
        result.error = error;
        cleanEarlyArtifacts();
        return result;
    }
    // Checkpoint updates outside the critical pre-render save must not abort a
    // run that is otherwise progressing (the parts themselves are intact);
    // they surface as a warning so a broken checkpoint is not silent. Report
    // the first failure only: this runs once per completed segment.
    bool checkpointWarningReported = false;
    const auto saveCheckpoint = [&] {
        QString saveError;
        if (checkpoint.save(checkpointPath, &saveError))
            return;
        if (checkpointWarningReported)
            return;
        checkpointWarningReported = true;
        appendWarning(&result,
                      QStringLiteral("无法保存导出检查点（续跑信息可能丢失）：%1")
                          .arg(saveError));
    };

    SegmentRouter router(partsDir, timing, frozenSettings.fps,
                         videoEncoder.context());
    // A segment completed by the router (either through an explicit
    // closeThrough or the router's own bounded writer window) becomes durable
    // state immediately: a hard crash then loses at most the buffered writers,
    // never the whole video pass.
    const auto persistCompletedSegments = [&] {
        const int completed = std::min(timing.segmentCount,
                                       startSegment + router.completedSegments());
        if (completed <= checkpoint.completedSegments)
            return;
        checkpoint.completedSegments = completed;
        checkpoint.bytesWritten = router.bytesWritten();
        checkpoint.lastError.clear();
        saveCheckpoint();
    };
    TimelineMixer mixer(kSampleRate, kChannels);
    mixer.setProject(frozen);
    float peak = 0.0f;
    const int64_t startedMs = QDateTime::currentMSecsSinceEpoch();
    bool canceled = false;
    // Declared before the progress lambda because the lambda reads them.
    int64_t encodedFrames = 0;
    int64_t framesAtStart = 0;
    int64_t fedSamples = 0; // audio always starts at sample 0, even on resume
    int64_t paddedSamples = 0;
    int64_t audioBytesWritten = 0;
    if (startSegment < timing.segmentCount) {
        int64_t vBegin = 0;
        int64_t vEnd = 0;
        videoFrameRangeForSegment(timing, frozenSettings.fps, startSegment,
                                  &vBegin, &vEnd);
        encodedFrames = vBegin;
        framesAtStart = vBegin;
    } else {
        // Nothing left to render: report the already finished frame count so a
        // fully complete resume does not look like it restarted.
        encodedFrames = timing.totalFrames;
        framesAtStart = timing.totalFrames;
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
        // Speed and ETA only describe work done by this run. Counting frames
        // inherited from the checkpoint would overstate throughput after a
        // resume and make the ETA collapse to zero.
        const int64_t framesThisRun =
            std::max<int64_t>(0, encodedFrames - framesAtStart);
        progress.encodeFps = progress.elapsedMs > 0
                                 ? double(framesThisRun) * 1000.0
                                       / double(progress.elapsedMs)
                                 : 0.0;
        progress.etaMs = progress.encodeFps > 0.0
                             ? int64_t(double(timing.totalFrames
                                              - progress.completedFrames)
                                       * 1000.0 / progress.encodeFps)
                             : -1;
        progress.outputBytes = router.bytesWritten() + audioBytesWritten;
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
        audioTrack.finish(nullptr);
        QFile::remove(audioPartPath);
        cleanEarlyArtifacts();
        return result;
    }
    const auto writeAudioPackets =
        [&](const std::vector<EncodedPacket> &packets) -> bool {
        for (const EncodedPacket &packet : packets) {
            if (!audioTrack.write(packet, &error)) {
                result.error = QStringLiteral("音频写入失败：%1").arg(error);
                return false;
            }
            audioBytesWritten += int64_t(packet.data.size());
        }
        return true;
    };
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
                if (!writeAudioPackets(packets))
                    break;
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
            } else if (writeAudioPackets(packets)) {
                fedSamples += kAacFrameSamples;
            }
        }
        std::vector<EncodedPacket> audioTail;
        if (result.error.isEmpty() && !audioEncoder.flush(&audioTail, &error))
            result.error = error;
        if (result.error.isEmpty())
            writeAudioPackets(audioTail);
        if (result.error.isEmpty() && !audioTrack.finish(&error))
            result.error = error;
    }
    if (canceled || !result.error.isEmpty()) {
        audioTrack.finish(nullptr);
        QFile::remove(audioPartPath);
        checkpoint.lastError = result.error;
        saveCheckpoint();
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
            std::max<qint64>(kExportDiskFloorBytes,
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
                // A removal-cache failure is reported verbatim: callers (and
                // the UI) key their "regenerate then export" flow on this
                // exact text, and it must never be mistaken for a transient
                // decode error that could fall back to the original.
                result.error = error.contains(QStringLiteral("AI 修复缓存"))
                                   ? error
                                   : QStringLiteral("第 %1 帧渲染失败：%2")
                                         .arg(frame)
                                         .arg(error);
                result.resumable = checkpoint.completedSegments > 0;
                result.audioPeak = peak;
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
        // Also covers parts the router closed on its own while routing packets
        // (it keeps at most four writers open).
        persistCompletedSegments();
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
    // `finishAll(false)` returning true only means the cleanup succeeded, so
    // the "complete" claim needs both a clean run and a successful close.
    const bool successSoFar = !canceled && result.error.isEmpty();
    const bool finishedAll = router.finishAll(successSoFar, &error);
    if (!finishedAll && result.error.isEmpty())
        result.error = error;
    checkpoint.completedSegments =
        (successSoFar && finishedAll)
            ? timing.segmentCount
            : std::min(timing.segmentCount,
                       startSegment + router.completedSegments());
    checkpoint.bytesWritten = router.bytesWritten();
    checkpoint.lastError = result.error;
    saveCheckpoint();

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

    // Finalizing/Validating deliberately do not poll the cancel hook: the
    // output is all but written and abandoning it there would only leave a
    // temp file for the next run to clean up.
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
    // Move an existing output aside instead of deleting it first: if the final
    // rename fails (a lock, a full disk, an antivirus scan) the previous
    // export is restored rather than lost.
    QString previousOutput;
    if (QFileInfo::exists(result.outputPath)) {
        previousOutput = result.outputPath + QStringLiteral(".previous");
        QFile::remove(previousOutput);
        if (!QFile::rename(result.outputPath, previousOutput)) {
            result.error = QStringLiteral("无法替换已存在的导出文件：%1")
                               .arg(result.outputPath);
            // The assembled temp and every part are still intact; a retry can
            // finish without re-rendering.
            result.resumable = true;
            result.audioPeak = peak;
            return result;
        }
    }
    if (!QFile::rename(temp, result.outputPath)) {
        if (!previousOutput.isEmpty())
            QFile::rename(previousOutput, result.outputPath);
        result.error =
            QStringLiteral("无法写入最终导出文件：%1").arg(result.outputPath);
        result.resumable = true;
        result.audioPeak = peak;
        return result;
    }
    if (!previousOutput.isEmpty())
        QFile::remove(previousOutput);
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
