// src/ai/AiEngineMain.cpp
//
// Local inference subprocess. It reads only the AiJobSpec, talks to the parent
// over a local socket and never opens the project file or the network.
//
// With ONNX Runtime and OpenCV available (VIDEOEDITOR_HAVE_ORT=1) it runs the
// Task 5 tracking pipeline (EfficientTAM keyframes + DIS propagation). The
// repair pipeline belongs to Task 6: a non-tracking-only job reports
// inpaint_unavailable after tracking. Without the runtimes this file keeps the
// Task 4 stub behaviour so the protocol tests still have a real executable.

#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMap>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <functional>
#include <limits>

#include "AiProtocol.h"

#if VIDEOEDITOR_HAVE_ORT
#include "ModelRegistry.h"
#include "engine/FrameReader.h"
#include "engine/InpaintPipeline.h"
#include "engine/LamaInpainter.h"
#include "engine/MiGanInpainter.h"
#include "engine/Sam2Tracker.h"
#include "engine/TemporalStabilizer.h"
#include "engine/TrackingPipeline.h"
#include "RemovalCache.h"
#endif

namespace {

using namespace VideoEditor;

// "SendMessage" is a Windows macro, so the sink alias deliberately avoids it.
using MessageSink = std::function<void(const QJsonObject &)>;

#if VIDEOEDITOR_HAVE_ORT

// Reads newline-delimited commands from the parent without installing an event
// loop, so the pipeline can poll once per frame.
class CommandPoller {
public:
    explicit CommandPoller(QLocalSocket &socket)
        : m_socket(socket)
    {
    }

    // Returns true when a cancel arrived. Pause/resume update `paused` in
    // arrival order, so a pause+resume batch ends unpaused. Cancel always
    // wins. Reading without an event loop needs an explicit wait: on Windows a
    // named pipe may report 0 bytesAvailable() until the read is attempted.
    bool poll(bool &paused)
    {
        if (m_socket.state() != QLocalSocket::ConnectedState)
            return true;
        if (m_socket.bytesAvailable() == 0)
            m_socket.waitForReadyRead(1);
        if (m_socket.bytesAvailable() > 0)
            m_buffer.append(m_socket.readAll());
        if (m_buffer.size() > AiProtocol::kMaxLineBytes) {
            m_buffer.clear();
            return true;
        }

        bool cancel = false;
        int newline = -1;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            QJsonObject message;
            if (!AiProtocol::parseLine(line, &message, nullptr))
                continue;
            const QString cmd = message.value(QStringLiteral("cmd")).toString();
            if (cmd == QLatin1String("cancel"))
                cancel = true;
            else if (cmd == QLatin1String("pause"))
                paused = true;
            else if (cmd == QLatin1String("resume"))
                paused = false;
        }
        return cancel;
    }

private:
    QLocalSocket &m_socket;
    QByteArray m_buffer;
};

constexpr int kRepairChunkFrames = 30;
constexpr int kTrackingPercent = 20;
constexpr int kRepairPercentSpan = 80;

bool maskHasPixels(const BinaryMask &mask)
{
    return std::any_of(mask.bits.begin(), mask.bits.end(),
                       [](uint8_t value) { return value != 0; });
}

QString maskFilePath(const AiJobSpec &spec, int frame)
{
    return QDir(spec.cacheDir)
        .filePath(QStringLiteral("tracking/masks/frame_%1.rle")
                      .arg(frame, 6, 10, QLatin1Char('0')));
}

std::optional<BinaryMask> readFrameMask(const AiJobSpec &spec, int frame,
                                        QString *error)
{
    QFile file(maskFilePath(spec, frame));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取跟踪遮罩：%1")
                         .arg(file.fileName());
        return std::nullopt;
    }
    auto mask = MaskCodec::decode(file.readAll(), error);
    if (mask && (mask->width != spec.width || mask->height != spec.height)) {
        if (error)
            *error = QStringLiteral("跟踪遮罩尺寸与任务显示尺寸不一致。");
        return std::nullopt;
    }
    return mask;
}

// Feather of the authored keyframe nearest to the chunk start, constant for
// the whole chunk so alpha never jumps between frames.
int featherForChunk(const AiJobSpec &spec, int firstFrame)
{
    int best = 4;
    int bestDistance = std::numeric_limits<int>::max();
    for (const MaskKeyframe &keyframe : spec.maskKeyframes) {
        const int frame = int(microsToFrames(
            keyframe.sourceTimeUs - spec.sourceInUs, spec.fps));
        const int distance = std::abs(frame - firstFrame);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = std::clamp(keyframe.featherPx, 0, 64);
        }
    }
    return best;
}

QString firstOnnxPath(const ModelRegistry &registry,
                      const AiModelRecord &record)
{
    for (auto it = record.files.constBegin(); it != record.files.constEnd();
         ++it) {
        if (it.value().path.endsWith(QLatin1String(".onnx"),
                                     Qt::CaseInsensitive)) {
            return QDir(registry.rootDir()).filePath(it.value().path);
        }
    }
    return {};
}

enum class InpaintAdapter { MiGan, Lama };

// The adapter is a property of the model record, never of the requested mode;
// an unknown record must fail instead of being sent to the uint8 MI-GAN graph.
std::optional<InpaintAdapter> inpaintAdapterFor(const AiModelRecord &record)
{
    const QString haystack = record.id + QLatin1Char(' ') + record.version;
    const bool mentionsMigan =
        haystack.contains(QLatin1String("migan"), Qt::CaseInsensitive);
    const bool mentionsLama =
        haystack.contains(QLatin1String("lama"), Qt::CaseInsensitive);
    if (mentionsMigan && !mentionsLama)
        return InpaintAdapter::MiGan;
    if (mentionsLama && !mentionsMigan)
        return InpaintAdapter::Lama;
    // Explicit manifest io hints: MI-GAN is uint8, LaMa takes 4 channels.
    const QString inputDtype =
        record.io.value(QStringLiteral("inputDtype")).toString();
    if (inputDtype.compare(QLatin1String("uint8"), Qt::CaseInsensitive) == 0)
        return InpaintAdapter::MiGan;
    if (record.io.value(QStringLiteral("inputChannels")).toInt() == 4)
        return InpaintAdapter::Lama;
    return std::nullopt;
}

const AiModelRecord *resolveInpaintRecord(const ModelRegistry &registry,
                                          const AiJobSpec &spec,
                                          QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };

    // An explicit id was part of the cache key: never silently substitute a
    // different model when it is unknown or has the wrong role.
    if (!spec.inpaintModelId.isEmpty()) {
        const AiModelRecord *requested = registry.model(spec.inpaintModelId);
        if (!requested) {
            return fail(QStringLiteral("修复模型不存在：%1。")
                            .arg(spec.inpaintModelId));
        }
        if (!requested->isInpaint()) {
            return fail(QStringLiteral("修复模型角色无效：%1。")
                            .arg(spec.inpaintModelId));
        }
        return requested;
    }

    // Measured defaults: MI-GAN for Fast, int8 allowzero LaMa for Quality.
    // The registry resolves both the exact ids and a strict mode-appropriate
    // fallback so Quality can never silently run MI-GAN or an fp16 LaMa.
    const AiModelRecord *record =
        registry.preferredInpaintModel(spec.parameters.mode);
    if (!record || !record->isInpaint()) {
        return fail(QStringLiteral("没有可用的修复模型。"));
    }
    return record;
}

// Shared pause/cancel poll for tracking and repair. Returns true on cancel.
bool pollCommands(CommandPoller &poller, bool &paused)
{
    if (poller.poll(paused))
        return true;
    while (paused) {
        if (poller.poll(paused))
            return true;
        if (!paused)
            break;
        QThread::msleep(20);
    }
    return false;
}

void sendRepairProgress(const MessageSink &send, int frame, int totalFrames,
                        int completedFrames, QElapsedTimer &clock)
{
    const double fraction = totalFrames > 0
        ? std::clamp(double(completedFrames) / double(totalFrames), 0.0, 1.0)
        : 0.0;
    qint64 etaMs = 0;
    if (completedFrames > 0) {
        const double perFrame =
            double(clock.elapsed()) / double(completedFrames);
        etaMs = qint64(perFrame * double(totalFrames - completedFrames));
    }
    send(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("progress")},
        {QStringLiteral("stage"), QStringLiteral("repair")},
        {QStringLiteral("frame"), frame},
        {QStringLiteral("totalFrames"), totalFrames},
        {QStringLiteral("percent"),
         double(kTrackingPercent) + double(kRepairPercentSpan) * fraction},
        {QStringLiteral("etaMs"), static_cast<double>(etaMs)},
    });
}

BinaryMask cropBinaryMask(const BinaryMask &mask, const QRect &crop)
{
    BinaryMask result;
    result.width = crop.width();
    result.height = crop.height();
    result.bits.assign(size_t(crop.width()) * size_t(crop.height()), 0);
    for (int y = 0; y < crop.height(); ++y)
        for (int x = 0; x < crop.width(); ++x)
            if (mask.at(crop.x() + x, crop.y() + y))
                result.set(x, y, true);
    return result;
}

int runRealEngine(const AiJobSpec &spec, QLocalSocket &socket,
                  const MessageSink &send)
{
    const bool technical =
        qEnvironmentVariable("VIDEOEDITOR_M4_TECHNICAL") == QLatin1String("1");
    if (!technical) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("model_unavailable")},
            {QStringLiteral("message"), QStringLiteral("AI 模型尚未安装。")}});
        send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                         {QStringLiteral("cacheKey"), spec.cacheKey},
                         {QStringLiteral("completedFrames"), 0}});
        return 3;
    }

    const auto modelUnavailable = [&](const QString &message) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("model_unavailable")},
            {QStringLiteral("message"), message}});
        send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                         {QStringLiteral("cacheKey"), spec.cacheKey},
                         {QStringLiteral("completedFrames"), 0}});
        return 3;
    };

    QString error;
    const auto registry = ModelRegistry::load(spec.modelsManifestPath, &error);
    if (!registry) {
        return modelUnavailable(
            QStringLiteral("无法加载模型清单：%1").arg(error));
    }
    const auto offlineCapable = [](const AiModelRecord &model) {
        return model.files.contains(QStringLiteral("image_encoder"))
            && model.files.contains(QStringLiteral("prompt_encoder"))
            && model.files.contains(QStringLiteral("mask_decoder"));
    };

    const AiModelRecord *record = registry->model(spec.trackingModelId);
    if (!record || !record->isTracking()) {
        return modelUnavailable(QStringLiteral("跟踪模型不可用：%1。")
                                    .arg(spec.trackingModelId));
    }
    if (!offlineCapable(*record)) {
        // The manifest's first/default tracker is the SAM 2 bundle, which has
        // no prompt_encoder. Fall back to the offline-capable default (fp32
        // EfficientTAM) with a warning instead of failing the job.
        const AiModelRecord *fallback = registry->defaultTrackingModel();
        if (!fallback || fallback == record || !offlineCapable(*fallback)) {
            return modelUnavailable(
                QStringLiteral("跟踪模型 %1 缺少离线关键帧组件。")
                    .arg(record->id));
        }
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("warning")},
            {QStringLiteral("message"),
             QStringLiteral("请求的跟踪模型 %1 缺少离线关键帧组件，已改用 %2。")
                 .arg(record->id, fallback->id)}});
        record = fallback;
    }

    // Repair jobs need a resolved inpaint record; tracking-only jobs do not.
    const AiModelRecord *inpaintRecord = nullptr;
    std::optional<InpaintAdapter> inpaintAdapter;
    if (!spec.trackingOnly) {
        inpaintRecord = resolveInpaintRecord(*registry, spec, &error);
        if (!inpaintRecord) {
            return modelUnavailable(error.isEmpty()
                                        ? QStringLiteral("修复模型不可用。")
                                        : error);
        }
        inpaintAdapter = inpaintAdapterFor(*inpaintRecord);
        if (!inpaintAdapter) {
            return modelUnavailable(
                QStringLiteral("无法识别修复模型 %1 的适配器。")
                    .arg(inpaintRecord->id));
        }
    }

    // Hash only the records the engine will actually load.
    QStringList verifyIds{record->id};
    if (inpaintRecord)
        verifyIds.append(inpaintRecord->id);
    if (!registry->verifyFiles(verifyIds, &error)) {
        return modelUnavailable(
            QStringLiteral("模型文件校验失败：%1").arg(error));
    }

    CommandPoller poller(socket);
    bool paused = false;

    // A redo job reuses masks written by an earlier tracking run. Tracking-only
    // jobs always track, even when a redo range is present.
    const bool redo = !spec.trackingOnly && spec.redoFirstFrame >= 0;
    TrackingRunResult trackingResult;
    if (!redo) {
        TrackingConfig config;
        config.io = record->io;
        config.allowDml = false;
        config.allowCpu = true;
        for (auto it = record->files.constBegin();
             it != record->files.constEnd(); ++it) {
            if (!it.value().path.endsWith(QLatin1String(".onnx"),
                                          Qt::CaseInsensitive)) {
                continue;
            }
            config.partPaths.insert(
                it.key(), QDir(registry->rootDir()).filePath(it.value().path));
        }
        for (const QString &part :
             {QStringLiteral("image_encoder"),
              QStringLiteral("prompt_encoder"),
              QStringLiteral("mask_decoder")}) {
            if (!config.partPaths.contains(part)) {
                return modelUnavailable(
                    QStringLiteral("跟踪模型缺少组件：%1。").arg(part));
            }
        }

        Sam2Tracker tracker;
        if (!tracker.initialize(config, &error)) {
            return modelUnavailable(
                QStringLiteral("无法加载跟踪模型：%1").arg(error));
        }

        TrackingPipeline trackingPipeline;
        trackingPipeline.setTracker(&tracker);
        TrackingRunCallbacks callbacks;
        callbacks.event = [&send](QJsonObject message) {
            if (message.value(QStringLiteral("event")).toString()
                == QLatin1String("progress")) {
                const double percent =
                    message.value(QStringLiteral("percent")).toDouble();
                message.insert(QStringLiteral("percent"),
                               percent * double(kTrackingPercent) / 100.0);
            }
            send(message);
        };
        callbacks.cancel = [&]() { return pollCommands(poller, paused); };

        trackingResult = trackingPipeline.run(spec, callbacks);
        if (trackingResult.cancelled) {
            send(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("cancelled")},
                {QStringLiteral("completedFrames"),
                 trackingResult.completedFrames},
                {QStringLiteral("confidenceHash"),
                 trackingResult.confidenceHash},
                {QStringLiteral("trackingState"),
                 removalTrackingStateToString(trackingResult.state)}});
            return 0;
        }
        if (!trackingResult.ok) {
            send(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("error")},
                {QStringLiteral("code"), QStringLiteral("tracking_failed")},
                {QStringLiteral("message"), trackingResult.error}});
            return 4;
        }
    }

    if (spec.trackingOnly) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("done")},
            {QStringLiteral("cacheKey"), spec.cacheKey},
            {QStringLiteral("confidenceHash"), trackingResult.confidenceHash},
            {QStringLiteral("trackingState"),
             removalTrackingStateToString(trackingResult.state)},
            {QStringLiteral("completedFrames"),
             trackingResult.completedFrames}});
        return 0;
    }

    const auto repairFailed = [&](const QString &message) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("error")},
            {QStringLiteral("code"), QStringLiteral("inpaint_failed")},
            {QStringLiteral("message"), message}});
        return 5;
    };

    // spec.cacheDir is "<root>/<cacheKey>"; the engine writes chunks and the
    // manifest through RemovalCache so hashes match the parent exactly.
    const QString cleanCacheDir =
        QDir::cleanPath(QDir(spec.cacheDir).absolutePath());
    const QFileInfo cacheInfo(cleanCacheDir);
    if (cacheInfo.fileName() != spec.cacheKey) {
        return repairFailed(
            QStringLiteral("修复缓存目录必须以缓存键命名：%1。")
                .arg(spec.cacheDir));
    }
    RemovalCache cache(cacheInfo.absolutePath());

    FrameReader reader;
    if (!reader.open(spec.assetPath, spec.fps, spec.sourceInUs,
                     spec.sourceOutUs, QSize(spec.width, spec.height),
                     &error)) {
        return repairFailed(
            QStringLiteral("无法读取修复源帧：%1").arg(error));
    }
    const int64_t requestedFrames =
        microsToFrames(spec.sourceOutUs - spec.sourceInUs, spec.fps);
    int totalFrames =
        int(std::min<int64_t>(requestedFrames, reader.frameCount()));
    if (totalFrames <= 0)
        return repairFailed(QStringLiteral("修复帧数为零。"));

    std::unique_ptr<Inpainter> inpainter;
    const QString inpaintPath = firstOnnxPath(*registry, *inpaintRecord);
    if (inpaintPath.isEmpty())
        return modelUnavailable(QStringLiteral("修复模型缺少 ONNX 文件。"));
    if (*inpaintAdapter == InpaintAdapter::Lama) {
        inpainter = std::make_unique<LamaInpainter>(inpaintPath, 0);
    } else {
        inpainter = std::make_unique<MiGanInpainter>(
            inpaintPath, 0, /*allowDml=*/true, /*allowCpu=*/true);
    }
    if (!inpainter->isReady()) {
        return modelUnavailable(
            QStringLiteral("无法加载修复模型：%1").arg(inpainter->lastError()));
    }

    InpaintPipeline pipeline;
    pipeline.setInpainter(std::move(inpainter));

    std::optional<RemovalCacheManifest> existing;
    if (spec.resume || redo)
        existing = cache.readManifest(spec.cacheKey, nullptr);

    QMap<int, RemovalCacheChunk> manifestChunks;
    if (existing) {
        for (const RemovalCacheChunk &chunk : existing->chunks)
            manifestChunks.insert(chunk.index, chunk);
    }

    const int chunkCount = (totalFrames + kRepairChunkFrames - 1)
        / kRepairChunkFrames;
    QSet<int> repairChunks;
    if (redo) {
        const int firstChunk =
            std::clamp(spec.redoFirstFrame / kRepairChunkFrames, 0,
                       chunkCount - 1);
        const int lastChunk =
            std::clamp(spec.redoLastFrame / kRepairChunkFrames, 0,
                       chunkCount - 1);
        for (int chunk = firstChunk; chunk <= lastChunk; ++chunk)
            repairChunks.insert(chunk);
    } else {
        for (int chunk = 0; chunk < chunkCount; ++chunk)
            repairChunks.insert(chunk);
    }

    QElapsedTimer repairClock;
    repairClock.start();
    int processedFrames = 0;
    bool cancelled = false;

    for (int chunk = 0; chunk < chunkCount; ++chunk) {
        if (!repairChunks.contains(chunk))
            continue;

        const int firstFrame = chunk * kRepairChunkFrames;
        const int framesInChunk =
            std::min(kRepairChunkFrames, totalFrames - firstFrame);

        if (pollCommands(poller, paused)) {
            cancelled = true;
            break;
        }

        // Resume skips chunks the manifest already records as complete and
        // whose directory is still present. A redo range always rewrites.
        const QString chunkIndexPath =
            QDir(cache.cacheDir(spec.cacheKey))
                .filePath(QStringLiteral("chunks/%1/index.json").arg(chunk));
        if (spec.resume && !redo && manifestChunks.contains(chunk)
            && QFileInfo::exists(chunkIndexPath)) {
            processedFrames += framesInChunk;
            sendRepairProgress(send, firstFrame + framesInChunk - 1,
                               totalFrames, processedFrames, repairClock);
            continue;
        }

        // Memory bound: one chunk holds up to 30 RGB32 frames (30 * W * H * 4
        // bytes) plus masks. At 1080p that is ~250 MB; at 4K it approaches
        // ~1 GB. The offline scope is 1080p; a 4K streaming path is Task 9.
        QList<QImage> frames;
        QList<BinaryMask> masks;
        frames.reserve(framesInChunk);
        masks.reserve(framesInChunk);
        for (int i = 0; i < framesInChunk; ++i) {
            const int globalFrame = firstFrame + i;
            QString frameError;
            QImage frame = reader.readFrame(globalFrame, &frameError);
            if (frame.isNull()) {
                return repairFailed(frameError.isEmpty()
                                        ? QStringLiteral("读取修复帧失败。")
                                        : frameError);
            }
            auto mask = readFrameMask(spec, globalFrame, &frameError);
            if (!mask) {
                return repairFailed(frameError.isEmpty()
                                        ? QStringLiteral("读取跟踪遮罩失败。")
                                        : frameError);
            }
            frames.append(frame);
            masks.append(*mask);
        }

        const int featherPx = featherForChunk(spec, firstFrame);
        QString repairError;
        auto patches = pipeline.repairChunk(
            frames, masks, spec.parameters, featherPx,
            [&](int index, const QString &) {
                if (pollCommands(poller, paused))
                    return false;
                sendRepairProgress(send, firstFrame + index, totalFrames,
                                   processedFrames + index, repairClock);
                return true;
            },
            &repairError);
        if (!patches) {
            if (pipeline.wasCancelled()) {
                cancelled = true;
                break;
            }
            return repairFailed(repairError.isEmpty()
                                    ? QStringLiteral("修复推理失败。")
                                    : repairError);
        }

        // Only frames with a non-empty mask produce a cache entry. The rest
        // are transparent patches returned by repairChunk and filtered here.
        QList<int> patchFrames;
        QList<int> patchIndices;
        QList<QImage> patchImages;
        QList<QRect> crops;
        QList<QPoint> centroids;
        for (int i = 0; i < framesInChunk; ++i) {
            if (!maskHasPixels(masks.at(i)))
                continue;
            patchFrames.append(firstFrame + i);
            patchIndices.append(i);
            patchImages.append(patches->at(i).rgba);
            crops.append(patches->at(i).crop);
            centroids.append(MaskEditor::contentBounds(masks.at(i)).center());
        }

        if (!patchImages.isEmpty()) {
            if (spec.parameters.temporalStabilization
                && patchImages.size() > 1) {
                TemporalStabilizer::stabilize(
                    patchImages, crops, centroids,
                    spec.parameters.stabilizationStrength);
            }
            // Second colour match after stabilization so the median blend
            // cannot introduce a global tint.
            for (int i = 0; i < patchImages.size(); ++i) {
                const QImage sourceCrop =
                    frames.at(patchIndices.at(i)).copy(crops.at(i));
                const BinaryMask maskCrop =
                    cropBinaryMask(masks.at(patchIndices.at(i)), crops.at(i));
                InpaintPipeline::colorMatch(sourceCrop, &patchImages[i],
                                            maskCrop);
            }
        }

        // A chunk with no holes is still written (with an empty frame list) so
        // every chunk index is present and the manifest can prove completeness.
        RemovalChunkIndex written;
        if (!cache.writeChunkFrames(spec.cacheKey, chunk, patchFrames,
                                    patchImages, crops, &written, &error)) {
            return repairFailed(error);
        }
        manifestChunks.insert(
            chunk, RemovalCacheChunk{chunk, firstFrame, framesInChunk,
                                     written.treeSha256});

        processedFrames += framesInChunk;
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("chunkDone")},
            {QStringLiteral("stage"), QStringLiteral("repair")},
            {QStringLiteral("chunk"), chunk},
            {QStringLiteral("frame"), firstFrame + framesInChunk - 1},
            {QStringLiteral("completedFrames"), processedFrames}});
        sendRepairProgress(send, firstFrame + framesInChunk - 1, totalFrames,
                           processedFrames, repairClock);
    }

    RemovalCacheManifest manifest;
    manifest.schema = QStringLiteral("m4.removal.cache.1");
    manifest.cacheKey = spec.cacheKey;
    manifest.clipId = spec.clipId;
    manifest.sourceInUs = spec.sourceInUs;
    manifest.sourceOutUs = spec.sourceOutUs;
    manifest.width = spec.width;
    manifest.height = spec.height;
    manifest.fps = spec.fps;
    manifest.trackingModelId = record->id;
    manifest.inpaintModelId = inpaintRecord->id;
    manifest.pipelineVersion = QStringLiteral("m4.0");
    // "complete" is only safe when every chunk index is listed and its
    // directory/index.json is present; a redo after a partial run must not
    // promote a manifest that still has missing chunks.
    bool allChunksComplete = !cancelled;
    for (int chunk = 0; chunk < chunkCount && allChunksComplete; ++chunk) {
        const auto it = manifestChunks.constFind(chunk);
        if (it == manifestChunks.constEnd()
            || it.value().treeSha256.isEmpty()) {
            allChunksComplete = false;
            break;
        }
        const QString indexPath =
            QDir(cache.cacheDir(spec.cacheKey))
                .filePath(QStringLiteral("chunks/%1/index.json").arg(chunk));
        if (!QFileInfo::exists(indexPath))
            allChunksComplete = false;
    }
    manifest.status = allChunksComplete ? QStringLiteral("complete")
                                        : QStringLiteral("partial");
    manifest.totalFrames = totalFrames;
    manifest.createdAt =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (auto it = manifestChunks.constBegin();
         it != manifestChunks.constEnd(); ++it) {
        manifest.chunks.append(it.value());
    }
    if (!cache.writeManifest(manifest, &error)) {
        return repairFailed(
            QStringLiteral("无法写入修复缓存清单：%1").arg(error));
    }

    RemovalTrackingState state = trackingResult.state;
    if (redo) {
        state = (existing && existing->status == QLatin1String("partial"))
            ? RemovalTrackingState::Partial
            : RemovalTrackingState::Complete;
    }

    if (cancelled) {
        send(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("cancelled")},
            {QStringLiteral("completedFrames"), processedFrames},
            {QStringLiteral("confidenceHash"),
             trackingResult.confidenceHash},
            {QStringLiteral("trackingState"),
             removalTrackingStateToString(state)}});
        return 0;
    }

    send(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("done")},
        {QStringLiteral("cacheKey"), spec.cacheKey},
        {QStringLiteral("confidenceHash"), trackingResult.confidenceHash},
        {QStringLiteral("trackingState"),
         removalTrackingStateToString(state)},
        {QStringLiteral("completedFrames"), totalFrames}});
    return 0;
}

#endif // VIDEOEDITOR_HAVE_ORT

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("job"), QStringLiteral("job spec"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("socket"), QStringLiteral("local socket"),
                      QStringLiteral("name")});
    parser.process(app);

    const QString jobPath = parser.value(QStringLiteral("job"));
    const QString socketName = parser.value(QStringLiteral("socket"));
    QFile jobFile(jobPath);
    if (!jobFile.open(QIODevice::ReadOnly))
        return 2;
    QString error;
    const auto spec = VideoEditor::aiJobSpecFromJson(
        QJsonDocument::fromJson(jobFile.readAll()).object(), &error);
    if (!spec)
        return 2;
    if (!VideoEditor::AiProtocol::validateJobSpec(*spec, &error))
        return 2;

    QLocalSocket socket;
    socket.connectToServer(socketName);
    if (!socket.waitForConnected(5000))
        return 3;

    const auto send = [&socket](const QJsonObject &message) {
        socket.write(VideoEditor::AiProtocol::encodeLine(message));
        socket.flush();
    };

    int exitCode = 3;
#if VIDEOEDITOR_HAVE_ORT
    exitCode = runRealEngine(*spec, socket, send);
#else
    // Task 4 stub: the real ONNX runtime is not available in this build.
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("error")},
                     {QStringLiteral("code"),
                      QStringLiteral("model_unavailable")},
                     {QStringLiteral("message"),
                      QStringLiteral("AI 模型尚未安装。")}});
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                     {QStringLiteral("cacheKey"), spec->cacheKey},
                     {QStringLiteral("completedFrames"), 0}});
    exitCode = 3;
#endif

    // flush() only queues the bytes; the process must not exit (closing the
    // pipe) before the parent has actually received them.
    socket.waitForBytesWritten(5000);
    socket.disconnectFromServer();
    return exitCode;
}
