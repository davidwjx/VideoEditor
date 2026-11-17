#include "LiveRepairService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include "FrameReader.h"
#include "InpaintPipeline.h"
#include "MaskCodec.h"

namespace VideoEditor {

namespace {

QString maskPathFor(const QString &cacheDir, int frame)
{
    return QDir(QDir(cacheDir).filePath(QStringLiteral("tracking/masks")))
        .filePath(QStringLiteral("frame_%1.rle")
                      .arg(frame, 6, 10, QLatin1Char('0')));
}

std::optional<BinaryMask> readMaskFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开跟踪遮罩文件：%1。").arg(path);
        return std::nullopt;
    }
    return MaskCodec::decode(file.readAll(), error);
}

} // namespace

LiveRepairService::LiveRepairService(QObject *parent)
    : QObject(parent)
{
}

LiveRepairService::~LiveRepairService()
{
    stop();
}

void LiveRepairService::clearWorkingState()
{
    m_reader.reset();
    m_pipeline.reset();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_spec = LiveRepairSpec{};
        m_frameCount = 0;
        m_patches.clear();
        m_lru.clear();
        m_emptyFrames.clear();
    }
    m_configured = false;
}

bool LiveRepairService::configure(const LiveRepairSpec &spec, QString *error)
{
    stop();
    if (error)
        error->clear();

    const auto fail = [this, error](const QString &message) {
        clearWorkingState();
        if (error)
            *error = message;
        return false;
    };

    if (spec.assetPath.isEmpty())
        return fail(QStringLiteral("实时预览缺少素材路径。"));
    if (!QFileInfo::exists(spec.assetPath))
        return fail(QStringLiteral("实时预览素材不存在：%1。").arg(spec.assetPath));
    if (spec.cacheDir.isEmpty())
        return fail(QStringLiteral("实时预览缺少修复缓存目录。"));
    if (spec.layerId.isEmpty())
        return fail(QStringLiteral("实时预览缺少消除层。"));
    if (spec.sourceOutUs <= spec.sourceInUs)
        return fail(QStringLiteral("消除层的源区间无效。"));
    if (!spec.fps.isValid())
        return fail(QStringLiteral("实时预览帧率无效。"));
    if (spec.displaySize.width() <= 0 || spec.displaySize.height() <= 0)
        return fail(QStringLiteral("实时预览显示尺寸无效。"));

    // Tracking must have run before live preview: the repair stage edits the
    // per-frame hole masks, and a missing directory would only fail later on
    // the worker thread with a much less obvious message.
    const QString maskDirPath =
        QDir(spec.cacheDir).filePath(QStringLiteral("tracking/masks"));
    const QDir maskDir(maskDirPath);
    if (!maskDir.exists())
        return fail(QStringLiteral("请先运行 AI 跟踪以生成逐帧遮罩。"));
    const QFileInfoList maskFiles = maskDir.entryInfoList(
        {QStringLiteral("frame_*.rle")}, QDir::Files, QDir::Name);
    if (maskFiles.isEmpty())
        return fail(QStringLiteral("请先运行 AI 跟踪以生成逐帧遮罩。"));

    // The masks are authored at the display geometry the cache was generated
    // for; an asset change that alters rotation/scale must not silently crop.
    QString maskError;
    const auto firstMask = readMaskFile(maskFiles.first().absoluteFilePath(),
                                        &maskError);
    if (!firstMask)
        return fail(maskError.isEmpty()
                        ? QStringLiteral("无法读取跟踪遮罩。")
                        : maskError);
    if (firstMask->width != spec.displaySize.width()
        || firstMask->height != spec.displaySize.height()) {
        return fail(QStringLiteral("跟踪遮罩尺寸（%1x%2）与预览尺寸（%3x%4）"
                                   "不一致。")
                        .arg(firstMask->width)
                        .arg(firstMask->height)
                        .arg(spec.displaySize.width())
                        .arg(spec.displaySize.height()));
    }

    std::unique_ptr<Inpainter> injected;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        injected = std::move(m_testInpainter);
    }
    if (!injected) {
        if (spec.inpainterModelPath.isEmpty())
            return fail(QStringLiteral("未配置实时预览修复模型。"));
        if (!QFileInfo::exists(spec.inpainterModelPath))
            return fail(QStringLiteral("实时预览修复模型不存在：%1。")
                            .arg(spec.inpainterModelPath));
    }

    auto reader = std::make_unique<FrameReader>();
    QString openError;
    if (!reader->open(spec.assetPath, spec.fps, spec.sourceInUs,
                      spec.sourceOutUs, spec.displaySize, &openError)) {
        return fail(openError.isEmpty()
                        ? QStringLiteral("无法打开实时预览视频。")
                        : QStringLiteral("无法打开实时预览视频：%1")
                              .arg(openError));
    }
    if (reader->frameCount() <= 0)
        return fail(QStringLiteral("实时预览帧数为零。"));

    auto pipeline = std::make_unique<InpaintPipeline>();
    if (injected) {
        pipeline->setInpainterForTest(std::move(injected));
    } else {
        InpaintConfig config;
        config.deviceIndex = 0;
        config.allowDml = true;
        config.allowCpu = true;
        if (spec.mode == RepairMode::Fast)
            config.miganPath = spec.inpainterModelPath;
        else
            config.lamaPath = spec.inpainterModelPath;
        pipeline->setConfig(config);
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_spec = spec;
        m_frameCount = reader->frameCount();
        m_patches.clear();
        m_lru.clear();
        m_emptyFrames.clear();
    }
    m_reader = std::move(reader);
    m_pipeline = std::move(pipeline);
    m_targetUs.store(spec.sourceInUs);
    m_targetSerial.fetch_add(1);
    m_configured = true;
    return true;
}

void LiveRepairService::start()
{
    if (!m_configured || !m_reader || !m_pipeline)
        return;
    if (m_thread) {
        if (m_thread->isRunning())
            return;
        delete m_thread;
        m_thread = nullptr;
    }
    m_stop.store(false);
    m_thread = QThread::create([this] { run(); });
    m_thread->setObjectName(QStringLiteral("LiveRepair"));
    m_thread->start();
    emit ready();
}

void LiveRepairService::stop()
{
    m_stop.store(true);
    m_targetSerial.fetch_add(1);
    m_waitCv.notify_all();

    QThread *thread = m_thread;
    m_thread = nullptr;
    if (thread) {
        // run() never calls stop() itself; joining from the UI thread cannot
        // deadlock on the worker.
        thread->wait();
        delete thread;
    }
    clearWorkingState();
}

bool LiveRepairService::isRunning() const
{
    return m_thread && m_thread->isRunning();
}

void LiveRepairService::setTarget(int64_t sourceUs)
{
    m_targetUs.store(sourceUs);
    m_targetSerial.fetch_add(1);
    m_waitCv.notify_all();
}

int LiveRepairService::frameForSourceUs(int64_t sourceUs) const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_frameCount <= 0 || !m_spec.fps.isValid())
        return -1;
    const double index =
        std::floor(double(sourceUs - m_spec.sourceInUs) * double(m_spec.fps.num)
                   / (1'000'000.0 * double(m_spec.fps.den)));
    return std::clamp(int(index), 0, m_frameCount - 1);
}

bool LiveRepairService::patchFor(const QString &layerId, int64_t sourceUs,
                                 LiveRepairPatch *patch) const
{
    if (!patch)
        return false;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (layerId != m_spec.layerId || m_frameCount <= 0
        || sourceUs < m_spec.sourceInUs || sourceUs >= m_spec.sourceOutUs) {
        return false;
    }
    const double index =
        std::floor(double(sourceUs - m_spec.sourceInUs) * double(m_spec.fps.num)
                   / (1'000'000.0 * double(m_spec.fps.den)));
    const int frame = std::clamp(int(index), 0, m_frameCount - 1);
    const auto it = m_patches.constFind(frame);
    if (it == m_patches.constEnd())
        return false;
    *patch = it.value();
    // Reading a patch keeps it resident while the user scrubs nearby.
    m_lru.removeAll(frame);
    m_lru.prepend(frame);
    return true;
}

void LiveRepairService::storePatch(int frame, const LiveRepairPatch &patch)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lru.removeAll(frame);
    m_lru.prepend(frame);
    m_patches.insert(frame, patch);
    while (m_lru.size() > m_cacheLimit) {
        const int victim = m_lru.takeLast();
        m_patches.remove(victim);
    }
}

bool LiveRepairService::processFrame(int frame)
{
    LiveRepairSpec spec;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (frame < 0 || frame >= m_frameCount)
            return true;
        if (m_patches.contains(frame) || m_emptyFrames.contains(frame))
            return true;
        spec = m_spec;
    }

    const QString maskPath = maskPathFor(spec.cacheDir, frame);
    if (!QFileInfo::exists(maskPath)) {
        // The user asked for live preview before (or outside) a completed
        // tracking run. Stop instead of silently drawing the original: that
        // would look like a successful repair of an empty hole.
        emit error(QStringLiteral("跟踪遮罩缺失（第 %1 帧）。"
                                  "请先运行 AI 跟踪以生成逐帧遮罩。")
                       .arg(frame));
        return false;
    }
    QString maskError;
    const auto mask = readMaskFile(maskPath, &maskError);
    if (!mask) {
        emit error(maskError.isEmpty()
                       ? QStringLiteral("无法读取跟踪遮罩（第 %1 帧）。").arg(frame)
                       : maskError);
        return false;
    }
    if (mask->width != spec.displaySize.width()
        || mask->height != spec.displaySize.height()) {
        emit error(QStringLiteral("跟踪遮罩尺寸与显示尺寸不一致（第 %1 帧）。")
                       .arg(frame));
        return false;
    }

    QString frameError;
    const QImage image = m_reader->readFrame(frame, &frameError);
    if (image.isNull()) {
        emit error(frameError.isEmpty()
                       ? QStringLiteral("无法读取实时预览帧（第 %1 帧）。").arg(frame)
                       : frameError);
        return false;
    }

    QString repairError;
    const auto patches =
        m_pipeline->repairChunk({image}, {*mask}, spec.parameters,
                                spec.featherPx, {}, &repairError);
    if (!patches) {
        emit error(repairError.isEmpty()
                       ? QStringLiteral("实时预览修复失败（第 %1 帧）。").arg(frame)
                       : repairError);
        return false;
    }
    if (patches->isEmpty()) {
        // This frame has no hole; the original frame is the correct result and
        // the compositor falls through to the cache/original on a miss.
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_emptyFrames.insert(frame);
        return true;
    }

    const PatchResult &result = patches->first();
    storePatch(frame, LiveRepairPatch{result.rgba, result.crop});
    emit frameReady(frame);
    return true;
}

void LiveRepairService::run()
{
    while (!m_stop.load()) {
        const int target = frameForSourceUs(m_targetUs.load());
        if (target < 0)
            break;
        const quint64 serial = m_targetSerial.load();

        if (!processFrame(target))
            break;

        // Prefetch the window sequentially in each direction: FrameReader
        // decodes forward cheaply, while every backwards step seeks to a
        // keyframe. The playhead still wins because the target frame is
        // repaired before either sweep, and a target change aborts the sweep.
        const double fpsValue = [this] {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            return m_spec.fps.toDouble();
        }();
        const int radius = std::clamp(int(std::lround(kWindowSeconds * fpsValue)),
                                      1, 600);
        bool failed = false;
        bool jumped = false;
        for (int offset = 1; offset <= radius && !m_stop.load(); ++offset) {
            if (m_targetSerial.load() != serial) {
                jumped = true;
                break;
            }
            if (!processFrame(target + offset)) {
                failed = true;
                break;
            }
        }
        for (int offset = 1;
             offset <= radius && !m_stop.load() && !failed && !jumped;
             ++offset) {
            if (m_targetSerial.load() != serial) {
                jumped = true;
                break;
            }
            if (!processFrame(target - offset)) {
                failed = true;
                break;
            }
        }
        if (failed || m_stop.load())
            break;
        if (jumped)
            continue; // repair the new playhead frame before waiting

        // Nothing else to do until the target changes. The timeout keeps a
        // missed wake-up from parking the worker forever.
        std::unique_lock<std::mutex> lock(m_waitMutex);
        m_waitCv.wait_for(lock, std::chrono::milliseconds(500), [this, serial] {
            return m_stop.load() || m_targetSerial.load() != serial;
        });
    }
}

void LiveRepairService::setInpainterForTest(
    std::unique_ptr<Inpainter> inpainter)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_testInpainter = std::move(inpainter);
}

void LiveRepairService::setCacheLimitForTest(int limit)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_cacheLimit = std::max(1, limit);
    while (m_lru.size() > m_cacheLimit) {
        const int victim = m_lru.takeLast();
        m_patches.remove(victim);
    }
}

int LiveRepairService::cachedPatchCountForTest() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return int(m_patches.size());
}

int LiveRepairService::frameCountForTest() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_frameCount;
}

} // namespace VideoEditor
