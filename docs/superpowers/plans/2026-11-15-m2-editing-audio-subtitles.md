# M2 编辑能力（时间线、音频、字幕、变速）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 M1 媒体基础上交付完整剪辑能力：多轨时间线（移动/裁切/分割/删除/波纹/复制粘贴/吸附/音视频链接）、背景音乐（波形、音量/声像/淡入淡出/音量关键帧）、字幕（逐条编辑、样式、SRT 导入导出、预览烧录）、片段级 0.25–4 倍变速，并以音频主时钟驱动预览，音画偏差不超过一帧，全部编辑可撤销。

**Architecture:** 保持 M1 的分层：`videoeditor_core`（工程模型、迁移、命令、求值器、SRT）、`videoeditor_media`（解码、音频混音、WASAPI、D3D11 合成、波形）、`videoeditor_ui`（增量时间线模型、控制器、预览桥接）、`videoeditor_app`（装配与 QML）。合成在独立解码线程的 D3D11 设备上把多轨图层按顺序绘制到单个共享纹理环形缓冲，再由 Qt 场景图通过 `QNativeInterface::QSGD3D11Texture` 显示；音频在独立线程以 48 kHz 浮点混音，WASAPI 播放位置作为主时钟，视频按该时钟选择帧。工程 schema 升到 v2 并显式迁移 v1。

**Tech Stack:** C++20、Qt 6.8.x（Core/Gui/Quick/QuickControls2/Qml/Test）、FFmpeg 7.1 LGPL（libavformat/libavcodec/libavutil/libswscale/libswresample）、Direct3D 11、WASAPI（ole32）、GoogleTest、CMake 3.27+、MSVC 2022。

**Spec:** [docs/superpowers/specs/2026-11-14-video-editor-design.md](../specs/2026-11-14-video-editor-design.md)（M2 对应 §3.1、§4.1–4.5、§5.5、§6–§9、§11）

## Global Constraints

- 仅 Windows 10 21H2+ / Windows 11 x64；MSVC 2022 19.4x；CMake ≥ 3.27；C++20；源码 UTF-8（`/utf-8`）。
- Qt 固定 6.8.x；FFmpeg 固定 7.1.x LGPL。不得链接 GPL 组件；`ffmpeg.exe` 只用于代理/测试素材生成。
- 图形后端只能是 D3D11：不写 Vulkan/OpenGL/D3D12 路径；解码帧到显示全程无 CPU 读回（字幕与波形的 CPU 光栅化是编辑态一次性工作，不参与逐帧视频路径）。
- 时间统一 `int64_t` 微秒；帧率用 `Rational{num, den}`；禁止用浮点累加计算时间。音频 DSP 内允许用 `float/double` 做采样插值，但每个块的位置必须由整数时间重新计算，不跨块累加浮点。
- 变速范围 0.25–4.0，用整数 `speedMilli`（250–4000）表示，避免浮点误差；不做速度曲线与光流插帧。
- 内部音频固定 48 kHz、浮点、双声道；实时混音与导出都不得削波；音画同步容差 ≤ 1 帧。
- 原始素材只读；工程保存原子写入（`QSaveFile`）；schema 迁移显式、可测试；v1 工程必须能打开。
- 面向用户的字符串用中文；标识符、日志、注释用英文。
- 字幕首版只做手动编辑 + SRT；不做 ASR、软字幕轨（mov_text）。
- 导出（M3）与 AI 消除（M4）不在 M2 范围；但合成函数必须可被 M3 复用，不得写“仅预览”分支。
- 单元测试不得强制依赖 GPU 或音频设备：无法创建 D3D11 设备时 `GTEST_SKIP()`；WASAPI 测试用注入的 `NullAudioSink`。
- 每个任务结束必须 `cmake --build --preset windows-x64-release` 且 `ctest --preset windows-x64-release --output-on-failure` 通过后提交；M1 的 `core_tests`、`media_tests`、`app_tests`、`preview_*` 必须保持绿色。
- 所有提交信息末尾附加：`Co-Authored-By: Claude Code <noreply@anthropic.com>`。

## Review Focus

以下五类输入/条件最容易让用户感知到错误，且规格没有逐条展开。每个测试放进拥有该代码的任务：

1. **链接的音视频片段在非关键帧、不同源帧率下移动/裁切/分割**：两轨必须保持同一时间线区间，撤销一次恢复两者，不能漂移。测试在 Task 3（映射）与 Task 4（命令）。
2. **变速边界 0.25/4.0 与短片段组合**：1 帧片段变速后不能变成 0 长度、黑帧或负时长；再裁切/分割仍合法。测试在 Task 3、Task 4、Task 7。
3. **真实工具导出的 SRT**：Windows CRLF、多余空行、`-->` 两侧空格、UTF-8 BOM、UTF-16、GBK/ANSI、重叠字幕、毫秒 1–3 位、时间码 `,`/`.` 混用必须正确解析且导出可回读。测试在 Task 5。
4. **没有音频设备、设备被独占、采样率不匹配、播放欠载**：预览不得卡死或崩溃；无设备时静音继续按壁钟预览并提示。测试在 Task 8，手动验收在 Task 12。
5. **素材丢失、空轨、禁用轨、片段超出素材时长、v1 老工程**：打开工程不丢引用、不崩溃；求值器把缺失部分当空隙/黑场处理。测试在 Task 1、Task 2、Task 3、Task 10。

---

## Design Decisions

1. **片段引用与源区间**：`Clip` 只存 `assetId` 与 `sourceInUs/sourceOutUs`（源素材时间，半开区间），不内嵌素材副本。`timelineDurationUs = (sourceOut-sourceIn) * 1000 / speedMilli`（整数四舍五入）。链接用 `linkId`：同一 `linkId` 的片段构成链接组，**组内共享 `timelineStartUs`、`speedMilli` 与同一时间线增量**（不同素材可以有不同的 `sourceInUs`，因为原声与画面可能来自不同文件的入点）；所有影响链接组的编辑在**一个** `TimelineEditCommand` 内完成，一次撤销即可恢复整组。
2. **Schema v2**：新增 `timeline`、`tracks[]`、`subtitleStyles[]`；`assets[]` 与 v1 相同并扩展音频字段。v1 → v2 迁移在 `fromJson` 内完成：保留素材、补默认 `TimelineSettings`、补 `track-v1`/`track-a1`/`track-s1` 与默认字幕样式；保存写 v2。时间值以 JSON number 存储（微秒 < 2^53，精确）。
3. **合成方式**：**顺序 GPU 合成**而不是多层纹理叠加。`TimelineRenderWorker` 在解码线程把每一层（视频片段缩放到目标矩形、应用不透明度与变换、字幕整帧 RGBA 贴图）绘制到**一个**输出共享纹理环形缓冲，Qt 侧仍用 M1 的 `FrameBridge` 呈现。这样 `VideoPreviewItem`/`FrameBridge` 无需改动，且同一合成函数可被 M3 离线导出复用。
4. **字幕渲染路径**：用 `QPainter` 把当前时刻所有字幕光栅化到一张与工程同尺寸的透明 `QImage`（描边、背景、安全区对齐），按“字幕 id + 文本哈希 + 样式 + 分辨率”缓存；只有字幕集合变化时才重新光栅化与上传，逐帧只画一个全屏四边形。不用 D3D11 文字图集（CJK 字形覆盖与排版成本高），也不把字幕放在 QML 覆盖层（那样 M3 导出无法复用，预览/导出会分叉）。
5. **音频时钟与视频呈现**：音频线程按 10 ms 块混音并写入 WASAPI，时钟 = `startPositionUs + framesConsumed*1e6/48000`，用 `framesConsumed` 的绝对值计算，不做浮点累加。视频侧用工程帧率定时器读取时钟，映射到最近的工程帧；只有帧号变化才请求合成（落后时直接跳到当前帧，不追帧）。无音频设备时使用壁钟并提示“无音频输出，静音预览”。
6. **波形**：`FfmpegAudioSource` 全文件解码为 48 kHz 立体声，按 10 ms 一个桶记录 min/max，编码为带版本头的二进制 `.wf` 文件，放在 `AppLocalDataLocation/cache/waveform/<内容指纹>.wf`；异步生成，GUI 线程通过线程安全的 `WaveformService::peaks()` 读取；`WaveformImageProvider` 按请求宽度绘制 `QImage`，QML 用 `Image` 显示。
7. **变速**：视频按“时间线时间 → 源时间”映射后做帧选择（前进则丢弃/跳读，后退或跨度大则 seek），快速播放是帧采样，不做混合/插帧。音频：不保持音高时按 `speed` 线性重采样（自然变调）；保持音高时用 20 ms Hann 窗、50% 重叠的 OLA 时域伸缩（轻量、实时可用；M3 导出换更高质量算法）。两者都在每块重新用整数时间定位，避免累积误差。
8. **UI 与模型更新**：`TimelineModel` 以片段/字幕 id 为键做增量 diff：新增 `rowsInserted`、删除 `rowsRemoved`、属性变化 `dataChanged`，**从不** `beginResetModel`（除非整工程替换）。拖动过程中 QML 只改本地 `x`，松手时调用控制器一次，产生一个撤销步骤。播放头位置是控制器的独立属性，逐帧变化不触碰模型。

## Starting Point

M1 已完成并全部测试通过。本计划假设以下既有接口（不要重命名）：

- `Project { QString name; QList<Asset> assets; }`、`Asset { id, path, MediaInfo info, proxyPath }`、`ProjectSerializer::toJson/fromJson/save/load`、`ProjectDocument`（`QUndoStack`、`changed/assetsChanged/undoStateChanged/dirtyChanged`）。
- `IVideoDecoder { seek(int64_t), decodeNext(DecodedFrame&), info(), isHardware() }`、`createVideoDecoder(path, D3D11Context&, MediaInfo)`、`DecodedFrame { kind, texture, subresource, width, height, ptsUs, rgba }`。
- `D3D11Context::create(bool enableVideo, QString*)`、`SharedTextureRing`（3 个 RGBA 共享纹理 + 键控互斥量）、`FrameReadback::readbackSharedTexture`、`RgbaConverter::convertRgba`。
- `DecoderWorker`/`PreviewController`/`VideoPreviewItem`/`FrameBridge`（M1 资产预览路径；本计划在其上增加时间线模式，不破坏原行为）。
- 构建命令：`cmake --preset windows-x64-release`、`cmake --build --preset windows-x64-release`、`ctest --preset windows-x64-release --output-on-failure`。

---

### Task 1: Schema v2 时间线模型、序列化与 v1 迁移

**Files:**
- Create: `src/core/Timeline.h`
- Create: `src/core/Timeline.cpp`
- Create: `src/core/Project.cpp`
- Create: `tests/core/TimelineTest.cpp`
- Modify: `src/core/Project.h`
- Modify: `src/core/ProjectSerializer.h`
- Modify: `src/core/ProjectSerializer.cpp`
- Modify: `src/core/ProjectDocument.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `tests/core/ProjectSerializerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Rational`, `MediaInfo`, `QSaveFile`, `QUuid`.
- Produces:
  - `enum class TrackType { Video, Audio, Subtitle }`；`QString trackTypeToString(TrackType)`；`TrackType trackTypeFromString(const QString&, bool *ok = nullptr)`。
  - `struct TimelineSettings { int width; int height; Rational fps; int sampleRate; QString colorSpace; bool dropFrameTimecode; }`。
  - `struct Transform { int scaleMilli; int offsetXPermille; int offsetYPermille; int opacityMilli; }`。
  - `struct VolumeKeyframe { int64_t timeUs; int volumeMilli; }`。
  - `struct AudioParams { int volumeMilli; int panMilli; int64_t fadeInUs; int64_t fadeOutUs; bool muted; QList<VolumeKeyframe> keyframes; }`。
  - `struct Clip { QString id, assetId, linkId; int64_t sourceInUs, sourceOutUs, timelineStartUs; int speedMilli; bool preservePitch, enabled; Transform transform; AudioParams audio; int64_t timelineDurationUs() const; int64_t timelineEndUs() const; }`。
  - `struct SubtitleCue { QString id; int64_t startUs, endUs; QString text, styleId; int positionXPermille, positionYPermille; bool enabled; }`。
  - `struct SubtitleStyle { QString id, name, fontFamily; int fontPixelSize; quint32 colorArgb, outlineColorArgb; int outlineWidth; quint32 backgroundArgb; int alignment; }`。
  - `struct Track { QString id; TrackType type; QString name; bool enabled, muted, solo, locked; QList<Clip> clips; QList<SubtitleCue> cues; }`。
  - `Project { ..., TimelineSettings timeline; QList<Track> tracks; QList<SubtitleStyle> subtitleStyles; void ensureDefaultTracks(); }`，`kSchemaVersion = 2`。
  - `int64_t scaleBySpeed(int64_t timelineDeltaUs, int speedMilli)`、`int64_t unscaleBySpeed(int64_t sourceDeltaUs, int speedMilli)`、`int64_t sourceTimeAt(const Clip&, int64_t timelineUs)`。
  - `QList<SubtitleStyle> builtinSubtitleStyles()`（Task 5 使用，本任务先定义默认样式）。

- [ ] **Step 1: 写失败测试**

`tests/core/TimelineTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "Timeline.h"

using namespace VideoEditor;

namespace {

Clip makeClip(int64_t inUs, int64_t outUs, int speedMilli = 1000)
{
    Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = QStringLiteral("asset-1");
    clip.sourceInUs = inUs;
    clip.sourceOutUs = outUs;
    clip.timelineStartUs = 0;
    clip.speedMilli = speedMilli;
    return clip;
}

} // namespace

TEST(Timeline, ComputesDurationFromSpeed)
{
    EXPECT_EQ(makeClip(0, 2'000'000, 1000).timelineDurationUs(), 2'000'000);
    EXPECT_EQ(makeClip(0, 2'000'000, 2000).timelineDurationUs(), 1'000'000);
    EXPECT_EQ(makeClip(0, 2'000'000, 500).timelineDurationUs(), 4'000'000);
    EXPECT_EQ(makeClip(0, 1'000'000, 250).timelineDurationUs(), 4'000'000);
    EXPECT_EQ(makeClip(0, 1'000'000, 4000).timelineDurationUs(), 250'000);
}

TEST(Timeline, MapsTimelineTimeToSourceTimeWithoutFloat)
{
    Clip clip = makeClip(1'000'000, 5'000'000, 2000);
    clip.timelineStartUs = 3'000'000;
    EXPECT_EQ(sourceTimeAt(clip, 2'999'999), 1'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 3'000'000), 1'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 3'500'000), 2'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 5'000'000), 5'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 99'000'000), 5'000'000);
}

TEST(Timeline, SurvivesLongTimelineArithmeticAtFourX)
{
    // 2 hours of source at 4x: 1.8e10 * 4 * 1000 still fits in int64.
    Clip clip = makeClip(0, 7'200'000'000, 4000);
    EXPECT_EQ(clip.timelineDurationUs(), 1'800'000'000);
    EXPECT_EQ(sourceTimeAt(clip, 1'800'000'000), 7'200'000'000);
}

TEST(Timeline, TrackTypeRoundTrips)
{
    EXPECT_EQ(trackTypeFromString(QStringLiteral("video")), TrackType::Video);
    EXPECT_EQ(trackTypeFromString(QStringLiteral("audio")), TrackType::Audio);
    EXPECT_EQ(trackTypeFromString(QStringLiteral("subtitle")), TrackType::Subtitle);
    EXPECT_EQ(trackTypeToString(TrackType::Audio), QStringLiteral("audio"));
    bool ok = true;
    trackTypeFromString(QStringLiteral("nope"), &ok);
    EXPECT_FALSE(ok);
}

TEST(Timeline, EnsureDefaultTracksIsIdempotent)
{
    Project project;
    project.ensureDefaultTracks();
    project.ensureDefaultTracks();
    EXPECT_EQ(project.tracks.size(), 3);
    EXPECT_EQ(project.tracks.at(0).type, TrackType::Video);
    EXPECT_EQ(project.tracks.at(1).type, TrackType::Audio);
    EXPECT_EQ(project.tracks.at(2).type, TrackType::Subtitle);
    EXPECT_EQ(project.subtitleStyles.size(), 1);
    EXPECT_EQ(project.subtitleStyles.first().id, QStringLiteral("default"));
}
```

在 `tests/core/ProjectSerializerTest.cpp` 追加：

```cpp
namespace {

Project sampleTimelineProject()
{
    Project project;
    project.name = QStringLiteral("M2 工程");
    project.timeline.width = 1920;
    project.timeline.height = 1080;
    project.timeline.fps = Rational{30, 1};
    project.timeline.dropFrameTimecode = false;
    project.ensureDefaultTracks();

    MediaInfo info;
    info.path = QStringLiteral("C:/videos/素材.mp4");
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.audioCodec = QStringLiteral("aac");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = 10'000'000;
    info.hasAudio = true;
    info.supported = true;
    project.assets.append(Asset{QStringLiteral("asset-1"), info.path, info, {}});

    Clip video;
    video.id = QStringLiteral("clip-v");
    video.assetId = QStringLiteral("asset-1");
    video.linkId = QStringLiteral("link-1");
    video.sourceInUs = 1'000'000;
    video.sourceOutUs = 5'000'000;
    video.timelineStartUs = 500'000;
    video.speedMilli = 1500;
    video.preservePitch = true;
    video.transform.scaleMilli = 1200;
    video.transform.opacityMilli = 900;
    project.tracks[0].clips.append(video);

    Clip audio = video;
    audio.id = QStringLiteral("clip-a");
    audio.audio.volumeMilli = 750;
    audio.audio.panMilli = -250;
    audio.audio.fadeInUs = 200'000;
    audio.audio.fadeOutUs = 300'000;
    audio.audio.keyframes.append(VolumeKeyframe{0, 1000});
    audio.audio.keyframes.append(VolumeKeyframe{1'000'000, 500});
    project.tracks[1].clips.append(audio);

    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 0;
    cue.endUs = 2'000'000;
    cue.text = QStringLiteral("第一行\n第二行");
    cue.styleId = QStringLiteral("default");
    project.tracks[2].cues.append(cue);
    return project;
}

} // namespace

TEST(ProjectSerializer, RoundTripsTimelineSchemaV2)
{
    const Project original = sampleTimelineProject();
    const QJsonObject json = ProjectSerializer::toJson(original);
    EXPECT_EQ(json.value(QStringLiteral("schemaVersion")).toInt(),
              Project::kSchemaVersion);
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, original);
}

TEST(ProjectSerializer, MigratesVersionOneProject)
{
    QJsonObject v1 = ProjectSerializer::toJson(sampleTimelineProject());
    v1.insert(QStringLiteral("schemaVersion"), 1);
    v1.remove(QStringLiteral("timeline"));
    v1.remove(QStringLiteral("tracks"));
    v1.remove(QStringLiteral("subtitleStyles"));

    QString error;
    const auto restored = ProjectSerializer::fromJson(v1, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    ASSERT_EQ(restored->assets.size(), 1);
    EXPECT_EQ(restored->assets.first().path, QStringLiteral("C:/videos/素材.mp4"));
    EXPECT_EQ(restored->tracks.size(), 3);
    EXPECT_TRUE(restored->tracks.at(0).clips.isEmpty());
    EXPECT_EQ(restored->timeline.fps, (Rational{30, 1}));
    EXPECT_EQ(restored->subtitleStyles.size(), 1);

    // Saving a migrated project writes schemaVersion 2.
    const QJsonObject saved = ProjectSerializer::toJson(*restored);
    EXPECT_EQ(saved.value(QStringLiteral("schemaVersion")).toInt(), 2);
}

TEST(ProjectSerializer, RejectsInvalidClipRanges)
{
    QJsonObject json = ProjectSerializer::toJson(sampleTimelineProject());
    QJsonArray tracks = json.value(QStringLiteral("tracks")).toArray();
    QJsonObject videoTrack = tracks.at(0).toObject();
    QJsonArray clips = videoTrack.value(QStringLiteral("clips")).toArray();
    QJsonObject clip = clips.at(0).toObject();
    clip.insert(QStringLiteral("sourceOutUs"), clip.value(QStringLiteral("sourceInUs")).toDouble());
    clips.replace(0, clip);
    videoTrack.insert(QStringLiteral("clips"), clips);
    tracks.replace(0, videoTrack);
    json.insert(QStringLiteral("tracks"), tracks);

    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(json, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("片段")));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "Timeline|ProjectSerializer" --output-on-failure
```

预期：编译失败，找不到 `Timeline.h`。

- [ ] **Step 3: 实现时间线数据结构与映射**

`src/core/Timeline.h`：

```cpp
#pragma once

#include <cstdint>
#include <QList>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

enum class TrackType { Video, Audio, Subtitle };

QString trackTypeToString(TrackType type);
TrackType trackTypeFromString(const QString &text, bool *ok = nullptr);

constexpr int kDefaultSpeedMilli = 1000;
constexpr int kMinSpeedMilli = 250;
constexpr int kMaxSpeedMilli = 4000;

struct TimelineSettings {
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    int sampleRate = 48000;
    QString colorSpace = QStringLiteral("BT.709");
    bool dropFrameTimecode = false;

    bool operator==(const TimelineSettings &) const = default;
};

struct Transform {
    int scaleMilli = 1000;      // 1000 = fit inside the frame
    int offsetXPermille = 0;    // -1000..1000 of frame width from center
    int offsetYPermille = 0;
    int opacityMilli = 1000;

    bool operator==(const Transform &) const = default;
};

struct VolumeKeyframe {
    int64_t timeUs = 0;         // relative to the clip start on the timeline
    int volumeMilli = 1000;

    bool operator==(const VolumeKeyframe &) const = default;
};

struct AudioParams {
    int volumeMilli = 1000;
    int panMilli = 0;           // -1000 left .. 1000 right
    int64_t fadeInUs = 0;
    int64_t fadeOutUs = 0;
    bool muted = false;
    QList<VolumeKeyframe> keyframes;

    bool operator==(const AudioParams &) const = default;
};

struct Clip {
    QString id;
    QString assetId;
    QString linkId;             // empty = unlinked; linked clips share the id
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    int64_t timelineStartUs = 0;
    int speedMilli = kDefaultSpeedMilli;
    bool preservePitch = false;
    bool enabled = true;
    Transform transform;
    AudioParams audio;

    int64_t timelineDurationUs() const;
    int64_t timelineEndUs() const { return timelineStartUs + timelineDurationUs(); }

    bool operator==(const Clip &) const = default;
};

struct SubtitleCue {
    QString id;
    int64_t startUs = 0;
    int64_t endUs = 0;
    QString text;
    QString styleId = QStringLiteral("default");
    int positionXPermille = 500;
    int positionYPermille = 880;
    bool enabled = true;

    bool operator==(const SubtitleCue &) const = default;
};

struct SubtitleStyle {
    QString id = QStringLiteral("default");
    QString name = QStringLiteral("默认样式");
    QString fontFamily = QStringLiteral("Microsoft YaHei");
    int fontPixelSize = 48;
    quint32 colorArgb = 0xFFFFFFFF;
    quint32 outlineColorArgb = 0xFF000000;
    int outlineWidth = 2;
    quint32 backgroundArgb = 0x00000000;
    int alignment = 2;          // Qt::AlignHCenter

    bool operator==(const SubtitleStyle &) const = default;
};

struct Track {
    QString id;
    TrackType type = TrackType::Video;
    QString name;
    bool enabled = true;
    bool muted = false;
    bool solo = false;
    bool locked = false;
    QList<Clip> clips;
    QList<SubtitleCue> cues;

    bool operator==(const Track &) const = default;
};

QList<SubtitleStyle> builtinSubtitleStyles();

// Integer speed mapping. Both directions round half up so a clip that is
// trimmed and untrimmed by the same amount does not drift.
int64_t scaleBySpeed(int64_t timelineDeltaUs, int speedMilli);
int64_t unscaleBySpeed(int64_t sourceDeltaUs, int speedMilli);
int64_t sourceTimeAt(const Clip &clip, int64_t timelineUs);

} // namespace VideoEditor
```

`src/core/Timeline.cpp`：

```cpp
#include "Timeline.h"

namespace VideoEditor {

namespace {

int clampSpeed(int speedMilli)
{
    return qBound(kMinSpeedMilli, speedMilli, kMaxSpeedMilli);
}

} // namespace

QString trackTypeToString(TrackType type)
{
    switch (type) {
    case TrackType::Video:
        return QStringLiteral("video");
    case TrackType::Audio:
        return QStringLiteral("audio");
    case TrackType::Subtitle:
        return QStringLiteral("subtitle");
    }
    return QStringLiteral("video");
}

TrackType trackTypeFromString(const QString &text, bool *ok)
{
    const QString value = text.trimmed().toLower();
    if (ok)
        *ok = true;
    if (value == QStringLiteral("video"))
        return TrackType::Video;
    if (value == QStringLiteral("audio"))
        return TrackType::Audio;
    if (value == QStringLiteral("subtitle"))
        return TrackType::Subtitle;
    if (ok)
        *ok = false;
    return TrackType::Video;
}

QList<SubtitleStyle> builtinSubtitleStyles()
{
    SubtitleStyle boxed;
    boxed.id = QStringLiteral("boxed");
    boxed.name = QStringLiteral("黑底白字");
    boxed.backgroundArgb = 0xB0000000;
    boxed.outlineWidth = 1;

    SubtitleStyle bold;
    bold.id = QStringLiteral("bold-outline");
    bold.name = QStringLiteral("粗描边");
    bold.fontPixelSize = 56;
    bold.outlineWidth = 4;

    return {SubtitleStyle{}, boxed, bold};
}

int64_t scaleBySpeed(int64_t timelineDeltaUs, int speedMilli)
{
    if (timelineDeltaUs <= 0)
        return 0;
    return (timelineDeltaUs * clampSpeed(speedMilli) + 500) / 1000;
}

int64_t unscaleBySpeed(int64_t sourceDeltaUs, int speedMilli)
{
    if (sourceDeltaUs <= 0)
        return 0;
    const int speed = clampSpeed(speedMilli);
    return (sourceDeltaUs * 1000 + speed / 2) / speed;
}

int64_t Clip::timelineDurationUs() const
{
    return unscaleBySpeed(sourceOutUs - sourceInUs, speedMilli);
}

int64_t sourceTimeAt(const Clip &clip, int64_t timelineUs)
{
    const int64_t deltaUs = qMax<int64_t>(0, timelineUs - clip.timelineStartUs);
    const int64_t sourceUs = clip.sourceInUs + scaleBySpeed(deltaUs, clip.speedMilli);
    return qBound(clip.sourceInUs, sourceUs, clip.sourceOutUs);
}

} // namespace VideoEditor
```

修改 `src/core/Project.h`：

```cpp
#pragma once

#include <QList>
#include <QString>

#include "MediaInfo.h"
#include "Timeline.h"

namespace VideoEditor {

struct Asset {
    QString id;
    QString path;
    MediaInfo info;
    QString proxyPath;

    bool operator==(const Asset &) const = default;
};

struct Project {
    static constexpr int kSchemaVersion = 2;

    QString name = QStringLiteral("未命名工程");
    TimelineSettings timeline;
    QList<Asset> assets;
    QList<Track> tracks;
    QList<SubtitleStyle> subtitleStyles;

    void ensureDefaultTracks();
    const Asset *assetById(const QString &id) const;

    bool operator==(const Project &) const = default;
};

} // namespace VideoEditor
```

把 `ensureDefaultTracks` 与 `assetById` 的实现加到 `Timeline.cpp`（`Timeline.h` 已包含 `Track`/`SubtitleStyle`，`Project.h` 在 `src/core` 内被包含）：

```cpp
void Project::ensureDefaultTracks()
{
    auto hasType = [this](TrackType type) {
        for (const Track &track : tracks) {
            if (track.type == type)
                return true;
        }
        return false;
    };

    if (!hasType(TrackType::Video)) {
        Track track;
        track.id = QStringLiteral("track-v1");
        track.type = TrackType::Video;
        track.name = QStringLiteral("V1");
        tracks.append(track);
    }
    if (!hasType(TrackType::Audio)) {
        Track track;
        track.id = QStringLiteral("track-a1");
        track.type = TrackType::Audio;
        track.name = QStringLiteral("A1");
        tracks.append(track);
    }
    if (!hasType(TrackType::Subtitle)) {
        Track track;
        track.id = QStringLiteral("track-s1");
        track.type = TrackType::Subtitle;
        track.name = QStringLiteral("S1");
        tracks.append(track);
    }
    bool hasDefaultStyle = false;
    for (const SubtitleStyle &style : subtitleStyles) {
        if (style.id == QStringLiteral("default"))
            hasDefaultStyle = true;
    }
    if (!hasDefaultStyle)
        subtitleStyles.append(SubtitleStyle{});
}

const Asset *Project::assetById(const QString &id) const
{
    for (const Asset &asset : assets) {
        if (asset.id == id)
            return &asset;
    }
    return nullptr;
}
```

`Project.h` 的 `Asset` 与 `Project` 都定义在 Project.h，而 `Timeline.cpp` 只包含 `Timeline.h`，因此 `ensureDefaultTracks`/`assetById` 必须放在包含 `Project.h` 的文件里。本计划采用新建 `src/core/Project.cpp`（只 `#include "Project.h"`），并把上面两个函数的定义移入其中；`Project.h` 只保留声明。`Project.cpp` 加入 `src/core/CMakeLists.txt` 的源列表。

- [ ] **Step 4: 实现 v2 序列化与 v1 迁移**

`src/core/ProjectSerializer.h` 不变（签名已够）。`ProjectSerializer.cpp` 增加 `#include <QSet>`，并增加以下内部函数、替换 `toJson`/`fromJson`：

```cpp
QJsonObject transformToJson(const Transform &value)
{
    return {{QStringLiteral("scaleMilli"), value.scaleMilli},
            {QStringLiteral("offsetXPermille"), value.offsetXPermille},
            {QStringLiteral("offsetYPermille"), value.offsetYPermille},
            {QStringLiteral("opacityMilli"), value.opacityMilli}};
}

Transform transformFromJson(const QJsonObject &json)
{
    Transform value;
    value.scaleMilli = json.value(QStringLiteral("scaleMilli")).toInt(1000);
    value.offsetXPermille = json.value(QStringLiteral("offsetXPermille")).toInt(0);
    value.offsetYPermille = json.value(QStringLiteral("offsetYPermille")).toInt(0);
    value.opacityMilli = json.value(QStringLiteral("opacityMilli")).toInt(1000);
    return value;
}

QJsonObject audioToJson(const AudioParams &value)
{
    QJsonArray keyframes;
    for (const VolumeKeyframe &keyframe : value.keyframes) {
        keyframes.append(QJsonObject{
            {QStringLiteral("timeUs"), static_cast<double>(keyframe.timeUs)},
            {QStringLiteral("volumeMilli"), keyframe.volumeMilli}});
    }
    return {{QStringLiteral("volumeMilli"), value.volumeMilli},
            {QStringLiteral("panMilli"), value.panMilli},
            {QStringLiteral("fadeInUs"), static_cast<double>(value.fadeInUs)},
            {QStringLiteral("fadeOutUs"), static_cast<double>(value.fadeOutUs)},
            {QStringLiteral("muted"), value.muted},
            {QStringLiteral("keyframes"), keyframes}};
}

AudioParams audioFromJson(const QJsonObject &json)
{
    AudioParams value;
    value.volumeMilli = json.value(QStringLiteral("volumeMilli")).toInt(1000);
    value.panMilli = json.value(QStringLiteral("panMilli")).toInt(0);
    value.fadeInUs = static_cast<int64_t>(
        json.value(QStringLiteral("fadeInUs")).toDouble(0));
    value.fadeOutUs = static_cast<int64_t>(
        json.value(QStringLiteral("fadeOutUs")).toDouble(0));
    value.muted = json.value(QStringLiteral("muted")).toBool(false);
    for (const QJsonValue &entry : json.value(QStringLiteral("keyframes")).toArray()) {
        const QJsonObject object = entry.toObject();
        value.keyframes.append(VolumeKeyframe{
            static_cast<int64_t>(object.value(QStringLiteral("timeUs")).toDouble(0)),
            object.value(QStringLiteral("volumeMilli")).toInt(1000)});
    }
    return value;
}

QJsonObject clipToJson(const Clip &clip)
{
    return {{QStringLiteral("id"), clip.id},
            {QStringLiteral("assetId"), clip.assetId},
            {QStringLiteral("linkId"), clip.linkId},
            {QStringLiteral("sourceInUs"), static_cast<double>(clip.sourceInUs)},
            {QStringLiteral("sourceOutUs"), static_cast<double>(clip.sourceOutUs)},
            {QStringLiteral("timelineStartUs"), static_cast<double>(clip.timelineStartUs)},
            {QStringLiteral("speedMilli"), clip.speedMilli},
            {QStringLiteral("preservePitch"), clip.preservePitch},
            {QStringLiteral("enabled"), clip.enabled},
            {QStringLiteral("transform"), transformToJson(clip.transform)},
            {QStringLiteral("audio"), audioToJson(clip.audio)}};
}

Clip clipFromJson(const QJsonObject &json)
{
    Clip clip;
    clip.id = json.value(QStringLiteral("id")).toString();
    clip.assetId = json.value(QStringLiteral("assetId")).toString();
    clip.linkId = json.value(QStringLiteral("linkId")).toString();
    clip.sourceInUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceInUs")).toDouble(0));
    clip.sourceOutUs = static_cast<int64_t>(
        json.value(QStringLiteral("sourceOutUs")).toDouble(0));
    clip.timelineStartUs = static_cast<int64_t>(
        json.value(QStringLiteral("timelineStartUs")).toDouble(0));
    clip.speedMilli = json.value(QStringLiteral("speedMilli")).toInt(kDefaultSpeedMilli);
    clip.preservePitch = json.value(QStringLiteral("preservePitch")).toBool(false);
    clip.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    clip.transform = transformFromJson(json.value(QStringLiteral("transform")).toObject());
    clip.audio = audioFromJson(json.value(QStringLiteral("audio")).toObject());
    return clip;
}

QJsonObject cueToJson(const SubtitleCue &cue)
{
    return {{QStringLiteral("id"), cue.id},
            {QStringLiteral("startUs"), static_cast<double>(cue.startUs)},
            {QStringLiteral("endUs"), static_cast<double>(cue.endUs)},
            {QStringLiteral("text"), cue.text},
            {QStringLiteral("styleId"), cue.styleId},
            {QStringLiteral("positionXPermille"), cue.positionXPermille},
            {QStringLiteral("positionYPermille"), cue.positionYPermille},
            {QStringLiteral("enabled"), cue.enabled}};
}

SubtitleCue cueFromJson(const QJsonObject &json)
{
    SubtitleCue cue;
    cue.id = json.value(QStringLiteral("id")).toString();
    cue.startUs = static_cast<int64_t>(json.value(QStringLiteral("startUs")).toDouble(0));
    cue.endUs = static_cast<int64_t>(json.value(QStringLiteral("endUs")).toDouble(0));
    cue.text = json.value(QStringLiteral("text")).toString();
    cue.styleId = json.value(QStringLiteral("styleId")).toString(QStringLiteral("default"));
    cue.positionXPermille = json.value(QStringLiteral("positionXPermille")).toInt(500);
    cue.positionYPermille = json.value(QStringLiteral("positionYPermille")).toInt(880);
    cue.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    return cue;
}

QJsonObject styleToJson(const SubtitleStyle &style)
{
    return {{QStringLiteral("id"), style.id},
            {QStringLiteral("name"), style.name},
            {QStringLiteral("fontFamily"), style.fontFamily},
            {QStringLiteral("fontPixelSize"), style.fontPixelSize},
            {QStringLiteral("colorArgb"), static_cast<double>(style.colorArgb)},
            {QStringLiteral("outlineColorArgb"), static_cast<double>(style.outlineColorArgb)},
            {QStringLiteral("outlineWidth"), style.outlineWidth},
            {QStringLiteral("backgroundArgb"), static_cast<double>(style.backgroundArgb)},
            {QStringLiteral("alignment"), style.alignment}};
}

SubtitleStyle styleFromJson(const QJsonObject &json)
{
    SubtitleStyle style;
    style.id = json.value(QStringLiteral("id")).toString(QStringLiteral("default"));
    style.name = json.value(QStringLiteral("name")).toString(QStringLiteral("默认样式"));
    style.fontFamily = json.value(QStringLiteral("fontFamily"))
                           .toString(QStringLiteral("Microsoft YaHei"));
    style.fontPixelSize = json.value(QStringLiteral("fontPixelSize")).toInt(48);
    style.colorArgb = static_cast<quint32>(
        json.value(QStringLiteral("colorArgb")).toDouble(0xFFFFFFFFu));
    style.outlineColorArgb = static_cast<quint32>(
        json.value(QStringLiteral("outlineColorArgb")).toDouble(0xFF000000u));
    style.outlineWidth = json.value(QStringLiteral("outlineWidth")).toInt(2);
    style.backgroundArgb = static_cast<quint32>(
        json.value(QStringLiteral("backgroundArgb")).toDouble(0));
    style.alignment = json.value(QStringLiteral("alignment")).toInt(2);
    return style;
}

QJsonObject timelineToJson(const TimelineSettings &settings)
{
    return {{QStringLiteral("width"), settings.width},
            {QStringLiteral("height"), settings.height},
            {QStringLiteral("frameRate"), rationalToJson(settings.fps)},
            {QStringLiteral("sampleRate"), settings.sampleRate},
            {QStringLiteral("colorSpace"), settings.colorSpace},
            {QStringLiteral("dropFrameTimecode"), settings.dropFrameTimecode}};
}

TimelineSettings timelineFromJson(const QJsonObject &json)
{
    TimelineSettings settings;
    settings.width = json.value(QStringLiteral("width")).toInt(1920);
    settings.height = json.value(QStringLiteral("height")).toInt(1080);
    settings.fps = rationalFromJson(json.value(QStringLiteral("frameRate")).toObject());
    if (!settings.fps.isValid())
        settings.fps = Rational{30, 1};
    settings.sampleRate = json.value(QStringLiteral("sampleRate")).toInt(48000);
    settings.colorSpace = json.value(QStringLiteral("colorSpace"))
                              .toString(QStringLiteral("BT.709"));
    settings.dropFrameTimecode = json.value(QStringLiteral("dropFrameTimecode"))
                                     .toBool(false);
    return settings;
}

QJsonObject trackToJson(const Track &track)
{
    QJsonArray clips;
    for (const Clip &clip : track.clips)
        clips.append(clipToJson(clip));
    QJsonArray cues;
    for (const SubtitleCue &cue : track.cues)
        cues.append(cueToJson(cue));
    return {{QStringLiteral("id"), track.id},
            {QStringLiteral("type"), trackTypeToString(track.type)},
            {QStringLiteral("name"), track.name},
            {QStringLiteral("enabled"), track.enabled},
            {QStringLiteral("muted"), track.muted},
            {QStringLiteral("solo"), track.solo},
            {QStringLiteral("locked"), track.locked},
            {QStringLiteral("clips"), clips},
            {QStringLiteral("cues"), cues}};
}

Track trackFromJson(const QJsonObject &json, bool *ok)
{
    Track track;
    track.id = json.value(QStringLiteral("id")).toString();
    bool typeOk = false;
    track.type = trackTypeFromString(json.value(QStringLiteral("type")).toString(), &typeOk);
    track.name = json.value(QStringLiteral("name")).toString();
    track.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    track.muted = json.value(QStringLiteral("muted")).toBool(false);
    track.solo = json.value(QStringLiteral("solo")).toBool(false);
    track.locked = json.value(QStringLiteral("locked")).toBool(false);
    for (const QJsonValue &value : json.value(QStringLiteral("clips")).toArray())
        track.clips.append(clipFromJson(value.toObject()));
    for (const QJsonValue &value : json.value(QStringLiteral("cues")).toArray())
        track.cues.append(cueFromJson(value.toObject()));
    if (ok)
        *ok = typeOk && !track.id.isEmpty();
    return track;
}

QString validateProject(const Project &project)
{
    QSet<QString> trackIds;
    QSet<QString> clipIds;
    QSet<QString> cueIds;
    for (const Track &track : project.tracks) {
        if (track.id.isEmpty() || trackIds.contains(track.id))
            return QStringLiteral("工程包含无效或重复的轨道 id。");
        trackIds.insert(track.id);
        if (track.type == TrackType::Subtitle && !track.clips.isEmpty())
            return QStringLiteral("字幕轨“%1”不能包含视频/音频片段。").arg(track.name);
        if (track.type != TrackType::Subtitle && !track.cues.isEmpty())
            return QStringLiteral("轨道“%1”不能包含字幕条目。").arg(track.name);
        for (const Clip &clip : track.clips) {
            if (clip.id.isEmpty() || clipIds.contains(clip.id))
                return QStringLiteral("轨道“%1”包含无效或重复的片段 id。").arg(track.name);
            clipIds.insert(clip.id);
            if (clip.assetId.isEmpty())
                return QStringLiteral("片段“%1”缺少素材引用。").arg(clip.id);
            if (clip.sourceInUs < 0 || clip.sourceOutUs <= clip.sourceInUs)
                return QStringLiteral("片段“%1”的源区间无效。").arg(clip.id);
            if (clip.timelineStartUs < 0)
                return QStringLiteral("片段“%1”的起始时间为负。").arg(clip.id);
            if (clip.speedMilli < kMinSpeedMilli || clip.speedMilli > kMaxSpeedMilli)
                return QStringLiteral("片段“%1”的变速超出 0.25–4 倍。").arg(clip.id);
        }
        for (const SubtitleCue &cue : track.cues) {
            if (cue.id.isEmpty() || cueIds.contains(cue.id))
                return QStringLiteral("字幕轨“%1”包含无效或重复的字幕 id。").arg(track.name);
            cueIds.insert(cue.id);
            if (cue.endUs <= cue.startUs || cue.startUs < 0)
                return QStringLiteral("字幕“%1”的时间范围无效。").arg(cue.id);
        }
    }
    return {};
}
```

`toJson` 追加 `timeline`、`tracks`、`subtitleStyles` 三个字段。`fromJson` 的结构：

```cpp
std::optional<Project> ProjectSerializer::fromJson(const QJsonObject &json, QString *error)
{
    const int version = json.value(QStringLiteral("schemaVersion")).toInt(0);
    if (version <= 0) { /* existing Chinese error */ }
    if (version > Project::kSchemaVersion) { /* existing "版本过新" error */ }

    Project project;
    project.name = json.value(QStringLiteral("name"))
                       .toString(QStringLiteral("未命名工程"));
    // existing asset loop, unchanged
    if (version == 1) {
        project.ensureDefaultTracks();
        return project;
    }

    project.timeline = timelineFromJson(json.value(QStringLiteral("timeline")).toObject());
    for (const QJsonValue &value : json.value(QStringLiteral("tracks")).toArray()) {
        bool ok = false;
        Track track = trackFromJson(value.toObject(), &ok);
        if (!ok) {
            setError(error, QStringLiteral("工程包含无效的轨道条目。"));
            return std::nullopt;
        }
        project.tracks.append(track);
    }
    for (const QJsonValue &value : json.value(QStringLiteral("subtitleStyles")).toArray())
        project.subtitleStyles.append(styleFromJson(value.toObject()));
    // Do not inject default tracks for a v2 file that explicitly has none: an
    // empty timeline is valid, and round-tripping must preserve it exactly.
    // New projects get defaults from ProjectDocument's constructor; v1 files
    // get them in the migration branch above.

    const QString validation = validateProject(project);
    if (!validation.isEmpty()) {
        setError(error, validation);
        return std::nullopt;
    }
    return project;
}
```

`ProjectDocument` 构造函数里追加 `m_project.ensureDefaultTracks();`，并在 `setProject` 内调用 `project.ensureDefaultTracks()`。

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "Timeline|ProjectSerializer|ProjectDocument" --output-on-failure
```

预期：全部 PASS，M1 的工程序列化测试仍通过。

- [ ] **Step 6: 提交**

```bash
git add src/core tests/core tests/CMakeLists.txt
git commit -m "feat(core): add timeline schema v2 with explicit v1 migration"
```

---

### Task 2: 音频素材探测、导入与测试素材

**Files:**
- Modify: `src/core/MediaInfo.h`
- Modify: `src/core/CodecSupport.h`
- Modify: `src/core/CodecSupport.cpp`
- Modify: `src/core/ProjectSerializer.cpp`
- Modify: `src/media/probe/MediaProbe.cpp`
- Modify: `src/ui/MediaListModel.cpp`
- Modify: `src/media/proxy/ProxyPlanner.cpp`（仅加“无视频不生成代理”的保护，若已存在则确认）
- Modify: `tools/generate_fixtures.ps1`
- Create: `tests/fixtures/tone.wav`、`bgm.mp3`、`bgm.flac`、`bgm.m4a`、`sync.mp4`（由脚本生成并提交）
- Create: `tests/media/AudioProbeTest.cpp`
- Modify: `tests/core/CodecSupportTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MediaProbe::probe`、`classifyVideoCodec`、`MediaInfo`。
- Produces:
  - `enum class MediaKind { Unknown, Video, Audio }`；`MediaInfo { MediaKind kind; bool hasVideo; int audioSampleRate; int audioChannels; int64_t audioDurationUs; }`。
  - `bool isSupportedAudioCodec(const QString &ffmpegName)`（支持 `aac`、`mp3`、`flac`、`pcm_s16le`、`pcm_s24le`、`pcm_s32le`、`pcm_f32le`、`alac`）。
  - `MediaProbe::probe` 对纯音频文件返回 `kind == Audio`、`supported == true`；既无视频也无音频时返回中文错误。
  - `MediaListModel` 对音频素材显示 `音频 · <codec>` 与 `<Hz> · <N> 声道`。

- [ ] **Step 1: 扩展测试素材生成脚本**

在 `tools/generate_fixtures.ps1` 末尾追加：

```powershell
# M2 audio, sync and performance fixtures.
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" `
  -c:a pcm_s16le (Join-Path $out "tone.wav")
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" `
  -c:a libmp3lame -b:a 192k (Join-Path $out "bgm.mp3")
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" `
  -c:a flac (Join-Path $out "bgm.flac")
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" `
  -c:a aac -b:a 192k (Join-Path $out "bgm.m4a")
# sync.mp4: testsrc2 timestamp pattern + a 100 ms 1 kHz beep at every whole second.
& $ffmpeg -y -hide_banner -loglevel error `
  -f lavfi -i "testsrc2=size=640x360:rate=30:duration=10" `
  -f lavfi -i "aevalsrc=0.8*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.1):s=48000:d=10" `
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest (Join-Path $out "sync.mp4")
```

运行脚本（GPL CLI 仅用于生成素材）：

```powershell
$env:VIDEOEDITOR_FFMPEG = "C:/Users/David/AppData/Local/Microsoft/WinGet/Packages/BtbN.FFmpeg.GPL.Shared.7.1_Microsoft.Winget.Source_8wekyb3d8bbwe/ffmpeg-n7.1.5-12-g1fdbca85aa-win64-gpl-shared-7.1/bin/ffmpeg.exe"
./tools/generate_fixtures.ps1
```

预期：新增 5 个文件，`tone.wav` 为 2 秒、`bgm.*` 为 10 秒、`sync.mp4` 有音轨。

- [ ] **Step 2: 写失败测试**

`tests/media/AudioProbeTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>

#include "MediaProbe.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

bool requireFixture(const QString &path)
{
    if (QFileInfo::exists(path))
        return true;
    GTEST_SKIP() << "missing fixture: " << path.toStdString();
    return false;
}

} // namespace

TEST(AudioProbe, ProbesAllBackgroundMusicFormats)
{
    for (const QString name : {QStringLiteral("tone.wav"), QStringLiteral("bgm.mp3"),
                               QStringLiteral("bgm.flac"), QStringLiteral("bgm.m4a")}) {
        const QString path = fixture(name);
        if (!requireFixture(path))
            continue;
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        ASSERT_TRUE(info.has_value()) << name.toStdString() << ": "
                                      << error.toStdString();
        EXPECT_EQ(info->kind, MediaKind::Audio) << name.toStdString();
        EXPECT_FALSE(info->hasVideo) << name.toStdString();
        EXPECT_TRUE(info->hasAudio) << name.toStdString();
        EXPECT_TRUE(info->supported) << info->unsupportedReason.toStdString();
        EXPECT_GT(info->audioSampleRate, 0);
        EXPECT_EQ(info->audioChannels, 1);
        EXPECT_GT(info->audioDurationUs, 0);
        EXPECT_GT(info->durationUs, 0);
    }
}

TEST(AudioProbe, KeepsVideoProbeBehaviour)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!requireFixture(path))
        return;
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, MediaKind::Video);
    EXPECT_TRUE(info->hasVideo);
    EXPECT_TRUE(info->hasAudio);
    EXPECT_GT(info->width, 0);
}

TEST(AudioProbe, ReadsSyncFixtureAudioAndVideo)
{
    const QString path = fixture(QStringLiteral("sync.mp4"));
    if (!requireFixture(path))
        return;
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, MediaKind::Video);
    EXPECT_TRUE(info->hasAudio);
    EXPECT_EQ(info->videoCodec, QStringLiteral("h264"));
    EXPECT_GT(info->audioSampleRate, 0);
}
```

在 `tests/core/CodecSupportTest.cpp` 追加：

```cpp
TEST(CodecSupport, AcceptsBackgroundMusicCodecs)
{
    for (const QString name : {QStringLiteral("aac"), QStringLiteral("mp3"),
                               QStringLiteral("flac"), QStringLiteral("pcm_s16le"),
                               QStringLiteral("pcm_s24le"), QStringLiteral("alac")}) {
        EXPECT_TRUE(isSupportedAudioCodec(name)) << name.toStdString();
    }
    EXPECT_FALSE(isSupportedAudioCodec(QStringLiteral("opus")));
}
```

在 `tests/app/AppControllerTest.cpp` 追加（纯音频导入后媒体列表显示音频信息）：

```cpp
TEST(AppController, ImportsAudioOnlyAsset)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("bgm.mp3"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_TRUE(controller.lastError().isEmpty());
    const QVariant codec = controller.mediaModel()->data(
        controller.mediaModel()->index(0, 0), MediaListModel::CodecRole);
    EXPECT_TRUE(codec.toString().contains(QStringLiteral("音频")));
}
```

- [ ] **Step 3: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "AudioProbe|CodecSupport|AppController" --output-on-failure
```

预期：编译失败（`MediaKind`、`isSupportedAudioCodec` 不存在）。

- [ ] **Step 4: 实现音频探测**

`MediaInfo.h` 增加：

```cpp
enum class MediaKind { Unknown, Video, Audio };

struct MediaInfo {
    // existing fields unchanged
    MediaKind kind = MediaKind::Unknown;
    bool hasVideo = false;
    int audioSampleRate = 0;
    int audioChannels = 0;
    int64_t audioDurationUs = 0;
    // existing operator== stays default
};
```

`CodecSupport.h/.cpp` 增加：

```cpp
bool isSupportedAudioCodec(const QString &ffmpegName)
{
    static const QSet<QString> kSupported{
        QStringLiteral("aac"), QStringLiteral("mp3"), QStringLiteral("flac"),
        QStringLiteral("pcm_s16le"), QStringLiteral("pcm_s24le"),
        QStringLiteral("pcm_s32le"), QStringLiteral("pcm_f32le"),
        QStringLiteral("alac")};
    return kSupported.contains(ffmpegName.trimmed().toLower());
}
```

`MediaProbe.cpp` 先把 `MediaInfo info;` 的声明移到视频/音频流判定之前（现有代码在视频分支里才声明），然后在找到 `videoIndex` 之后改为：

```cpp
const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO,
                                           -1, -1, nullptr, 0);
if (audioIndex >= 0) {
    AVStream *audio = format->streams[audioIndex];
    info.hasAudio = true;
    info.audioCodec = QString::fromUtf8(avcodec_get_name(audio->codecpar->codec_id));
    info.audioSampleRate = audio->codecpar->sample_rate;
    info.audioChannels = audio->codecpar->ch_layout.nb_channels;
    if (audio->duration != AV_NOPTS_VALUE) {
        info.audioDurationUs = av_rescale_q(audio->duration, audio->time_base,
                                            AV_TIME_BASE_Q);
    }
}

if (videoIndex < 0) {
    if (audioIndex < 0) {
        setError(error, QStringLiteral("“%1”不包含视频或音频流。")
                            .arg(QFileInfo(path).fileName()));
        return std::nullopt;
    }
    info.kind = MediaKind::Audio;
    info.hasVideo = false;
    info.videoCodec.clear();
    info.container = containerName(format.get());
    if (info.durationUs <= 0)
        info.durationUs = info.audioDurationUs;
    if (info.durationUs <= 0 && format->duration != AV_NOPTS_VALUE)
        info.durationUs = format->duration;
    info.supported = isSupportedAudioCodec(info.audioCodec);
    if (!info.supported) {
        info.unsupportedReason = QStringLiteral(
            "不支持的音频编码（%1）。请先转码为 AAC 或 MP3。").arg(info.audioCodec);
    } else if (info.durationUs <= 0) {
        info.supported = false;
        info.unsupportedReason = QStringLiteral("无法读取音频时长，文件可能已损坏。");
    }
    return info;
}
info.kind = MediaKind::Video;
info.hasVideo = true;
```

注意：原有的 `videoIndex < 0` 提前返回必须删除，改为上面的分支；其余视频字段解析保持不变。`ProjectSerializer` 的 `infoToJson/infoFromJson` 增加 `kind`（字符串 `video`/`audio`，旧文件缺省 `video`）、`hasVideo`、`audioSampleRate`、`audioChannels`、`audioDurationUs`。

`MediaListModel.cpp` 的 `data()`：

```cpp
case CodecRole:
    if (asset.info.kind == MediaKind::Audio)
        return QStringLiteral("音频 · %1").arg(asset.info.audioCodec.toUpper());
    return friendlyCodecName(classifyVideoCodec(asset.info.videoCodec).kind);
case ResolutionRole:
    if (asset.info.kind == MediaKind::Audio) {
        return QStringLiteral("%1 Hz · %2 声道")
            .arg(asset.info.audioSampleRate)
            .arg(asset.info.audioChannels);
    }
    return QStringLiteral("%1×%2").arg(asset.info.width).arg(asset.info.height);
```

`ProxyPlanner.cpp` 开头加：

```cpp
if (!info.hasVideo || info.width <= 0 || info.height <= 0)
    return ProxyPlan{};
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "AudioProbe|CodecSupport|AppController|MediaProbe|MediaInfo" --output-on-failure
```

预期：全部 PASS（`bgm.m4a` 若本机 FFmpeg 缺 AAC 编码器则测试 SKIP 并打印原因）。

- [ ] **Step 6: 提交**

```bash
git add src tests tools/generate_fixtures.ps1 tests/fixtures
git commit -m "feat(media): probe audio-only assets and add background music fixtures"
```

---

### Task 3: 时间线求值器与丢帧时间码

**Files:**
- Create: `src/core/TimelineEvaluator.h`
- Create: `src/core/TimelineEvaluator.cpp`
- Modify: `src/core/TimeBase.h`
- Modify: `src/core/TimeBase.cpp`
- Create: `tests/core/TimelineEvaluatorTest.cpp`
- Modify: `tests/core/TimeBaseTest.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Project`、`Track`、`Clip`、`SubtitleCue`、`SubtitleStyle`、`sourceTimeAt`、`formatTimecode`。
- Produces:
  - `struct VideoLayer { QString trackId, clipId, assetId; int trackIndex; int64_t localTimeUs, sourceTimeUs; Transform transform; }`。
  - `struct AudioLayer { QString trackId, clipId; int trackIndex; int64_t localTimeUs, sourceTimeUs; bool trackMuted, trackSolo; Clip clip; }`。
  - `struct SubtitleLayer { QString trackId, cueId, text; SubtitleStyle style; int positionXPermille, positionYPermille; int64_t localTimeUs; }`。
  - `struct FrameComposition { int64_t timelineUs; QList<VideoLayer> video; QList<SubtitleLayer> subtitles; }`。
  - `TimelineEvaluator(const Project&)`；`FrameComposition frameAt(int64_t timelineUs) const`；`QList<AudioLayer> audioRange(int64_t startUs, int64_t endUs) const`；`int64_t durationUs() const`。
  - `QString formatTimecodeAtRate(int64_t micros, Rational fps, bool dropFrame)`。

- [ ] **Step 1: 写失败测试**

`tests/core/TimelineEvaluatorTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "TimelineEvaluator.h"

using namespace VideoEditor;

namespace {

Clip clip(const QString &id, const QString &asset, int64_t startUs, int64_t inUs,
          int64_t outUs, int speedMilli = 1000)
{
    Clip value;
    value.id = id;
    value.assetId = asset;
    value.timelineStartUs = startUs;
    value.sourceInUs = inUs;
    value.sourceOutUs = outUs;
    value.speedMilli = speedMilli;
    return value;
}

Project projectWithTracks()
{
    Project project;
    project.timeline.fps = Rational{30, 1};
    Track bottom;
    bottom.id = QStringLiteral("v1");
    bottom.name = QStringLiteral("V1");
    bottom.type = TrackType::Video;
    bottom.clips.append(clip(QStringLiteral("v1-c1"), QStringLiteral("a1"), 0,
                             0, 2'000'000));
    Track top;
    top.id = QStringLiteral("v2");
    top.name = QStringLiteral("V2");
    top.type = TrackType::Video;
    top.clips.append(clip(QStringLiteral("v2-c1"), QStringLiteral("a2"), 1'000'000,
                          0, 4'000'000));
    Track audio;
    audio.id = QStringLiteral("a1");
    audio.name = QStringLiteral("A1");
    audio.type = TrackType::Audio;
    audio.clips.append(clip(QStringLiteral("a1-c1"), QStringLiteral("a3"), 0,
                            0, 2'000'000));
    Track subs;
    subs.id = QStringLiteral("s1");
    subs.name = QStringLiteral("S1");
    subs.type = TrackType::Subtitle;
    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 500'000;
    cue.endUs = 1'500'000;
    cue.text = QStringLiteral("你好");
    subs.cues.append(cue);
    project.tracks = {bottom, top, audio, subs};
    project.subtitleStyles.append(SubtitleStyle{});
    return project;
}

} // namespace

TEST(TimelineEvaluator, OrdersVideoLayersBottomToTop)
{
    const Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(1'200'000);
    ASSERT_EQ(frame.video.size(), 2);
    EXPECT_EQ(frame.video.at(0).trackId, QStringLiteral("v1"));
    EXPECT_EQ(frame.video.at(1).trackId, QStringLiteral("v2"));
    EXPECT_EQ(frame.video.at(0).sourceTimeUs, 1'200'000);
    EXPECT_EQ(frame.video.at(1).sourceTimeUs, 200'000);
}

TEST(TimelineEvaluator, ReturnsGapsAndSkipsDisabledTracks)
{
    Project project = projectWithTracks();
    TimelineEvaluator evaluator(project);
    EXPECT_TRUE(evaluator.frameAt(2'500'000).video.size() == 1); // only V2
    EXPECT_EQ(evaluator.frameAt(2'500'000).video.first().trackId,
              QStringLiteral("v2"));
    EXPECT_TRUE(evaluator.frameAt(6'000'000).video.isEmpty());

    project.tracks[1].enabled = false;
    TimelineEvaluator disabled(project);
    EXPECT_TRUE(disabled.frameAt(2'500'000).video.isEmpty());
}

TEST(TimelineEvaluator, SubtitlesAreIndependentAndOnTop)
{
    const Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(1'000'000);
    ASSERT_EQ(frame.subtitles.size(), 1);
    EXPECT_EQ(frame.subtitles.first().text, QStringLiteral("你好"));
    EXPECT_EQ(frame.subtitles.first().style.id, QStringLiteral("default"));
    EXPECT_TRUE(evaluator.frameAt(2'000'000).subtitles.isEmpty());
}

TEST(TimelineEvaluator, AudioRangeIntersectsClips)
{
    const Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    const QList<AudioLayer> layers = evaluator.audioRange(1'500'000, 1'600'000);
    ASSERT_EQ(layers.size(), 1);
    EXPECT_EQ(layers.first().clipId, QStringLiteral("a1-c1"));
    EXPECT_EQ(layers.first().sourceTimeUs, 1'500'000);
    EXPECT_TRUE(evaluator.audioRange(2'000'000, 2'100'000).isEmpty());
}

TEST(TimelineEvaluator, AppliesSpeedToSourceMapping)
{
    Project project = projectWithTracks();
    project.tracks[1].clips[0].speedMilli = 2000;
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(2'000'000);
    const auto it = std::find_if(frame.video.cbegin(), frame.video.cend(),
                                 [](const VideoLayer &layer) {
                                     return layer.trackId == QStringLiteral("v2");
                                 });
    ASSERT_NE(it, frame.video.cend());
    EXPECT_EQ(it->sourceTimeUs, 500'000);
    EXPECT_EQ(it->localTimeUs, 1'000'000);
}

TEST(TimelineEvaluator, ReportsTimelineDuration)
{
    Project project = projectWithTracks();
    const TimelineEvaluator evaluator(project);
    EXPECT_EQ(evaluator.durationUs(), 5'000'000); // V2 ends at 1s + 4s
    project.tracks[3].cues[0].endUs = 9'000'000;
    const TimelineEvaluator longer(project);
    EXPECT_EQ(longer.durationUs(), 9'000'000);
}

TEST(TimelineEvaluator, PicksLastOverlappingClipInTrackOrder)
{
    Project project;
    Track track;
    track.id = QStringLiteral("v1");
    track.type = TrackType::Video;
    track.clips.append(clip(QStringLiteral("first"), QStringLiteral("a1"), 0, 0, 4'000'000));
    track.clips.append(clip(QStringLiteral("second"), QStringLiteral("a1"), 1'000'000, 0, 4'000'000));
    project.tracks.append(track);
    const TimelineEvaluator evaluator(project);
    const FrameComposition frame = evaluator.frameAt(1'500'000);
    ASSERT_EQ(frame.video.size(), 1);
    EXPECT_EQ(frame.video.first().clipId, QStringLiteral("second"));
}
```

在 `tests/core/TimeBaseTest.cpp` 追加：

```cpp
TEST(TimeBase, FormatsDropFrameTimecode)
{
    const Rational ntsc{30000, 1001};
    EXPECT_EQ(formatTimecodeAtRate(1'800 * 1'001'000LL / 30, ntsc, true),
              QStringLiteral("00:01:00;02"));
    EXPECT_EQ(formatTimecodeAtRate(17'982 * 1'001'000LL / 30, ntsc, true),
              QStringLiteral("00:10:00;00"));
    EXPECT_EQ(formatTimecodeAtRate(0, ntsc, true), QStringLiteral("00:00:00;00"));
    EXPECT_EQ(formatTimecodeAtRate(1'000'000, Rational{30, 1}, false),
              QStringLiteral("00:00:01:00"));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "TimelineEvaluator|TimeBase" --output-on-failure
```

预期：编译失败，找不到 `TimelineEvaluator.h`、`formatTimecodeAtRate`。

- [ ] **Step 3: 实现求值器**

`src/core/TimelineEvaluator.h`：

```cpp
#pragma once

#include <cstdint>
#include <QList>

#include "Timeline.h"

namespace VideoEditor {

struct VideoLayer {
    QString trackId;
    QString clipId;
    QString assetId;
    int trackIndex = 0;
    int64_t localTimeUs = 0;
    int64_t sourceTimeUs = 0;
    Transform transform;

    bool operator==(const VideoLayer &) const = default;
};

struct AudioLayer {
    QString trackId;
    QString clipId;
    int trackIndex = 0;
    int64_t localTimeUs = 0;
    int64_t sourceTimeUs = 0;
    bool trackMuted = false;
    bool trackSolo = false;
    Clip clip;

    bool operator==(const AudioLayer &) const = default;
};

struct SubtitleLayer {
    QString trackId;
    QString cueId;
    QString text;
    SubtitleStyle style;
    int positionXPermille = 500;
    int positionYPermille = 880;
    int64_t localTimeUs = 0;

    bool operator==(const SubtitleLayer &) const = default;
};

struct FrameComposition {
    int64_t timelineUs = 0;
    QList<VideoLayer> video;        // bottom to top
    QList<SubtitleLayer> subtitles; // bottom to top, always drawn last
};

class TimelineEvaluator {
public:
    explicit TimelineEvaluator(const Project &project) : m_project(project) {}

    FrameComposition frameAt(int64_t timelineUs) const;
    QList<AudioLayer> audioRange(int64_t startUs, int64_t endUs) const;
    int64_t durationUs() const;

private:
    const SubtitleStyle &styleFor(const QString &styleId) const;
    const Project &m_project;
};

} // namespace VideoEditor
```

`src/core/TimelineEvaluator.cpp`：

```cpp
#include "TimelineEvaluator.h"

#include <algorithm>

namespace VideoEditor {

namespace {

bool covers(const Clip &clip, int64_t timeUs)
{
    return timeUs >= clip.timelineStartUs && timeUs < clip.timelineEndUs();
}

} // namespace

const SubtitleStyle &TimelineEvaluator::styleFor(const QString &styleId) const
{
    for (const SubtitleStyle &style : m_project.subtitleStyles) {
        if (style.id == styleId)
            return style;
    }
    for (const SubtitleStyle &style : m_project.subtitleStyles) {
        if (style.id == QStringLiteral("default"))
            return style;
    }
    static const SubtitleStyle fallback;
    return fallback;
}

FrameComposition TimelineEvaluator::frameAt(int64_t timelineUs) const
{
    FrameComposition frame;
    frame.timelineUs = timelineUs;

    for (int trackIndex = 0; trackIndex < m_project.tracks.size(); ++trackIndex) {
        const Track &track = m_project.tracks.at(trackIndex);
        if (!track.enabled)
            continue;

        if (track.type == TrackType::Video) {
            // Overlaps are legal in the model; the last clip in track order wins.
            const Clip *active = nullptr;
            for (const Clip &clip : track.clips) {
                if (clip.enabled && covers(clip, timelineUs))
                    active = &clip;
            }
            if (!active)
                continue;
            VideoLayer layer;
            layer.trackId = track.id;
            layer.clipId = active->id;
            layer.assetId = active->assetId;
            layer.trackIndex = trackIndex;
            layer.localTimeUs = timelineUs - active->timelineStartUs;
            layer.sourceTimeUs = sourceTimeAt(*active, timelineUs);
            layer.transform = active->transform;
            frame.video.append(layer);
        } else if (track.type == TrackType::Subtitle) {
            for (const SubtitleCue &cue : track.cues) {
                if (!cue.enabled || timelineUs < cue.startUs || timelineUs >= cue.endUs)
                    continue;
                SubtitleLayer layer;
                layer.trackId = track.id;
                layer.cueId = cue.id;
                layer.text = cue.text;
                layer.style = styleFor(cue.styleId);
                layer.positionXPermille = cue.positionXPermille;
                layer.positionYPermille = cue.positionYPermille;
                layer.localTimeUs = timelineUs - cue.startUs;
                frame.subtitles.append(layer);
            }
        }
    }
    return frame;
}

QList<AudioLayer> TimelineEvaluator::audioRange(int64_t startUs, int64_t endUs) const
{
    QList<AudioLayer> layers;
    if (endUs <= startUs)
        return layers;

    for (int trackIndex = 0; trackIndex < m_project.tracks.size(); ++trackIndex) {
        const Track &track = m_project.tracks.at(trackIndex);
        if (!track.enabled || track.type != TrackType::Audio)
            continue;
        for (const Clip &clip : track.clips) {
            if (!clip.enabled)
                continue;
            if (clip.timelineEndUs() <= startUs || clip.timelineStartUs >= endUs)
                continue;
            const int64_t localUs = qMax<int64_t>(0, startUs - clip.timelineStartUs);
            AudioLayer layer;
            layer.trackId = track.id;
            layer.clipId = clip.id;
            layer.trackIndex = trackIndex;
            layer.localTimeUs = localUs;
            layer.sourceTimeUs = sourceTimeAt(clip, startUs);
            layer.trackMuted = track.muted;
            layer.trackSolo = track.solo;
            layer.clip = clip;
            layers.append(layer);
        }
    }
    return layers;
}

int64_t TimelineEvaluator::durationUs() const
{
    int64_t duration = 0;
    for (const Track &track : m_project.tracks) {
        for (const Clip &clip : track.clips)
            duration = qMax(duration, clip.timelineEndUs());
        for (const SubtitleCue &cue : track.cues)
            duration = qMax(duration, cue.endUs);
    }
    return duration;
}

} // namespace VideoEditor
```

- [ ] **Step 4: 实现丢帧时间码**

`TimeBase.h` 声明 `QString formatTimecodeAtRate(int64_t micros, Rational fps, bool dropFrame);`。`TimeBase.cpp`：

```cpp
QString formatTimecodeAtRate(int64_t micros, Rational fps, bool dropFrame)
{
    micros = qMax<int64_t>(0, micros);
    if (!fps.isValid())
        fps = Rational{30, 1};
    if (!dropFrame)
        return formatTimecode(micros, fps);

    const double nominal = fps.toDouble();
    const int64_t nominalFps = qMax<int64_t>(1, qRound64(nominal));
    // 29.97 -> 2, 59.94 -> 4 dropped frame numbers per minute.
    const int64_t dropPerMinute = qRound64(nominal * 0.066666);
    int64_t frames = microsToFrames(micros, fps);
    const int64_t framesPerMinute = nominalFps * 60 - dropPerMinute;
    const int64_t framesPer10Minutes = nominalFps * 600 - dropPerMinute * 9;
    const int64_t tens = frames / framesPer10Minutes;
    const int64_t remainder = frames % framesPer10Minutes;
    if (remainder >= dropPerMinute) {
        frames += dropPerMinute * 9 * tens
                 + dropPerMinute * ((remainder - dropPerMinute) / framesPerMinute);
    } else {
        frames += dropPerMinute * 9 * tens;
    }

    const int64_t frameNumber = frames % nominalFps;
    const int64_t totalSeconds = frames / nominalFps;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3;%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frameNumber, 2, 10, QLatin1Char('0'));
}
```

在 `TimelineController`（Task 6）中按 `timeline.dropFrameTimecode` 选择此函数。

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "TimelineEvaluator|TimeBase" --output-on-failure
```

预期：全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core tests/core tests/CMakeLists.txt
git commit -m "feat(core): add timeline evaluator, layer ordering and drop-frame timecode"
```

---

### Task 4: 可撤销时间线命令、链接组、分割/波纹/复制粘贴/吸附

**Files:**
- Create: `src/core/commands/TimelineEditCommand.h`
- Create: `src/core/commands/TimelineEditCommand.cpp`
- Create: `src/core/commands/TrackListCommand.h`
- Create: `src/core/commands/TrackListCommand.cpp`
- Create: `src/core/TimelineEditor.h`
- Create: `src/core/TimelineEditor.cpp`
- Modify: `src/core/ProjectDocument.h`
- Modify: `src/core/ProjectDocument.cpp`
- Create: `tests/core/TimelineEditorTest.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ProjectDocument`（`project()`、`undoStack()`、`setProject`）、`Project`、`Clip`、`Track`、`sourceTimeAt`、`unscaleBySpeed`、`framesToMicros`。
- Produces:
  - `struct ClipLocator { QString trackId; QString clipId; }`。
  - `struct TrackEdit { QString trackId; QList<Clip> beforeClips, afterClips; QList<SubtitleCue> beforeCues, afterCues; }`。
  - `ProjectDocument::applyTrackEdit(trackId, clips, cues)`、`replaceTracks(tracks)`、`trackById(id)`、信号 `timelineChanged()`、`tracksChanged()`。
  - `TimelineEditor`：`addClips`、`addClip`、`moveClip`、`trimStart`、`trimEnd`、`splitClip`、`deleteClip(ripple)`、`setClipSpeed`、`setClipAudio`、`setClipTransform`、`setClipEnabled`、`linkClips`、`unlinkClip`、`copy`、`paste`、`addTrack`、`removeTrack`、`moveTrack`、`snapTime`。
  - `QStringList TimelineEditor::addClips(const QList<QPair<QString, Clip>> &trackAndClip)`：一个 `TimelineEditCommand` 内添加多个轨道上的片段（用于默认音视频链接）。

- [ ] **Step 1: 写失败测试**

`tests/core/TimelineEditorTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "ProjectDocument.h"
#include "TimelineEditor.h"

using namespace VideoEditor;

namespace {

Asset makeAsset(const QString &id, int64_t durationUs)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/%1.mp4").arg(id);
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = durationUs;
    info.hasAudio = true;
    info.hasVideo = true;
    info.kind = MediaKind::Video;
    info.supported = true;
    return Asset{id, info.path, info, {}};
}

Clip makeClip(const QString &id, const QString &assetId, int64_t startUs,
              int64_t inUs, int64_t outUs)
{
    Clip clip;
    clip.id = id;
    clip.assetId = assetId;
    clip.timelineStartUs = startUs;
    clip.sourceInUs = inUs;
    clip.sourceOutUs = outUs;
    return clip;
}

struct Fixture {
    ProjectDocument document;
    Asset videoAsset = makeAsset(QStringLiteral("asset-v"), 20'000'000);
    Asset audioAsset = makeAsset(QStringLiteral("asset-a"), 20'000'000);

    Fixture()
    {
        document.setProject(Project{});
        document.addAsset(videoAsset);
        document.addAsset(audioAsset);
    }

    const Track &videoTrack() const { return document.project().tracks.at(0); }
    const Track &audioTrack() const { return document.project().tracks.at(1); }
};

Clip linkedPair(Fixture &fixture, const QString &linkId, int64_t startUs)
{
    TimelineEditor editor(fixture.document);
    Clip video = makeClip(QStringLiteral("clip-v"), QStringLiteral("asset-v"),
                          startUs, 1'000'000, 9'000'000);
    video.linkId = linkId;
    editor.addClip(QStringLiteral("track-v1"), video);
    Clip audio = makeClip(QStringLiteral("clip-a"), QStringLiteral("asset-a"),
                          startUs, 0, 8'000'000);
    audio.linkId = linkId;
    editor.addClip(QStringLiteral("track-a1"), audio);
    return video;
}

} // namespace

TEST(TimelineEditor, AddsClipsAsUndoableCommands)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    Clip clip = makeClip(QStringLiteral("clip-1"), QStringLiteral("asset-v"),
                         1'000'000, 0, 2'000'000);
    EXPECT_EQ(editor.addClip(QStringLiteral("track-v1"), clip),
              QStringLiteral("clip-1"));
    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_TRUE(fixture.document.canUndo());
    fixture.document.undo();
    EXPECT_TRUE(fixture.videoTrack().clips.isEmpty());
    fixture.document.redo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
}

TEST(TimelineEditor, MovesLinkedClipsAsOneUndoStep)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2'000'000);

    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 2'000'000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 0);
}

TEST(TimelineEditor, TrimsLinkedClipsAndKeepsSourceAligned)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceInUs, 2'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().timelineStartUs, 1'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceInUs, 1'000'000);

    editor.trimEnd(QStringLiteral("track-a1"), QStringLiteral("clip-a"), 5'000'000);
    EXPECT_EQ(fixture.videoTrack().clips.first().sourceOutUs, 6'000'000);
    EXPECT_EQ(fixture.audioTrack().clips.first().sourceOutUs, 5'000'000);
}

TEST(TimelineEditor, RejectsTrimsShorterThanOneFrame)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.trimStart(QStringLiteral("track-v1"), QStringLiteral("clip-v"),
                     9'000'000); // 1 frame before the end
    const Clip &clip = fixture.videoTrack().clips.first();
    EXPECT_GE(clip.timelineDurationUs(), framesToMicros(1, Rational{30, 1}));
}

TEST(TimelineEditor, SplitsLinkedClipsAndRegroupsRightHalves)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.splitClip(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 3'000'000);

    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    ASSERT_EQ(fixture.audioTrack().clips.size(), 2);
    const Clip &leftVideo = fixture.videoTrack().clips.at(0);
    const Clip &rightVideo = fixture.videoTrack().clips.at(1);
    EXPECT_EQ(leftVideo.sourceOutUs, 4'000'000);
    EXPECT_EQ(rightVideo.sourceInUs, 4'000'000);
    EXPECT_EQ(rightVideo.timelineStartUs, 3'000'000);
    EXPECT_EQ(rightVideo.linkId, fixture.audioTrack().clips.at(1).linkId);
    EXPECT_NE(rightVideo.linkId, leftVideo.linkId);
    EXPECT_EQ(fixture.audioTrack().clips.at(1).timelineStartUs, 3'000'000);

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.audioTrack().clips.size(), 1);
}

TEST(TimelineEditor, RippleDeleteShiftsLaterClips)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c1"), QStringLiteral("asset-v"),
                            0, 0, 2'000'000));
    editor.addClip(QStringLiteral("track-v1"),
                   makeClip(QStringLiteral("c2"), QStringLiteral("asset-v"),
                            2'000'000, 0, 2'000'000));
    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("c1"), true);

    ASSERT_EQ(fixture.videoTrack().clips.size(), 1);
    EXPECT_EQ(fixture.videoTrack().clips.first().id, QStringLiteral("c2"));
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineStartUs, 0);

    fixture.document.undo();
    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    EXPECT_EQ(fixture.videoTrack().clips.at(1).timelineStartUs, 2'000'000);
}

TEST(TimelineEditor, CopyPasteCreatesNewIdsAndOffsets)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 1'000'000);
    TimelineEditor editor(fixture.document);
    editor.copy({ClipLocator{QStringLiteral("track-v1"), QStringLiteral("clip-v")}});
    editor.paste(5'000'000, QStringLiteral("track-v1"));

    ASSERT_EQ(fixture.videoTrack().clips.size(), 2);
    const Clip &pasted = fixture.videoTrack().clips.at(1);
    EXPECT_NE(pasted.id, QStringLiteral("clip-v"));
    EXPECT_EQ(pasted.timelineStartUs, 5'000'000);
    // A copied linked clip keeps its grouping, but the pasted group gets a new
    // link id so it cannot accidentally re-link with the original.
    EXPECT_FALSE(pasted.linkId.isEmpty());
    EXPECT_NE(pasted.linkId, QStringLiteral("link-1"));

    fixture.document.undo();
    EXPECT_EQ(fixture.videoTrack().clips.size(), 1);
}

TEST(TimelineEditor, SpeedChangeKeepsLinkedPairsInSync)
{
    Fixture fixture;
    linkedPair(fixture, QStringLiteral("link-1"), 0);
    TimelineEditor editor(fixture.document);
    editor.setClipSpeed(QStringLiteral("track-v1"), QStringLiteral("clip-v"), 2000, true);

    EXPECT_EQ(fixture.videoTrack().clips.first().speedMilli, 2000);
    EXPECT_EQ(fixture.audioTrack().clips.first().speedMilli, 2000);
    EXPECT_TRUE(fixture.audioTrack().clips.first().preservePitch);
    EXPECT_EQ(fixture.videoTrack().clips.first().timelineDurationUs(), 4'000'000);
}

TEST(TimelineEditor, SnapUsesNearestEdgeWithinThreshold)
{
    const QList<int64_t> edges{1'000'000, 2'000'000, 5'000'000};
    const SnapResult near = TimelineEditor::snapTime(1'050'000, 100'000, edges);
    EXPECT_TRUE(near.snapped);
    EXPECT_EQ(near.timeUs, 1'000'000);

    const SnapResult far = TimelineEditor::snapTime(1'500'000, 100'000, edges);
    EXPECT_FALSE(far.snapped);
    EXPECT_EQ(far.timeUs, 1'500'000);
}

TEST(TimelineEditor, ReordersTracks)
{
    Fixture fixture;
    TimelineEditor editor(fixture.document);
    Track video2;
    video2.id = QStringLiteral("track-v2");
    video2.type = TrackType::Video;
    video2.name = QStringLiteral("V2");
    editor.addTrack(video2);
    ASSERT_EQ(fixture.document.project().tracks.size(), 4);

    editor.moveTrack(3, 0);
    EXPECT_EQ(fixture.document.project().tracks.first().id, QStringLiteral("track-v2"));
    fixture.document.undo();
    EXPECT_EQ(fixture.document.project().tracks.first().id, QStringLiteral("track-v1"));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R TimelineEditor --output-on-failure
```

预期：编译失败，找不到 `TimelineEditor.h`。

- [ ] **Step 3: 扩展 ProjectDocument**

`ProjectDocument.h` 追加：

```cpp
public:
    void applyTrackEdit(const QString &trackId, const QList<Clip> &clips,
                        const QList<SubtitleCue> &cues);
    void replaceTracks(const QList<Track> &tracks);
    Track *trackById(const QString &id);
    const Track *trackById(const QString &id) const;
signals:
    void timelineChanged();
    void tracksChanged();
```

`ProjectDocument.cpp`：

```cpp
void ProjectDocument::applyTrackEdit(const QString &trackId,
                                     const QList<Clip> &clips,
                                     const QList<SubtitleCue> &cues)
{
    for (Track &track : m_project.tracks) {
        if (track.id != trackId)
            continue;
        track.clips = clips;
        track.cues = cues;
        emit changed();
        emit timelineChanged();
        emit dirtyChanged();
        return;
    }
}

void ProjectDocument::replaceTracks(const QList<Track> &tracks)
{
    m_project.tracks = tracks;
    emit changed();
    emit tracksChanged();
    emit timelineChanged();
    emit dirtyChanged();
}

Track *ProjectDocument::trackById(const QString &id)
{
    for (Track &track : m_project.tracks) {
        if (track.id == id)
            return &track;
    }
    return nullptr;
}

const Track *ProjectDocument::trackById(const QString &id) const
{
    for (const Track &track : m_project.tracks) {
        if (track.id == id)
            return &track;
    }
    return nullptr;
}
```

`setProject` 内追加 `project.ensureDefaultTracks();`，并在发出既有信号后发出 `tracksChanged(); timelineChanged();`。

- [ ] **Step 4: 实现命令类**

`src/core/commands/TimelineEditCommand.h`：

```cpp
#pragma once

#include <QUndoCommand>
#include <QList>
#include <QString>

#include "Timeline.h"

namespace VideoEditor {

class ProjectDocument;

struct TrackEdit {
    QString trackId;
    QList<Clip> beforeClips;
    QList<Clip> afterClips;
    QList<SubtitleCue> beforeCues;
    QList<SubtitleCue> afterCues;
};

class TimelineEditCommand : public QUndoCommand {
public:
    TimelineEditCommand(ProjectDocument *document, const QString &text,
                        QList<TrackEdit> edits);
    void redo() override;
    void undo() override;

private:
    ProjectDocument *m_document;
    QList<TrackEdit> m_edits;
};

} // namespace VideoEditor
```

`TimelineEditCommand.cpp`：

```cpp
#include "TimelineEditCommand.h"

#include "ProjectDocument.h"

namespace VideoEditor {

TimelineEditCommand::TimelineEditCommand(ProjectDocument *document,
                                         const QString &text,
                                         QList<TrackEdit> edits)
    : m_document(document)
    , m_edits(std::move(edits))
{
    setText(text);
}

void TimelineEditCommand::redo()
{
    for (const TrackEdit &edit : m_edits)
        m_document->applyTrackEdit(edit.trackId, edit.afterClips, edit.afterCues);
}

void TimelineEditCommand::undo()
{
    for (const TrackEdit &edit : m_edits)
        m_document->applyTrackEdit(edit.trackId, edit.beforeClips, edit.beforeCues);
}

} // namespace VideoEditor
```

`TrackListCommand` 结构相同，保存 `QList<Track> m_before/m_after`，`redo()` 调 `replaceTracks(m_after)`，`undo()` 调 `m_before`。

- [ ] **Step 5: 实现 TimelineEditor 核心编辑算法**

`src/core/TimelineEditor.h`：

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <QList>
#include <QString>

#include "Timeline.h"
#include "commands/TimelineEditCommand.h" // TrackEdit is part of this API

namespace VideoEditor {

class ProjectDocument;

struct ClipLocator {
    QString trackId;
    QString clipId;
    bool operator==(const ClipLocator &) const = default;
};

struct SnapResult {
    int64_t timeUs = 0;
    bool snapped = false;
};

class TimelineEditor {
public:
    explicit TimelineEditor(ProjectDocument &document);

    QStringList addClips(const QList<QPair<QString, Clip>> &trackAndClip);
    QString addClip(const QString &trackId, Clip clip);
    void moveClip(const QString &trackId, const QString &clipId, int64_t newStartUs);
    void trimStart(const QString &trackId, const QString &clipId, int64_t newStartUs);
    void trimEnd(const QString &trackId, const QString &clipId, int64_t newEndUs);
    void splitClip(const QString &trackId, const QString &clipId, int64_t timelineUs);
    void deleteClip(const QString &trackId, const QString &clipId, bool ripple);
    void setClipSpeed(const QString &trackId, const QString &clipId,
                      int speedMilli, bool preservePitch);
    void setClipAudio(const QString &trackId, const QString &clipId,
                      const AudioParams &params);
    void setClipTransform(const QString &trackId, const QString &clipId,
                          const Transform &transform);
    void setClipEnabled(const QString &trackId, const QString &clipId, bool enabled);
    void linkClips(const QList<ClipLocator> &clips);
    void unlinkClip(const QString &trackId, const QString &clipId);
    void copy(const QList<ClipLocator> &clips);
    bool hasClipboard() const { return !m_clipboard.isEmpty(); }
    void paste(int64_t timelineUs, const QString &targetTrackId);
    void addTrack(const Track &track);
    void removeTrack(const QString &trackId);
    void moveTrack(int fromIndex, int toIndex);

    static SnapResult snapTime(int64_t timeUs, int64_t thresholdUs,
                               const QList<int64_t> &candidates);

private:
    struct ClipboardEntry {
        QString sourceTrackId;
        Clip clip;
    };

    QList<ClipLocator> linkGroup(const QString &trackId, const QString &clipId) const;
    const Clip *findClip(const QString &trackId, const QString &clipId) const;
    void pushEdits(const QString &text, QList<TrackEdit> edits);
    QList<TrackEdit> editsForClips(const QList<ClipLocator> &locators,
                                   const std::function<void(Clip &)> &mutate) const;
    QString uniqueLinkId() const;

    ProjectDocument &m_document;
    QList<ClipboardEntry> m_clipboard;
};

} // namespace VideoEditor
```

`src/core/TimelineEditor.cpp` 的关键实现（完整文件；包含 `ProjectDocument.h`、`TimelineEditCommand.h`、`TrackListCommand.h`、`<QHash>`、`<QSet>`、`<QUuid>`、`<algorithm>`、`<climits>`）：

```cpp
namespace {

int64_t oneFrameUs(const ProjectDocument &document)
{
    const Rational fps = document.project().timeline.fps.isValid()
                             ? document.project().timeline.fps
                             : Rational{30, 1};
    return framesToMicros(1, fps);
}

} // namespace

TimelineEditor::TimelineEditor(ProjectDocument &document)
    : m_document(document)
{
}

QList<ClipLocator> TimelineEditor::linkGroup(const QString &trackId,
                                             const QString &clipId) const
{
    const Clip *clip = findClip(trackId, clipId);
    if (!clip)
        return {};
    if (clip->linkId.isEmpty())
        return {ClipLocator{trackId, clipId}};

    QList<ClipLocator> group;
    for (const Track &track : m_document.project().tracks) {
        for (const Clip &candidate : track.clips) {
            if (candidate.linkId == clip->linkId)
                group.append(ClipLocator{track.id, candidate.id});
        }
    }
    return group;
}

const Clip *TimelineEditor::findClip(const QString &trackId,
                                     const QString &clipId) const
{
    const Track *track = m_document.trackById(trackId);
    if (!track)
        return nullptr;
    for (const Clip &clip : track->clips) {
        if (clip.id == clipId)
            return &clip;
    }
    return nullptr;
}

void TimelineEditor::pushEdits(const QString &text, QList<TrackEdit> edits)
{
    if (edits.isEmpty())
        return;
    m_document.undoStack()->push(
        new TimelineEditCommand(&m_document, text, std::move(edits)));
}

QList<TrackEdit> TimelineEditor::editsForClips(
    const QList<ClipLocator> &locators,
    const std::function<void(Clip &)> &mutate) const
{
    QList<TrackEdit> edits;
    QSet<QString> seenTracks;
    for (const ClipLocator &locator : locators) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track)
            continue;
        if (!seenTracks.contains(locator.trackId)) {
            TrackEdit edit;
            edit.trackId = locator.trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterClips = track->clips;
            edit.afterCues = track->cues;
            edits.append(edit);
            seenTracks.insert(locator.trackId);
        }
    }
    for (TrackEdit &edit : edits) {
        for (Clip &clip : edit.afterClips) {
            for (const ClipLocator &locator : locators) {
                if (locator.trackId == edit.trackId && locator.clipId == clip.id) {
                    mutate(clip);
                    break;
                }
            }
        }
    }
    return edits;
}

QStringList TimelineEditor::addClips(
    const QList<QPair<QString, Clip>> &trackAndClip)
{
    QList<TrackEdit> edits;
    QStringList addedIds;
    QHash<QString, int> editIndexByTrack;

    for (const auto &entry : trackAndClip) {
        const QString &trackId = entry.first;
        Clip clip = entry.second;
        const Track *track = m_document.trackById(trackId);
        if (!track || track->type == TrackType::Subtitle || track->locked)
            continue;
        if (clip.id.isEmpty())
            clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (clip.assetId.isEmpty() || clip.sourceOutUs <= clip.sourceInUs)
            continue;
        clip.speedMilli = qBound(kMinSpeedMilli, clip.speedMilli, kMaxSpeedMilli);
        clip.timelineStartUs = qMax<int64_t>(0, clip.timelineStartUs);

        if (!editIndexByTrack.contains(trackId)) {
            TrackEdit edit;
            edit.trackId = trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterClips = track->clips;
            edit.afterCues = track->cues;
            editIndexByTrack.insert(trackId, edits.size());
            edits.append(edit);
        }
        edits[editIndexByTrack.value(trackId)].afterClips.append(clip);
        addedIds.append(clip.id);
    }
    pushEdits(QStringLiteral("添加片段"), edits);
    return addedIds;
}

QString TimelineEditor::addClip(const QString &trackId, Clip clip)
{
    const QStringList ids = addClips({{trackId, clip}});
    return ids.isEmpty() ? QString() : ids.first();
}

void TimelineEditor::moveClip(const QString &trackId, const QString &clipId,
                              int64_t newStartUs)
{
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty())
        return;

    int64_t delta = newStartUs - findClip(trackId, clipId)->timelineStartUs;
    for (const ClipLocator &locator : group) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            continue;
        delta = qMax(delta, -clip->timelineStartUs);
        if (m_document.trackById(locator.trackId)->locked)
            return;
    }
    pushEdits(QStringLiteral("移动片段"),
              editsForClips(group, [delta](Clip &clip) {
                  clip.timelineStartUs += delta;
              }));
}

void TimelineEditor::trimStart(const QString &trackId, const QString &clipId,
                               int64_t newStartUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked || newStartUs < 0)
        return;
    const int64_t minUs = oneFrameUs(m_document);
    const int64_t maxStart = clicked->timelineEndUs() - minUs;
    newStartUs = qBound<int64_t>(0, newStartUs, maxStart);
    const int64_t timelineDelta = newStartUs - clicked->timelineStartUs;
    if (timelineDelta == 0)
        return;

    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    pushEdits(QStringLiteral("裁切片段入点"),
              editsForClips(group, [timelineDelta](Clip &clip) {
                  const int64_t sourceDelta = scaleBySpeed(timelineDelta, clip.speedMilli);
                  const int64_t minimumRemaining =
                      qMax<int64_t>(1, unscaleBySpeed(1, clip.speedMilli));
                  const int64_t newIn = qBound<int64_t>(
                      0, clip.sourceInUs + sourceDelta,
                      clip.sourceOutUs - minimumRemaining);
                  const int64_t actualTimelineDelta =
                      unscaleBySpeed(newIn - clip.sourceInUs, clip.speedMilli);
                  clip.sourceInUs = newIn;
                  clip.timelineStartUs = qMax<int64_t>(0,
                      clip.timelineStartUs + actualTimelineDelta);
              }));
}

void TimelineEditor::trimEnd(const QString &trackId, const QString &clipId,
                             int64_t newEndUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    const int64_t minUs = oneFrameUs(m_document);
    newEndUs = qMax(clicked->timelineStartUs + minUs, newEndUs);
    const int64_t wantedDuration = newEndUs - clicked->timelineStartUs;
    if (wantedDuration == clicked->timelineDurationUs())
        return;

    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    pushEdits(QStringLiteral("裁切片段出点"),
              editsForClips(group, [this, wantedDuration, minUs](Clip &clip) {
                  const int64_t minimumOut =
                      clip.sourceInUs + qMax<int64_t>(1, unscaleBySpeed(minUs, clip.speedMilli));
                  int64_t wantedOut = clip.sourceInUs
                      + scaleBySpeed(wantedDuration, clip.speedMilli);
                  if (const Asset *asset = m_document.project().assetById(clip.assetId))
                      wantedOut = qMin(wantedOut, asset->info.durationUs);
                  clip.sourceOutUs = qMax(minimumOut, wantedOut);
              }));
}

void TimelineEditor::splitClip(const QString &trackId, const QString &clipId,
                               int64_t timelineUs)
{
    const Clip *clicked = findClip(trackId, clipId);
    if (!clicked)
        return;
    if (timelineUs <= clicked->timelineStartUs || timelineUs >= clicked->timelineEndUs())
        return;
    const int64_t splitSource = sourceTimeAt(*clicked, timelineUs);
    if (splitSource <= clicked->sourceInUs || splitSource >= clicked->sourceOutUs)
        return;

    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    const QString rightLinkId =
        clicked->linkId.isEmpty() ? QString() : uniqueLinkId();

    QList<TrackEdit> edits;
    QHash<QString, int> editIndexByTrack;
    for (const ClipLocator &locator : group) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track || track->locked)
            return;
        if (!editIndexByTrack.contains(locator.trackId)) {
            TrackEdit edit;
            edit.trackId = locator.trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterClips = track->clips;
            edit.afterCues = track->cues;
            editIndexByTrack.insert(locator.trackId, edits.size());
            edits.append(edit);
        }
        TrackEdit &target = edits[editIndexByTrack.value(locator.trackId)];
        for (int i = 0; i < target.afterClips.size(); ++i) {
            Clip &left = target.afterClips[i];
            if (left.id != locator.clipId)
                continue;
            const int64_t sourceAtCut = sourceTimeAt(left, timelineUs);
            if (sourceAtCut <= left.sourceInUs || sourceAtCut >= left.sourceOutUs)
                break;
            Clip right = left;
            right.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            right.sourceInUs = sourceAtCut;
            right.timelineStartUs = timelineUs;
            if (!right.linkId.isEmpty())
                right.linkId = rightLinkId;
            left.sourceOutUs = sourceAtCut;
            target.afterClips.insert(i + 1, right);
            break;
        }
    }
    pushEdits(QStringLiteral("分割片段"), edits);
}

void TimelineEditor::deleteClip(const QString &trackId, const QString &clipId,
                                bool ripple)
{
    const QList<ClipLocator> group = linkGroup(trackId, clipId);
    if (group.isEmpty())
        return;

    QList<TrackEdit> edits;
    QHash<QString, int> editIndexByTrack;
    for (const ClipLocator &locator : group) {
        const Track *track = m_document.trackById(locator.trackId);
        if (!track || track->locked)
            return;
        if (!editIndexByTrack.contains(locator.trackId)) {
            TrackEdit edit;
            edit.trackId = locator.trackId;
            edit.beforeClips = track->clips;
            edit.beforeCues = track->cues;
            edit.afterCues = track->cues;
            editIndexByTrack.insert(locator.trackId, edits.size());
            edits.append(edit);
        }
        TrackEdit &edit = edits[editIndexByTrack.value(locator.trackId)];

        int64_t removedStart = 0;
        int64_t removedDuration = 0;
        for (const Clip &clip : track->clips) {
            for (const ClipLocator &onTrack : group) {
                if (onTrack.trackId == locator.trackId && onTrack.clipId == clip.id) {
                    removedStart = clip.timelineStartUs;
                    removedDuration = qMax(removedDuration, clip.timelineDurationUs());
                }
            }
        }

        edit.afterClips.clear();
        for (const Clip &clip : track->clips) {
            bool removed = false;
            for (const ClipLocator &onTrack : group) {
                if (onTrack.trackId == locator.trackId && onTrack.clipId == clip.id)
                    removed = true;
            }
            if (removed)
                continue;
            Clip adjusted = clip;
            if (ripple && removedDuration > 0
                && clip.timelineStartUs >= removedStart + removedDuration) {
                adjusted.timelineStartUs -= removedDuration;
            }
            edit.afterClips.append(adjusted);
        }
    }
    pushEdits(ripple ? QStringLiteral("波纹删除") : QStringLiteral("删除片段"), edits);
}
```

波纹删除的语义：被删片段在每条受影响轨道上的 `[removedStart, removedStart + removedDuration)` 区间被移除；同一轨道上起点不早于该区间结束的片段整体左移 `removedDuration`。链接组内的所有片段同时删除，但仍只产生一个撤销步骤。

`setClipSpeed` 同样作用于链接组；源区间不变，时长自动变化；同步更新 `linkId` 组内所有片段的 `speedMilli/preservePitch`：

```cpp
void TimelineEditor::setClipSpeed(const QString &trackId, const QString &clipId,
                                  int speedMilli, bool preservePitch)
{
    speedMilli = qBound(kMinSpeedMilli, speedMilli, kMaxSpeedMilli);
    pushEdits(QStringLiteral("设置片段变速"),
              editsForClips(linkGroup(trackId, clipId),
                            [speedMilli, preservePitch](Clip &clip) {
                                clip.speedMilli = speedMilli;
                                clip.preservePitch = preservePitch;
                            }));
}
```

`setClipAudio`/`setClipTransform`/`setClipEnabled` 只改点击片段（不联动）：

```cpp
void TimelineEditor::setClipAudio(const QString &trackId, const QString &clipId,
                                  const AudioParams &params)
{
    pushEdits(QStringLiteral("调整音频"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [params](Clip &clip) { clip.audio = params; }));
}

void TimelineEditor::setClipTransform(const QString &trackId,
                                      const QString &clipId,
                                      const Transform &transform)
{
    pushEdits(QStringLiteral("调整画面"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [transform](Clip &clip) { clip.transform = transform; }));
}

void TimelineEditor::setClipEnabled(const QString &trackId,
                                    const QString &clipId, bool enabled)
{
    pushEdits(enabled ? QStringLiteral("启用片段") : QStringLiteral("禁用片段"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [enabled](Clip &clip) { clip.enabled = enabled; }));
}
```

`linkClips`：为所有传入片段生成同一个新 `linkId`，并统一 `timelineStartUs` 为组内最小值、`speedMilli` 为第一个片段的倍速（保持简单可预测），命令文本“链接片段”。`unlinkClip` 只清空该片段的 `linkId`：

```cpp
void TimelineEditor::linkClips(const QList<ClipLocator> &clips)
{
    if (clips.size() < 2)
        return;
    const Clip *first = findClip(clips.first().trackId, clips.first().clipId);
    if (!first)
        return;
    const QString linkId = uniqueLinkId();
    int64_t startUs = first->timelineStartUs;
    int speedMilli = first->speedMilli;
    for (const ClipLocator &locator : clips) {
        const Clip *clip = findClip(locator.trackId, locator.clipId);
        if (!clip)
            return;
        startUs = qMin(startUs, clip->timelineStartUs);
    }
    pushEdits(QStringLiteral("链接片段"),
              editsForClips(clips, [linkId, startUs, speedMilli](Clip &clip) {
                  clip.linkId = linkId;
                  clip.timelineStartUs = startUs;
                  clip.speedMilli = speedMilli;
              }));
}

void TimelineEditor::unlinkClip(const QString &trackId, const QString &clipId)
{
    pushEdits(QStringLiteral("解链片段"),
              editsForClips({ClipLocator{trackId, clipId}},
                            [](Clip &clip) { clip.linkId.clear(); }));
}
```

`copy` 把每个 locator 的 `Clip` 副本与来源轨 id 存入 `m_clipboard`。`paste`：

```cpp
void TimelineEditor::paste(int64_t timelineUs, const QString &targetTrackId)
{
    if (m_clipboard.isEmpty())
        return;
    const Track *target = m_document.trackById(targetTrackId);
    if (!target || target->locked)
        return;

    int64_t minStart = INT64_MAX;
    for (const ClipboardEntry &entry : m_clipboard)
        minStart = qMin(minStart, entry.clip.timelineStartUs);

    QHash<QString, QList<Clip>> addedByTrack;
    QHash<QString, QString> linkMap;
    for (const ClipboardEntry &entry : m_clipboard) {
        const Track *sourceTrack = m_document.trackById(entry.sourceTrackId);
        QString destinationId = entry.sourceTrackId;
        if (!sourceTrack || sourceTrack->type != target->type) {
            if (sourceTrack && sourceTrack->type != target->type)
                continue;
            destinationId = targetTrackId;
        }
        const Track *destination = m_document.trackById(destinationId);
        if (!destination || destination->locked || destination->type != target->type)
            continue;

        Clip copy = entry.clip;
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copy.timelineStartUs = timelineUs + (entry.clip.timelineStartUs - minStart);
        if (!copy.linkId.isEmpty()) {
            if (!linkMap.contains(copy.linkId))
                linkMap.insert(copy.linkId,
                               QUuid::createUuid().toString(QUuid::WithoutBraces));
            copy.linkId = linkMap.value(copy.linkId);
        }
        addedByTrack[destinationId].append(copy);
    }

    QList<TrackEdit> edits;
    for (auto it = addedByTrack.cbegin(); it != addedByTrack.cend(); ++it) {
        const Track *track = m_document.trackById(it.key());
        if (!track)
            continue;
        TrackEdit edit;
        edit.trackId = it.key();
        edit.beforeClips = track->clips;
        edit.afterClips = track->clips;
        edit.afterClips.append(it.value());
        edit.beforeCues = track->cues;
        edit.afterCues = track->cues;
        edits.append(edit);
    }
    pushEdits(QStringLiteral("粘贴片段"), edits);
}
```

`addTrack`/`removeTrack`/`moveTrack` 用 `TrackListCommand` 保存完整轨道列表前后快照：

```cpp
void TimelineEditor::addTrack(const Track &track)
{
    QList<Track> before = m_document.project().tracks;
    QList<Track> after = before;
    Track added = track;
    if (added.id.isEmpty())
        added.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    after.append(added);
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("添加轨道"), before, after));
}

void TimelineEditor::removeTrack(const QString &trackId)
{
    QList<Track> before = m_document.project().tracks;
    QList<Track> after;
    for (const Track &track : before) {
        if (track.id != trackId)
            after.append(track);
    }
    if (after.size() == before.size())
        return;
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("删除轨道"), before, after));
}

void TimelineEditor::moveTrack(int fromIndex, int toIndex)
{
    QList<Track> before = m_document.project().tracks;
    if (fromIndex < 0 || fromIndex >= before.size()
        || toIndex < 0 || toIndex >= before.size()) {
        return;
    }
    QList<Track> after = before;
    after.move(fromIndex, toIndex);
    m_document.undoStack()->push(new TrackListCommand(
        &m_document, QStringLiteral("调整轨道顺序"), before, after));
}

QString TimelineEditor::uniqueLinkId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
```

`snapTime`：

```cpp
SnapResult TimelineEditor::snapTime(int64_t timeUs, int64_t thresholdUs,
                                    const QList<int64_t> &candidates)
{
    SnapResult result{timeUs, false};
    int64_t bestDistance = thresholdUs + 1;
    for (const int64_t candidate : candidates) {
        const int64_t distance = qAbs(candidate - timeUs);
        if (distance <= thresholdUs && distance < bestDistance) {
            result.timeUs = candidate;
            result.snapped = true;
            bestDistance = distance;
        }
    }
    return result;
}
```

- [ ] **Step 6: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "TimelineEditor|ProjectDocument|ProjectSerializer" --output-on-failure
```

预期：全部 PASS；特别是 `MovesLinkedClipsAsOneUndoStep` 与 `SplitsLinkedClipsAndRegroupsRightHalves`。

- [ ] **Step 7: 提交**

```bash
git add src/core tests/core tests/CMakeLists.txt
git commit -m "feat(core): add undoable timeline editing, linked clips and ripple operations"
```

---

### Task 5: 字幕命令、样式预设与 SRT 导入导出

**Files:**
- Create: `src/core/Srt.h`
- Create: `src/core/Srt.cpp`
- Create: `tests/core/SrtTest.cpp`
- Create: `tests/core/SubtitleEditorTest.cpp`
- Modify: `src/core/TimelineEditor.h`
- Modify: `src/core/TimelineEditor.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SubtitleCue`、`SubtitleStyle`、`builtinSubtitleStyles()`、`TimelineEditCommand`、`ProjectDocument::applyTrackEdit`。
- Produces:
  - `QString decodeTextFile(const QByteArray &bytes)`：BOM（UTF-8/UTF-16LE/UTF-16BE）优先；严格 UTF-8；失败回退 Windows 系统 ANSI（中文系统为 GBK/GB18030）。
  - `struct SrtParseResult { QList<SubtitleCue> cues; QString error; bool ok() const; }`。
  - `SrtParseResult parseSrt(const QByteArray &bytes, const QString &defaultStyleId = QStringLiteral("default"))`。
  - `QByteArray writeSrt(const QList<SubtitleCue> &cues)`：UTF-8 BOM + CRLF，只导出 `enabled` 的条目，按 `startUs` 稳定排序。
  - `bool parseSrtTime(const QString &text, int64_t *startUs, int64_t *endUs)`。
  - `QString formatSrtTime(int64_t us)`。
  - `TimelineEditor` 新增：`addSubtitleCue`、`updateSubtitleCue`、`removeSubtitleCue`、`applySubtitleStyle`、`importSrt`、`exportSrt`。

- [ ] **Step 1: 写失败测试**

`tests/core/SrtTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "Srt.h"

using namespace VideoEditor;

TEST(Srt, ParsesCrLfWithBomAndIndexLines)
{
    QByteArray bytes("\xEF\xBB\xBF", 3);
    bytes += "1\r\n00:00:01,000 --> 00:00:03,500\r\n第一行\r\n第二行\r\n\r\n"
             "2\r\n00:00:04.250 --> 00:00:05.000\r\nHello\r\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 2);
    EXPECT_EQ(result.cues.at(0).startUs, 1'000'000);
    EXPECT_EQ(result.cues.at(0).endUs, 3'500'000);
    EXPECT_EQ(result.cues.at(0).text, QStringLiteral("第一行\n第二行"));
    EXPECT_EQ(result.cues.at(1).startUs, 4'250'000);
    EXPECT_EQ(result.cues.at(1).text, QStringLiteral("Hello"));
    EXPECT_EQ(result.cues.at(1).styleId, QStringLiteral("default"));
}

TEST(Srt, AcceptsShortMillisecondFields)
{
    const QByteArray bytes = "00:00:00,5 --> 00:00:01,25\ntext\n";
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().startUs, 500'000);
    EXPECT_EQ(result.cues.first().endUs, 1'250'000);
}

TEST(Srt, DecodesUtf16Le)
{
    const QString text = QStringLiteral("1\n00:00:00,000 --> 00:00:01,000\n中文字幕\n");
    QByteArray bytes;
    bytes.append('\xFF');
    bytes.append('\xFE');
    const char16_t *units = reinterpret_cast<const char16_t *>(text.utf16());
    for (int i = 0; i < text.size(); ++i) {
        bytes.append(char(units[i] & 0xFF));
        bytes.append(char((units[i] >> 8) & 0xFF));
    }
    const SrtParseResult result = parseSrt(bytes);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    ASSERT_EQ(result.cues.size(), 1);
    EXPECT_EQ(result.cues.first().text, QStringLiteral("中文字幕"));
}

TEST(Srt, ReportsBadTimecodeWithCueIndex)
{
    const SrtParseResult result = parseSrt("1\nnot a timecode\nhello\n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("第 1 条")));
    EXPECT_TRUE(result.error.contains(QStringLiteral("时间码")));
}

TEST(Srt, RejectsInvertedRanges)
{
    const SrtParseResult result = parseSrt(
        "1\n00:00:05,000 --> 00:00:02,000\nhello\n");
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("结束时间")));
}

TEST(Srt, ExportsUtf8BomCrLfAndRoundTrips)
{
    QList<SubtitleCue> cues;
    SubtitleCue first;
    first.id = QStringLiteral("b");
    first.startUs = 2'000'000;
    first.endUs = 3'000'000;
    first.text = QStringLiteral("第二条");
    SubtitleCue second;
    second.id = QStringLiteral("a");
    second.startUs = 0;
    second.endUs = 1'000'000;
    second.text = QStringLiteral("第一条\n换行");
    cues = {first, second};

    const QByteArray bytes = writeSrt(cues);
    ASSERT_TRUE(bytes.startsWith("\xEF\xBB\xBF"));
    EXPECT_TRUE(bytes.contains("00:00:00,000 --> 00:00:01,000\r\n第一条"));
    EXPECT_TRUE(bytes.contains("\r\n\r\n"));
    EXPECT_TRUE(bytes.endsWith("\r\n"));

    const SrtParseResult parsed = parseSrt(bytes);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toStdString();
    ASSERT_EQ(parsed.cues.size(), 2);
    EXPECT_EQ(parsed.cues.at(0).text, QStringLiteral("第一条\n换行"));
    EXPECT_EQ(parsed.cues.at(1).text, QStringLiteral("第二条"));
}

TEST(Srt, SkipsDisabledCuesOnExport)
{
    QList<SubtitleCue> cues;
    SubtitleCue enabled;
    enabled.id = QStringLiteral("a");
    enabled.startUs = 0;
    enabled.endUs = 1'000'000;
    enabled.text = QStringLiteral("显示");
    SubtitleCue disabled = enabled;
    disabled.id = QStringLiteral("b");
    disabled.text = QStringLiteral("隐藏");
    disabled.enabled = false;
    cues = {enabled, disabled};

    const SrtParseResult parsed = parseSrt(writeSrt(cues));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.cues.size(), 1);
    EXPECT_EQ(parsed.cues.first().text, QStringLiteral("显示"));
}

TEST(Srt, FormatsTime)
{
    EXPECT_EQ(formatSrtTime(3'661'234'000), QStringLiteral("01:01:01,234"));
    EXPECT_EQ(formatSrtTime(-1), QStringLiteral("00:00:00,000"));
}
```

`tests/core/SubtitleEditorTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "ProjectDocument.h"
#include "Srt.h"
#include "TimelineEditor.h"

using namespace VideoEditor;

namespace {

QString subtitleTrackId()
{
    return QStringLiteral("track-s1");
}

} // namespace

TEST(SubtitleEditor, AddsUpdatesAndRemovesCuesUndoably)
{
    ProjectDocument document;
    TimelineEditor editor(document);

    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 0;
    cue.endUs = 2'000'000;
    cue.text = QStringLiteral("你好");
    ASSERT_EQ(editor.addSubtitleCue(subtitleTrackId(), cue), QStringLiteral("cue-1"));
    ASSERT_EQ(document.project().tracks.at(2).cues.size(), 1);

    SubtitleCue updated = cue;
    updated.text = QStringLiteral("你好，世界");
    updated.positionYPermille = 800;
    editor.updateSubtitleCue(subtitleTrackId(), cue.id, updated);
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("你好，世界"));

    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text, QStringLiteral("你好"));
    document.redo();
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("你好，世界"));

    editor.removeSubtitleCue(subtitleTrackId(), cue.id);
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());
    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.size(), 1);
}

TEST(SubtitleEditor, AppliesStyleToSingleCueOrAll)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    for (int i = 0; i < 2; ++i) {
        SubtitleCue cue;
        cue.id = QStringLiteral("cue-%1").arg(i);
        cue.startUs = i * 1'000'000;
        cue.endUs = cue.startUs + 900'000;
        cue.text = QStringLiteral("字幕 %1").arg(i);
        editor.addSubtitleCue(subtitleTrackId(), cue);
    }

    document.project().subtitleStyles = builtinSubtitleStyles();
    editor.applySubtitleStyle(subtitleTrackId(), QStringLiteral("boxed"),
                              QStringLiteral("cue-1"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(0).styleId,
              QStringLiteral("default"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(1).styleId,
              QStringLiteral("boxed"));

    editor.applySubtitleStyle(subtitleTrackId(), QStringLiteral("bold-outline"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(0).styleId,
              QStringLiteral("bold-outline"));
    EXPECT_EQ(document.project().tracks.at(2).cues.at(1).styleId,
              QStringLiteral("bold-outline"));

    document.undo();
    EXPECT_EQ(document.project().tracks.at(2).cues.at(0).styleId,
              QStringLiteral("default"));
}

TEST(SubtitleEditor, ImportsSrtAsSingleUndoStep)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    const QByteArray srt =
        "1\n00:00:00,000 --> 00:00:01,000\n第一句\n\n"
        "2\n00:00:01,500 --> 00:00:02,000\n第二句\n";
    QString error;
    EXPECT_EQ(editor.importSrt(subtitleTrackId(), srt, &error), 2)
        << error.toStdString();
    EXPECT_EQ(document.project().tracks.at(2).cues.size(), 2);

    document.undo();
    EXPECT_TRUE(document.project().tracks.at(2).cues.isEmpty());

    document.redo();
    const QByteArray exported = editor.exportSrt(subtitleTrackId());
    EXPECT_TRUE(exported.contains(QStringLiteral("第二句").toUtf8()));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "Srt|SubtitleEditor" --output-on-failure
```

预期：编译失败，找不到 `Srt.h`。

- [ ] **Step 3: 实现 SRT 编解码**

`src/core/Srt.h`：

```cpp
#pragma once

#include <cstdint>
#include <QByteArray>
#include <QList>
#include <QString>

#include "Timeline.h"

namespace VideoEditor {

struct SrtParseResult {
    QList<SubtitleCue> cues;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

QString decodeTextFile(const QByteArray &bytes);
SrtParseResult parseSrt(const QByteArray &bytes,
                        const QString &defaultStyleId = QStringLiteral("default"));
QByteArray writeSrt(const QList<SubtitleCue> &cues);
bool parseSrtTime(const QString &text, int64_t *startUs, int64_t *endUs);
QString formatSrtTime(int64_t us);

} // namespace VideoEditor
```

`src/core/Srt.cpp`：

```cpp
#include "Srt.h"

#include <algorithm>
#include <string>

#include <QRegularExpression>
#include <QStringConverter>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace VideoEditor {

namespace {

QString decodeUtf16(const QByteArray &bytes, bool bigEndian)
{
    const int units = (bytes.size() - 2) / 2;
    QByteArray swapped;
    const char *data = bytes.constData() + 2;
    if (bigEndian) {
        swapped.resize(units * 2);
        for (int i = 0; i < units * 2; i += 2) {
            swapped[i] = bytes.constData()[2 + i + 1];
            swapped[i + 1] = bytes.constData()[2 + i];
        }
        data = swapped.constData();
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(data), units);
}

int parseMilliseconds(const QString &digits)
{
    QString padded = digits;
    while (padded.size() < 3)
        padded.append(QLatin1Char('0'));
    return padded.left(3).toInt();
}

bool parseTimeToken(const QRegularExpressionMatch &match, int base, int64_t *us)
{
    const int64_t hours = match.captured(base + 0).toLongLong();
    const int64_t minutes = match.captured(base + 1).toLongLong();
    const int64_t seconds = match.captured(base + 2).toLongLong();
    const int ms = parseMilliseconds(match.captured(base + 3));
    if (hours > 99 || minutes > 59 || seconds > 59)
        return false;
    *us = ((hours * 3600 + minutes * 60 + seconds) * 1000 + ms) * 1000;
    return true;
}

} // namespace

QString decodeTextFile(const QByteArray &bytes)
{
    if (bytes.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(bytes.mid(3));
    if (bytes.startsWith("\xFF\xFE"))
        return decodeUtf16(bytes, false);
    if (bytes.startsWith("\xFE\xFF"))
        return decodeUtf16(bytes, true);

    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8.decode(bytes);
    if (!utf8.hasError())
        return text;

#ifdef _WIN32
    const int wideLength = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
                                               bytes.constData(), int(bytes.size()),
                                               nullptr, 0);
    if (wideLength > 0) {
        std::wstring wide(size_t(wideLength), L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.constData(), int(bytes.size()),
                            wide.data(), wideLength);
        return QString::fromStdWString(wide);
    }
#endif
    return QString::fromUtf8(bytes);
}

bool parseSrtTime(const QString &text, int64_t *startUs, int64_t *endUs)
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*$)"));
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch())
        return false;
    return parseTimeToken(match, 1, startUs) && parseTimeToken(match, 5, endUs);
}

SrtParseResult parseSrt(const QByteArray &bytes, const QString &defaultStyleId)
{
    SrtParseResult result;
    QString content = decodeTextFile(bytes);
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    content.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const QStringList blocks = content.split(QRegularExpression(
        QStringLiteral("\n[ \t]*\n")), Qt::SkipEmptyParts);
    int cueIndex = 0;
    for (const QString &rawBlock : blocks) {
        QString block = rawBlock;
        while (block.startsWith(QLatin1Char('\n')))
            block.remove(0, 1);
        while (block.endsWith(QLatin1Char('\n')))
            block.chop(1);
        if (block.trimmed().isEmpty())
            continue;
        ++cueIndex;

        const QStringList lines = block.split(QLatin1Char('\n'));
        int timeLine = 0;
        if (!lines.first().contains(QStringLiteral("-->")))
            timeLine = 1;
        if (timeLine >= lines.size()) {
            result.error = QStringLiteral("第 %1 条字幕缺少时间码。").arg(cueIndex);
            return result;
        }

        int64_t startUs = 0;
        int64_t endUs = 0;
        if (!parseSrtTime(lines.at(timeLine), &startUs, &endUs)) {
            result.error = QStringLiteral("第 %1 条字幕时间码无效：%2")
                               .arg(cueIndex)
                               .arg(lines.at(timeLine).trimmed());
            return result;
        }
        if (endUs <= startUs) {
            result.error = QStringLiteral("第 %1 条字幕的结束时间早于或等于开始时间。")
                               .arg(cueIndex);
            return result;
        }

        QStringList textLines = lines.mid(timeLine + 1);
        if (textLines.isEmpty()) {
            result.error = QStringLiteral("第 %1 条字幕没有文本。").arg(cueIndex);
            return result;
        }
        SubtitleCue cue;
        cue.id = QStringLiteral("cue-%1").arg(cueIndex);
        cue.startUs = startUs;
        cue.endUs = endUs;
        cue.text = textLines.join(QLatin1Char('\n'));
        cue.styleId = defaultStyleId;
        result.cues.append(cue);
    }

    if (result.cues.isEmpty() && result.error.isEmpty())
        result.error = QStringLiteral("SRT 文件中没有可导入的字幕。");
    return result;
}

QString formatSrtTime(int64_t us)
{
    us = qMax<int64_t>(0, us);
    const int64_t totalMs = us / 1000;
    const int64_t ms = totalMs % 1000;
    const int64_t totalSeconds = totalMs / 1000;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

QByteArray writeSrt(const QList<SubtitleCue> &cues)
{
    QList<SubtitleCue> sorted;
    for (const SubtitleCue &cue : cues) {
        if (cue.enabled)
            sorted.append(cue);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const SubtitleCue &a, const SubtitleCue &b) {
                         return a.startUs < b.startUs;
                     });

    QString output;
    int index = 1;
    for (const SubtitleCue &cue : sorted) {
        output += QStringLiteral("%1\r\n%2 --> %3\r\n%4\r\n\r\n")
                      .arg(index++)
                      .arg(formatSrtTime(cue.startUs), formatSrtTime(cue.endUs),
                           cue.text);
    }
    QByteArray bytes("\xEF\xBB\xBF", 3);
    bytes += output.toUtf8();
    return bytes;
}

} // namespace VideoEditor
```

- [ ] **Step 4: 扩展 TimelineEditor 的字幕命令**

`TimelineEditor.h` 追加：

```cpp
    QString addSubtitleCue(const QString &trackId, SubtitleCue cue);
    void updateSubtitleCue(const QString &trackId, const QString &cueId,
                           const SubtitleCue &cue);
    void removeSubtitleCue(const QString &trackId, const QString &cueId);
    void applySubtitleStyle(const QString &trackId, const QString &styleId,
                            const QString &cueId = {});
    int importSrt(const QString &trackId, const QByteArray &bytes,
                  QString *error = nullptr);
    QByteArray exportSrt(const QString &trackId) const;
private:
    QList<TrackEdit> editsForCues(
        const QString &trackId,
        const std::function<void(QList<SubtitleCue> &)> &mutate) const;
```

`TimelineEditor.cpp` 实现：

```cpp
QList<TrackEdit> TimelineEditor::editsForCues(
    const QString &trackId,
    const std::function<void(QList<SubtitleCue> &)> &mutate) const
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle || track->locked)
        return {};
    TrackEdit edit;
    edit.trackId = trackId;
    edit.beforeClips = track->clips;
    edit.afterClips = track->clips;
    edit.beforeCues = track->cues;
    edit.afterCues = track->cues;
    mutate(edit.afterCues);
    return {edit};
}

QString TimelineEditor::addSubtitleCue(const QString &trackId, SubtitleCue cue)
{
    if (cue.id.isEmpty())
        cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (cue.endUs <= cue.startUs || cue.startUs < 0)
        return {};
    cue.positionXPermille = qBound(0, cue.positionXPermille, 1000);
    cue.positionYPermille = qBound(0, cue.positionYPermille, 1000);
    if (cue.styleId.isEmpty())
        cue.styleId = QStringLiteral("default");
    const QString id = cue.id;
    const QList<TrackEdit> edits =
        editsForCues(trackId, [cue](QList<SubtitleCue> &cues) {
            int index = cues.size();
            for (int i = 0; i < cues.size(); ++i) {
                if (cues.at(i).startUs > cue.startUs) {
                    index = i;
                    break;
                }
            }
            cues.insert(index, cue);
        });
    if (edits.isEmpty())
        return {};
    pushEdits(QStringLiteral("添加字幕"), edits);
    return id;
}

void TimelineEditor::updateSubtitleCue(const QString &trackId,
                                       const QString &cueId,
                                       const SubtitleCue &cue)
{
    if (cue.endUs <= cue.startUs)
        return;
    pushEdits(QStringLiteral("编辑字幕"),
              editsForCues(trackId, [cueId, cue](QList<SubtitleCue> &cues) {
                  for (SubtitleCue &existing : cues) {
                      if (existing.id == cueId) {
                          SubtitleCue updated = cue;
                          updated.id = cueId;
                          existing = updated;
                          return;
                      }
                  }
              }));
}

void TimelineEditor::removeSubtitleCue(const QString &trackId,
                                       const QString &cueId)
{
    pushEdits(QStringLiteral("删除字幕"),
              editsForCues(trackId, [cueId](QList<SubtitleCue> &cues) {
                  for (int i = 0; i < cues.size(); ++i) {
                      if (cues.at(i).id == cueId) {
                          cues.removeAt(i);
                          return;
                      }
                  }
              }));
}

void TimelineEditor::applySubtitleStyle(const QString &trackId,
                                        const QString &styleId,
                                        const QString &cueId)
{
    pushEdits(QStringLiteral("应用字幕样式"),
              editsForCues(trackId, [styleId, cueId](QList<SubtitleCue> &cues) {
                  for (SubtitleCue &cue : cues) {
                      if (cueId.isEmpty() || cue.id == cueId)
                          cue.styleId = styleId;
                  }
              }));
}

int TimelineEditor::importSrt(const QString &trackId, const QByteArray &bytes,
                              QString *error)
{
    const SrtParseResult parsed = parseSrt(bytes);
    if (!parsed.ok()) {
        if (error)
            *error = parsed.error;
        return 0;
    }
    QList<SubtitleCue> imported;
    QSet<QString> usedIds;
    for (SubtitleCue cue : parsed.cues) {
        cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        while (usedIds.contains(cue.id))
            cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        usedIds.insert(cue.id);
        imported.append(cue);
    }
    const QList<TrackEdit> edits =
        editsForCues(trackId, [imported](QList<SubtitleCue> &cues) {
            cues.append(imported);
            std::stable_sort(cues.begin(), cues.end(),
                             [](const SubtitleCue &a, const SubtitleCue &b) {
                                 return a.startUs < b.startUs;
                             });
        });
    if (edits.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有可用的字幕轨。");
        return 0;
    }
    pushEdits(QStringLiteral("导入 SRT"), edits);
    return int(imported.size());
}

QByteArray TimelineEditor::exportSrt(const QString &trackId) const
{
    const Track *track = m_document.trackById(trackId);
    if (!track || track->type != TrackType::Subtitle)
        return {};
    return writeSrt(track->cues);
}
```

`Srt.h` 已被 `TimelineEditor.cpp` 包含。

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "Srt|SubtitleEditor|TimelineEditor" --output-on-failure
```

预期：全部 PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core tests/core tests/CMakeLists.txt
git commit -m "feat(core): add subtitle editing, style presets and SRT import/export"
```

---

### Task 6: 增量时间线模型与编辑控制器

**Files:**
- Create: `src/ui/TimelineModel.h`
- Create: `src/ui/TimelineModel.cpp`
- Create: `src/ui/TrackListModel.h`
- Create: `src/ui/TrackListModel.cpp`
- Create: `src/ui/TimelineController.h`
- Create: `src/ui/TimelineController.cpp`
- Create: `tests/ui/TimelineModelTest.cpp`
- Create: `tests/ui/TimelineControllerTest.cpp`
- Create: `tests/TestGuiMain.cpp`
- Modify: `src/ui/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ProjectDocument`（`timelineChanged/tracksChanged/project()`）、`TimelineEditor`、`TimelineEvaluator::durationUs`、`formatTimecodeAtRate`、`framesToMicros`、`MediaProbe`（导入后无需）、`QUuid`。
- Produces:
  - `TimelineModel` 角色：`KindRole("clip"|"cue")`、`ItemIdRole`、`TrackIdRole`、`TrackTypeRole`、`TrackIndexRole`、`StartUsRole`、`DurationUsRole`、`AssetIdRole`、`NameRole`、`SpeedMilliRole`、`EnabledRole`、`LinkedRole`、`VolumeMilliRole`、`PanMilliRole`、`FadeInUsRole`、`FadeOutUsRole`、`TextRole`、`StyleIdRole`、`SelectedRole`。`roleNames()` 必须映射为 QML 使用的名字：`kind/itemId/trackId/trackType/trackIndex/startUs/durationUs/assetId/name/speedMilli/enabled/linked/volumeMilli/panMilli/fadeInUs/fadeOutUs/text/styleId/selected`。
  - `TimelineModel::setProject(const Project&)`、`selectItem(const QString &id)`、`selectedItemId()`。
  - `TrackListModel` 角色：`TrackIdRole`、`NameRole`、`TypeRole`、`TypeLabelRole`、`EnabledRole`、`MutedRole`、`SoloRole`、`LockedRole`、`IndexRole`；`roleNames()` 映射为 `trackId/name/type/typeLabel/enabled/muted/solo/locked/index`。
  - `TimelineController` 属性：`timelineModel`、`trackModel`、`positionUs`、`durationUs`、`timecode`、`pixelsPerSecond`、`snapEnabled`、`selectedClipId`、`selectedCueId`；方法：`seekUs`、`stepFrames`、`moveClip`、`trimStart`、`trimEnd`、`splitAtPlayhead`、`deleteSelected`、`copySelected`、`pasteAtPlayhead`、`snapUs`、`addAssetToTimeline`、`setClipSpeed`、`setClipAudio`、`setClipTransform`、`selectClip`、`selectCue`、`setCueText`、`addCueAtPlayhead`、`deleteSelectedCue`、`applySubtitleStyle`、`importSrtFile`、`exportSrtFile`、`addTrack`、`removeTrack`、`moveTrackUp`、`moveTrackDown`。

- [ ] **Step 1: 写失败测试**

`tests/TestGuiMain.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QGuiApplication>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VideoEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VideoEditorTests"));

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

`tests/ui/TimelineModelTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QSignalSpy>

#include "ProjectDocument.h"
#include "TimelineEditor.h"
#include "TimelineModel.h"
#include "TrackListModel.h"

using namespace VideoEditor;

namespace {

Asset asset(const QString &id, int64_t durationUs, MediaKind kind)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/%1.mp4").arg(id);
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = kind == MediaKind::Video ? 1920 : 0;
    info.height = kind == MediaKind::Video ? 1080 : 0;
    info.frameRate = Rational{30, 1};
    info.durationUs = durationUs;
    info.hasAudio = true;
    info.hasVideo = kind == MediaKind::Video;
    info.kind = kind;
    info.supported = true;
    return Asset{id, info.path, info, {}};
}

Clip clip(const QString &id, int64_t startUs)
{
    Clip value;
    value.id = id;
    value.assetId = QStringLiteral("asset-v");
    value.timelineStartUs = startUs;
    value.sourceInUs = 0;
    value.sourceOutUs = 2'000'000;
    return value;
}

} // namespace

TEST(TimelineModel, UpdatesIncrementallyWithoutReset)
{
    ProjectDocument document;
    document.addAsset(asset(QStringLiteral("asset-v"), 10'000'000, MediaKind::Video));
    TimelineEditor editor(document);
    TimelineModel model;
    TrackListModel tracks;
    model.setProject(document.project());
    tracks.setProject(document.project());
    QAbstractItemModelTester modelTester(&model);
    QAbstractItemModelTester trackTester(&tracks);

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);

    QObject::connect(&document, &ProjectDocument::timelineChanged,
                     &model, [&] { model.setProject(document.project()); });
    QObject::connect(&document, &ProjectDocument::tracksChanged,
                     &tracks, [&] { tracks.setProject(document.project()); });

    editor.addClip(QStringLiteral("track-v1"), clip(QStringLiteral("c1"), 0));
    EXPECT_EQ(insertSpy.count(), 1);
    EXPECT_EQ(model.rowCount(), 1);

    editor.moveClip(QStringLiteral("track-v1"), QStringLiteral("c1"), 1'000'000);
    EXPECT_GE(dataSpy.count(), 1);
    EXPECT_EQ(model.data(model.index(0, 0), TimelineModel::StartUsRole).toLongLong(),
              1'000'000);

    editor.deleteClip(QStringLiteral("track-v1"), QStringLiteral("c1"), false);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(resetSpy.count(), 0);
}

TEST(TimelineModel, ExposesClipsAndCuesWithTrackIndex)
{
    ProjectDocument document;
    TimelineEditor editor(document);
    editor.addClip(QStringLiteral("track-v1"), clip(QStringLiteral("c1"), 0));
    SubtitleCue cue;
    cue.id = QStringLiteral("cue-1");
    cue.startUs = 0;
    cue.endUs = 1'000'000;
    cue.text = QStringLiteral("字幕");
    editor.addSubtitleCue(QStringLiteral("track-s1"), cue);

    TimelineModel model;
    model.setProject(document.project());
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.data(model.index(0, 0), TimelineModel::KindRole).toString(),
              QStringLiteral("clip"));
    EXPECT_EQ(model.data(model.index(1, 0), TimelineModel::KindRole).toString(),
              QStringLiteral("cue"));
    EXPECT_EQ(model.data(model.index(1, 0), TimelineModel::TrackIndexRole).toInt(), 2);
    EXPECT_EQ(model.data(model.index(1, 0), TimelineModel::TextRole).toString(),
              QStringLiteral("字幕"));
}

TEST(TrackListModel, ReconcilesTrackReorder)
{
    ProjectDocument document;
    TrackListModel model;
    model.setProject(document.project());
    QAbstractItemModelTester tester(&model);
    ASSERT_EQ(model.rowCount(), 3);

    Track extra;
    extra.id = QStringLiteral("track-v2");
    extra.type = TrackType::Video;
    extra.name = QStringLiteral("V2");
    TimelineEditor editor(document);
    editor.addTrack(extra);
    model.setProject(document.project());
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.data(model.index(3, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v2"));

    editor.moveTrack(3, 0);
    model.setProject(document.project());
    EXPECT_EQ(model.data(model.index(0, 0), TrackListModel::TrackIdRole).toString(),
              QStringLiteral("track-v2"));
}
```

`tests/ui/TimelineControllerTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "ProjectDocument.h"
#include "TimelineController.h"
#include "TimelineModel.h"

using namespace VideoEditor;

namespace {

Asset videoAsset(int64_t durationUs)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/a.mp4");
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = durationUs;
    info.hasAudio = true;
    info.hasVideo = true;
    info.kind = MediaKind::Video;
    info.supported = true;
    return Asset{QStringLiteral("asset-v"), info.path, info, {}};
}

} // namespace

TEST(TimelineController, AddsVideoAndLinkedAudioAtPlayhead)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.seekUs(1'000'000);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"),
                                      controller.positionUs());
    ASSERT_FALSE(clipId.isEmpty());
    ASSERT_EQ(document.project().tracks.at(0).clips.size(), 1);
    ASSERT_EQ(document.project().tracks.at(1).clips.size(), 1);
    EXPECT_EQ(document.project().tracks.at(1).clips.first().linkId,
              document.project().tracks.at(0).clips.first().linkId);
    EXPECT_EQ(controller.timelineModel()->rowCount(), 2);
}

TEST(TimelineController, SplitAtPlayheadAndUndo)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                  QStringLiteral("track-v1"), 0);
    controller.seekUs(2'000'000);
    controller.splitAtPlayhead();
    EXPECT_EQ(document.project().tracks.at(0).clips.size(), 2);
    EXPECT_EQ(document.project().tracks.at(1).clips.size(), 2);

    document.undo();
    EXPECT_EQ(document.project().tracks.at(0).clips.size(), 1);
}

TEST(TimelineController, SnapUsUsesPlayheadAndClipEdges)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                  QStringLiteral("track-v1"), 0);
    controller.seekUs(3'000'000);
    EXPECT_EQ(controller.snapUs(3'040'000), 3'000'000);
    EXPECT_EQ(controller.snapUs(1'500'000), 1'500'000);
}

TEST(TimelineController, EditsCueTextAndSpeedThroughCommands)
{
    ProjectDocument document;
    document.addAsset(videoAsset(5'000'000));
    TimelineController controller(document);
    const QString clipId =
        controller.addAssetToTimeline(QStringLiteral("asset-v"),
                                      QStringLiteral("track-v1"), 0);
    controller.setClipSpeed(clipId, 2000, true);
    EXPECT_EQ(document.project().tracks.at(0).clips.first().speedMilli, 2000);
    EXPECT_TRUE(document.project().tracks.at(1).clips.first().preservePitch);

    controller.addCueAtPlayhead();
    ASSERT_EQ(document.project().tracks.at(2).cues.size(), 1);
    const QString cueId = document.project().tracks.at(2).cues.first().id;
    controller.setCueText(cueId, QStringLiteral("新字幕"));
    EXPECT_EQ(document.project().tracks.at(2).cues.first().text,
              QStringLiteral("新字幕"));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "TimelineModel|TimelineController|TrackListModel" --output-on-failure
```

预期：编译失败，找不到 `TimelineModel.h`。

- [ ] **Step 3: 实现 TimelineModel**

`src/ui/TimelineModel.h`：

```cpp
#pragma once

#include <QAbstractListModel>
#include <QHash>

#include "Project.h"

namespace VideoEditor {

class TimelineModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        ItemIdRole,
        TrackIdRole,
        TrackTypeRole,
        TrackIndexRole,
        StartUsRole,
        DurationUsRole,
        AssetIdRole,
        NameRole,
        SpeedMilliRole,
        EnabledRole,
        LinkedRole,
        VolumeMilliRole,
        PanMilliRole,
        FadeInUsRole,
        FadeOutUsRole,
        TextRole,
        StyleIdRole,
        SelectedRole
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setProject(const Project &project);
    void selectItem(const QString &id);
    QString selectedItemId() const { return m_selectedId; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void selectionChanged();

private:
    struct Row {
        QString kind;       // "clip" or "cue"
        QString id;
        QString trackId;
        int trackIndex = 0;
        Clip clip;
        SubtitleCue cue;
        bool operator==(const Row &) const = default;
    };

    static QList<Row> buildRows(const Project &project);
    static QString rowKey(const Row &row);
    int rowForKey(const QString &key) const;

    QList<Row> m_rows;
    QString m_selectedId;
};

} // namespace VideoEditor
```

`src/ui/TimelineModel.cpp` 的 `setProject` 严格按 id 增量更新：

```cpp
void TimelineModel::setProject(const Project &project)
{
    const QList<Row> next = buildRows(project);
    QHash<QString, int> nextByKey;
    for (int i = 0; i < next.size(); ++i)
        nextByKey.insert(rowKey(next.at(i)), i);

    // Remove rows that disappeared, from the end so indexes stay valid.
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (!nextByKey.contains(rowKey(m_rows.at(i)))) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }

    // Existing rows: update in place.
    for (int i = 0; i < next.size(); ++i) {
        const int existing = rowForKey(rowKey(next.at(i)));
        if (existing < 0)
            continue;
        if (!(m_rows.at(existing) == next.at(i))) {
            m_rows[existing] = next.at(i);
            emit dataChanged(index(existing), index(existing));
        }
    }

    // New rows are appended; the QML delegates position themselves from
    // StartUsRole/TrackIndexRole, so row order carries no visual meaning.
    for (const Row &row : next) {
        if (rowForKey(rowKey(row)) >= 0)
            continue;
        const int position = m_rows.size();
        beginInsertRows(QModelIndex(), position, position);
        m_rows.append(row);
        endInsertRows();
    }

    if (!m_selectedId.isEmpty()) {
        bool stillPresent = false;
        for (const Row &row : m_rows) {
            if (row.id == m_selectedId)
                stillPresent = true;
        }
        if (!stillPresent) {
            m_selectedId.clear();
            emit selectionChanged();
        }
    }
}
```

`buildRows` 遍历 `project.tracks`，把每个 `clip`（video/audio 轨）和 `cue`（subtitle 轨）转成 `Row`；`rowKey` 返回 `kind + ":" + id`；`data()` 按角色返回，`StartUsRole` 对 clip 用 `timelineStartUs`、对 cue 用 `startUs`，`DurationUsRole` 用 `timelineDurationUs()`/`endUs-startUs`，`NameRole` 用 `QFileInfo(asset path).fileName()`（通过 `QHash<QString, QString>` 在 `buildRows` 时解析，或返回 assetId；本计划选后者，QML 显示轨道/素材名由控制器补充）。`SelectedRole` 返回 `id == m_selectedId`。

- [ ] **Step 4: 实现 TrackListModel 与 TimelineController**

`TrackListModel` 与 `TimelineModel` 同构：`setProject` 以 track id 为键，增量删除/插入/`dataChanged`，`IndexRole` 返回当前数组位置。角色值见 Interfaces。

`src/ui/TimelineController.h`：

```cpp
#pragma once

#include <cstdint>
#include <QObject>
#include <QString>

#include "ProjectDocument.h"
#include "TimelineEditor.h"

namespace VideoEditor {

class TimelineModel;
class TrackListModel;

class TimelineController : public QObject {
    Q_OBJECT
    Q_PROPERTY(VideoEditor::TimelineModel *timelineModel READ timelineModel CONSTANT)
    Q_PROPERTY(VideoEditor::TrackListModel *trackModel READ trackModel CONSTANT)
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY durationChanged)
    Q_PROPERTY(QString timecode READ timecode NOTIFY positionChanged)
    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond
                   NOTIFY viewChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY viewChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCueId READ selectedCueId NOTIFY selectionChanged)

public:
    explicit TimelineController(ProjectDocument &document, QObject *parent = nullptr);

    TimelineModel *timelineModel() const { return m_timelineModel; }
    TrackListModel *trackModel() const { return m_trackModel; }
    qint64 positionUs() const { return m_positionUs; }
    qint64 durationUs() const;
    QString timecode() const;
    double pixelsPerSecond() const { return m_pixelsPerSecond; }
    void setPixelsPerSecond(double value);
    bool snapEnabled() const { return m_snapEnabled; }
    void setSnapEnabled(bool enabled);
    QString selectedClipId() const { return m_selectedClipId; }
    QString selectedCueId() const { return m_selectedCueId; }

    Q_INVOKABLE void seekUs(qint64 positionUs);
    Q_INVOKABLE void stepFrames(int delta);
    Q_INVOKABLE qint64 snapUs(qint64 timeUs) const;
    Q_INVOKABLE QString addAssetToTimeline(const QString &assetId,
                                          const QString &trackId,
                                          qint64 atUs);
    Q_INVOKABLE void moveClip(const QString &clipId, qint64 newStartUs);
    Q_INVOKABLE void trimStart(const QString &clipId, qint64 newStartUs);
    Q_INVOKABLE void trimEnd(const QString &clipId, qint64 newEndUs);
    Q_INVOKABLE void splitAtPlayhead();
    Q_INVOKABLE void deleteSelected(bool ripple);
    Q_INVOKABLE void copySelected();
    Q_INVOKABLE void pasteAtPlayhead();
    Q_INVOKABLE void setClipSpeed(const QString &clipId, int speedMilli,
                                  bool preservePitch);
    Q_INVOKABLE void setClipVolume(const QString &clipId, int volumeMilli);
    Q_INVOKABLE void setClipPan(const QString &clipId, int panMilli);
    Q_INVOKABLE void setClipFades(const QString &clipId, qint64 fadeInUs,
                                  qint64 fadeOutUs);
    Q_INVOKABLE void setClipTransform(const QString &clipId, int scaleMilli,
                                      int offsetXPermille, int offsetYPermille,
                                      int opacityMilli);
    Q_INVOKABLE void selectClip(const QString &clipId);
    Q_INVOKABLE void selectCue(const QString &cueId);
    Q_INVOKABLE QString addCueAtPlayhead();
    Q_INVOKABLE void setCueText(const QString &cueId, const QString &text);
    Q_INVOKABLE void deleteSelectedCue();
    Q_INVOKABLE void applySubtitleStyle(const QString &styleId,
                                        const QString &cueId = {});
    Q_INVOKABLE QString importSrtFile(const QString &path);
    Q_INVOKABLE bool exportSrtFile(const QString &path);
    Q_INVOKABLE void addTrack(int type);
    Q_INVOKABLE void removeTrack(const QString &trackId);
    Q_INVOKABLE void moveTrackUp(const QString &trackId);
    Q_INVOKABLE void moveTrackDown(const QString &trackId);
    Q_INVOKABLE void setTrackMuted(const QString &trackId, bool muted);
    Q_INVOKABLE void setTrackSolo(const QString &trackId, bool solo);
    Q_INVOKABLE void setTrackLocked(const QString &trackId, bool locked);

signals:
    void positionChanged();
    void durationChanged();
    void viewChanged();
    void selectionChanged();
    void editFailed(QString message);

private:
    Track *trackForClip(const QString &clipId, QString *trackId = nullptr);
    Track *trackForCue(const QString &cueId, QString *trackId = nullptr);
    QList<int64_t> snapCandidates(const QString &excludeClipId = {}) const;

    ProjectDocument &m_document;
    TimelineEditor m_editor;
    TimelineModel *m_timelineModel = nullptr;
    TrackListModel *m_trackModel = nullptr;
    qint64 m_positionUs = 0;
    double m_pixelsPerSecond = 80.0;
    bool m_snapEnabled = true;
    QString m_selectedClipId;
    QString m_selectedCueId;
};

} // namespace VideoEditor
```

实现要点（全部可测试，禁止只写注释）：

- 构造：`m_timelineModel->setProject(document.project());` 连接 `timelineChanged/tracksChanged` 到刷新函数；刷新后发 `durationChanged`，并在选中项消失时清空选择。
- `durationUs` 用 `TimelineEvaluator(m_document.project()).durationUs()`；`timecode` 用 `formatTimecodeAtRate(m_positionUs, fps, project.timeline.dropFrameTimecode)`。
- `snapUs`：仅当 `m_snapEnabled` 时调用 `TimelineEditor::snapTime(timeUs, snapThresholdUs(), candidates)`；阈值 = `llround(8.0 / pixelsPerSecond * 1e6)`（8 像素）。候选 = 0、播放头、工程时长、所有片段起止（排除正在移动的片段）。
- `addAssetToTimeline`：查素材；若 `kind==Video`，创建视频 Clip（`sourceIn=0, sourceOut=info.durationUs`）；若 `asset.info.hasAudio`，在第一条 audio 轨创建同 `linkId` 的音频 Clip（`sourceIn=0, sourceOut=info.durationUs`）。两条片段用**一次** `m_editor.addClips({{videoTrackId, videoClip}, {audioTrackId, audioClip}})` 添加，返回视频 clip id；若没有音频轨则只添加视频。`addClips` 在 Task 4 已定义并只生成一个 `TimelineEditCommand`。
- `moveClip/trim*/splitAtPlayhead/deleteSelected`：通过 `trackForClip` 找到轨道 id 后调用编辑器；`splitAtPlayhead` 对选中片段或播放头下所有轨道片段分割，并且只对**每个链接组一次**调用 `splitClip`（避免同组重复分割）：先收集 `QSet<QString> handledLinks`，空 linkId 用 `trackId+clipId` 作键。
- `setClipVolume/Pan/Fades` 读取当前 `AudioParams`，修改一个字段后调用 `setClipAudio`，命令文本由编辑器统一。`setClipTransform` 同理。
- `addCueAtPlayhead`：默认 2 秒时长（不超过工程时长 + 2s），默认样式 `default`、位置 (500,880)，返回新 cue id；`setCueText` 保持时间/样式不变，只改 text。
- `importSrtFile` 读取文件字节调用 `m_editor.importSrt("track-s1", bytes, &error)`，失败 `emit editFailed(error)` 并返回空；成功返回 cue 数量字符串。
- `exportSrtFile` 写 `QSaveFile`，内容 `m_editor.exportSrt("track-s1")`，失败 `emit editFailed` 并返回 false。
- `addTrack(int type)`：`type` 0/1/2 对应 video/audio/subtitle；名称按现有同类型数量生成 `V2`/`A2`/`S2`；新轨道 id 用 `QUuid`。`removeTrack` 禁止删除最后一条同类轨道（否则字幕/音频无处可放），失败发 `editFailed`。
- `setTrackMuted/Solo/Locked`：复制 `project().tracks`，修改目标字段，用 `TrackListCommand` 一次替换（一个撤销步骤），随后刷新两个模型并发出 `timelineChanged`。
- `TimelineModel::data()` 的 `TrackTypeRole` 返回 `trackTypeToString(track.type)`；`NameRole` 对 clip 返回素材文件名（由 `Project::assetById` 解析），对 cue 返回文本前 20 个字符。

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "TimelineModel|TimelineController|TrackListModel" --output-on-failure
```

预期：PASS；`QAbstractItemModelTester` 不报告信号错误，`modelReset` 计数为 0。

- [ ] **Step 6: 提交**

```bash
git add src/ui tests/ui tests/TestGuiMain.cpp tests/CMakeLists.txt
git commit -m "feat(ui): add incremental timeline models and editing controller"
```

---

### Task 7: 音频解码、变速、混音与限幅

**Files:**
- Create: `src/media/audio/AudioBuffer.h`
- Create: `src/media/audio/FfmpegAudioSource.h`
- Create: `src/media/audio/FfmpegAudioSource.cpp`
- Create: `src/media/audio/SpeedProcessor.h`
- Create: `src/media/audio/SpeedProcessor.cpp`
- Create: `src/media/audio/ClipAudioReader.h`
- Create: `src/media/audio/ClipAudioReader.cpp`
- Create: `src/media/audio/TimelineMixer.h`
- Create: `src/media/audio/TimelineMixer.cpp`
- Create: `tests/media/AudioSpeedTest.cpp`
- Create: `tests/media/AudioMixTest.cpp`
- Create: `tests/media/AudioSyncTest.cpp`
- Modify: `src/media/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Project`、`TimelineEvaluator::audioRange`、`sourceTimeAt`、`Asset`、FFmpeg `swresample`/`avutil`。
- Produces:
  - `struct AudioBuffer { std::vector<float> samples; int frames = 0; int channels = 2; int sampleRate = 48000; }`。
  - `class FfmpegAudioSource { static std::unique_ptr<FfmpegAudioSource> open(path, sampleRate, channels, QString*); bool seek(int64_t sourceUs, QString*); bool readRange(int64_t startUs, int frames, float *out, QString*); int64_t durationUs() const; }`。
  - `void resampleLinear(const float *src, int srcFrames, int channels, double step, float *dst, int dstFrames)`。
  - `class OlaTimeStretcher { OlaTimeStretcher(int channels); void reset(); void push(const float*, int frames); int pull(float*, int maxFrames, double speed); int bufferedFrames() const; }`。
  - `class ClipAudioReader { bool open(const Asset&, int sampleRate, int channels, QString*); void render(const Clip &clip, int64_t blockLocalUs, int frames, float *out); void reset(); }`。
  - `float clipGainAt(const Clip &clip, int64_t localTimeUs)`；`void panGains(int panMilli, float *left, float *right)`。
  - `class TimelineMixer { TimelineMixer(int sampleRate = 48000, int channels = 2); void setProject(const Project&); void setPosition(int64_t); int64_t positionUs() const; void render(float *interleaved, int frames, MixStats *stats = nullptr); }`，`MixStats { float peak; int activeLayers; }`。

- [ ] **Step 1: 写失败测试**

`tests/media/AudioSpeedTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "SpeedProcessor.h"

using namespace VideoEditor;

namespace {

std::vector<float> sine(int frames, int channels, double frequency, int sampleRate)
{
    std::vector<float> result(size_t(frames) * channels);
    for (int i = 0; i < frames; ++i) {
        const float value = float(std::sin(2.0 * 3.14159265358979323846
                                           * frequency * i / sampleRate));
        for (int c = 0; c < channels; ++c)
            result[size_t(i) * channels + c] = value;
    }
    return result;
}

int zeroCrossings(const std::vector<float> &samples, int channels)
{
    int crossings = 0;
    float previous = samples.empty() ? 0.0f : samples[0];
    for (size_t i = 1; i < samples.size() / size_t(channels); ++i) {
        const float current = samples[i * size_t(channels)];
        if ((previous <= 0.0f && current > 0.0f)
            || (previous >= 0.0f && current < 0.0f)) {
            ++crossings;
        }
        previous = current;
    }
    return crossings;
}

} // namespace

TEST(AudioSpeed, NaturalSpeedChangesPitchAndDuration)
{
    const int sampleRate = 48000;
    const auto input = sine(sampleRate, 2, 440.0, sampleRate); // 1 s
    std::vector<float> output(size_t(sampleRate / 2) * 2);
    resampleLinear(input.data(), sampleRate, 2, 2.0, output.data(), sampleRate / 2);
    // 880 Hz over 0.5 s -> about 880 zero crossings.
    EXPECT_NEAR(zeroCrossings(output, 2), 880, 40);
}

TEST(AudioSpeed, OlaPreservesPitchWhileHalvingDuration)
{
    const int sampleRate = 48000;
    const auto input = sine(sampleRate, 2, 440.0, sampleRate);
    OlaTimeStretcher stretcher(2);
    stretcher.push(input.data(), sampleRate);
    std::vector<float> output(size_t(sampleRate / 2) * 2);
    const int produced = stretcher.pull(output.data(), sampleRate / 2, 2.0);
    output.resize(size_t(produced) * 2);
    ASSERT_GT(produced, sampleRate / 4);
    // 440 Hz over 0.5 s -> about 440 crossings.
    EXPECT_NEAR(zeroCrossings(output, 2), 440, 40);
}

TEST(AudioSpeed, ClipReaderConsumesSourceAtClipSpeed)
{
    const QString path = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                             .filePath(QStringLiteral("tone.wav"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing tone.wav";
    Asset asset{QStringLiteral("a"), path, MediaInfo{}, {}};
    asset.info.kind = MediaKind::Audio;
    asset.info.hasAudio = true;
    asset.info.durationUs = 2'000'000;

    ClipAudioReader reader;
    QString error;
    ASSERT_TRUE(reader.open(asset, 48000, 2, &error)) << error.toStdString();

    Clip clip;
    clip.assetId = QStringLiteral("a");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 2'000'000;
    clip.speedMilli = 2000;
    clip.preservePitch = false;

    std::vector<float> output(size_t(24000) * 2);
    reader.render(clip, 0, 24000, output.data()); // 0.5 s timeline = 1 s source
    double energy = 0.0;
    for (float sample : output)
        energy += double(sample) * sample;
    EXPECT_GT(energy / output.size(), 0.01);
}
```

`tests/media/AudioMixTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "TimelineMixer.h"

using namespace VideoEditor;

namespace {

QString tonePath()
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
        .filePath(QStringLiteral("tone.wav"));
}

Asset toneAsset()
{
    Asset asset;
    asset.id = QStringLiteral("asset-a");
    asset.path = tonePath();
    asset.info.kind = MediaKind::Audio;
    asset.info.hasAudio = true;
    asset.info.hasVideo = false;
    asset.info.audioCodec = QStringLiteral("pcm_s16le");
    asset.info.audioSampleRate = 48000;
    asset.info.audioChannels = 1;
    asset.info.durationUs = 2'000'000;
    asset.info.supported = true;
    return asset;
}

Project projectWithAudio(const QString &clipId, int64_t startUs,
                         int64_t sourceOutUs, int speedMilli = 1000)
{
    Project project;
    project.timeline.fps = Rational{30, 1};
    project.assets.append(toneAsset());
    Track track;
    track.id = QStringLiteral("a1");
    track.type = TrackType::Audio;
    Clip clip;
    clip.id = clipId;
    clip.assetId = QStringLiteral("asset-a");
    clip.timelineStartUs = startUs;
    clip.sourceInUs = 0;
    clip.sourceOutUs = sourceOutUs;
    clip.speedMilli = speedMilli;
    track.clips.append(clip);
    project.tracks.append(track);
    return project;
}

} // namespace

TEST(AudioMix, RendersAudibleStereoAtCenterPan)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);

    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    EXPECT_EQ(stats.activeLayers, 1);
    EXPECT_GT(stats.peak, 0.1f);
    for (int i = 0; i < 4800; i += 97)
        EXPECT_NEAR(out[size_t(i) * 2], out[size_t(i) * 2 + 1], 1e-4f);
    EXPECT_EQ(mixer.positionUs(), 100'000);
}

TEST(AudioMix, RespectsTrackMuteAndClipMute)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].muted = true;
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(4800) * 2);
    mixer.render(out.data(), 4800, nullptr);
    for (float sample : out)
        EXPECT_EQ(sample, 0.0f);

    project.tracks[0].muted = false;
    project.tracks[0].clips[0].audio.muted = true;
    mixer.setProject(project);
    mixer.setPosition(0);
    mixer.render(out.data(), 4800, nullptr);
    for (float sample : out)
        EXPECT_EQ(sample, 0.0f);
}

TEST(AudioMix, SoloOverridesOtherTracks)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    Track second = project.tracks.first();
    second.id = QStringLiteral("a2");
    second.solo = true;
    second.clips[0].id = QStringLiteral("c2");
    project.tracks.append(second);

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    EXPECT_EQ(stats.activeLayers, 1);
}

TEST(AudioMix, FadeInRampsFromSilence)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].clips[0].audio.fadeInUs = 1'000'000;
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(4800) * 2);
    mixer.render(out.data(), 4800, nullptr);
    EXPECT_NEAR(out[0], 0.0f, 1e-5f);
    const float late = std::abs(out[size_t(4700) * 2]);
    const float early = std::abs(out[size_t(100) * 2]);
    EXPECT_GT(late, early);
}

TEST(AudioMix, LimiterPreventsClipping)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    Project project = projectWithAudio(QStringLiteral("c1"), 0, 2'000'000);
    project.tracks[0].clips[0].audio.volumeMilli = 2000;
    Track second = project.tracks.first();
    second.id = QStringLiteral("a2");
    second.clips[0].id = QStringLiteral("c2");
    second.clips[0].audio.volumeMilli = 2000;
    project.tracks.append(second);

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(500'000);
    std::vector<float> out(size_t(4800) * 2);
    MixStats stats;
    mixer.render(out.data(), 4800, &stats);
    for (float sample : out)
        EXPECT_LE(std::abs(sample), 1.0f);
    EXPECT_LE(stats.peak, 0.98f);
}

TEST(AudioMix, InterpolatesVolumeKeyframes)
{
    Clip clip;
    clip.sourceOutUs = 10'000'000;
    clip.audio.volumeMilli = 1000;
    clip.audio.keyframes = {VolumeKeyframe{0, 1000}, VolumeKeyframe{1'000'000, 500}};
    EXPECT_FLOAT_EQ(clipGainAt(clip, 0), 1.0f);
    EXPECT_FLOAT_EQ(clipGainAt(clip, 500'000), 0.75f);
    EXPECT_FLOAT_EQ(clipGainAt(clip, 2'000'000), 0.5f);
}
```

`tests/media/AudioSyncTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "AudioClock.h"
#include "TimelineMixer.h"

using namespace VideoEditor;

TEST(AudioSync, MixerPositionMatchesConsumedFramesWithoutDrift)
{
    Project project; // silent, only exercises the clock arithmetic
    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> block(size_t(480) * 2);
    const int blocksPerHour = 3600 * 100;
    for (int i = 0; i < blocksPerHour; ++i)
        mixer.render(block.data(), 480, nullptr);
    // 480 frames * 100 blocks/s * 3600 s = exactly one hour; integer math must
    // land on 3.6e9 us with zero drift.
    EXPECT_EQ(mixer.positionUs(), 3'600'000'000LL);
}

TEST(AudioSync, ClockMapsToFrameWithinOneFrame)
{
    const Rational fps{30, 1};
    for (int64_t offset : {0LL, 5'000LL, 16'000LL, 33'000LL, 999'000LL}) {
        const int64_t frame = frameIndexForClock(1'000'000 + offset, fps);
        const int64_t frameStartUs = framesToMicros(frame, fps);
        EXPECT_LE(std::llabs(frameStartUs - (1'000'000 + offset)),
                  framesToMicros(1, fps));
    }
}

TEST(AudioSync, BeepFixtureOnsetsStayAligned)
{
    const QString path = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                             .filePath(QStringLiteral("sync.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing sync.mp4";

    Asset asset;
    asset.id = QStringLiteral("sync");
    asset.path = path;
    asset.info.kind = MediaKind::Video;
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.durationUs = 10'000'000;
    asset.info.audioSampleRate = 48000;
    asset.info.supported = true;

    Project project;
    project.assets.append(asset);
    Track track;
    track.id = QStringLiteral("a1");
    track.type = TrackType::Audio;
    Clip clip;
    clip.id = QStringLiteral("c1");
    clip.assetId = asset.id;
    clip.sourceOutUs = 10'000'000;
    track.clips.append(clip);
    project.tracks.append(track);

    TimelineMixer mixer;
    mixer.setProject(project);
    mixer.setPosition(0);
    std::vector<float> out(size_t(48000) * 2);
    mixer.render(out.data(), 48000, nullptr);

    // Find the first sample above 0.2 after 0.9 s; the beep should start at 1.0 s.
    int firstLoud = -1;
    for (int i = 48000 * 9 / 10; i < 48000; ++i) {
        if (std::abs(out[size_t(i) * 2]) > 0.2f) {
            firstLoud = i;
            break;
        }
    }
    ASSERT_GE(firstLoud, 0);
    const int64_t onsetUs = int64_t(firstLoud) * 1'000'000 / 48000;
    const int64_t expectedFrame = frameIndexForClock(1'000'000, Rational{30, 1});
    const int64_t onsetFrame = frameIndexForClock(onsetUs, Rational{30, 1});
    EXPECT_LE(std::llabs(onsetFrame - expectedFrame), 1);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "AudioSpeed|AudioMix|AudioSync" --output-on-failure
```

预期：编译失败，找不到音频头文件。

- [ ] **Step 3: 实现 FFmpeg 音频源**

`src/media/audio/AudioBuffer.h`：

```cpp
#pragma once

#include <cstdint>
#include <vector>

namespace VideoEditor {

struct AudioBuffer {
    std::vector<float> samples;   // interleaved
    int frames = 0;
    int channels = 2;
    int sampleRate = 48000;

    void resize(int frameCount, int channelCount)
    {
        channels = channelCount;
        frames = frameCount;
        samples.assign(size_t(frameCount) * size_t(channelCount), 0.0f);
    }
};

} // namespace VideoEditor
```

`FfmpegAudioSource.cpp` 用 `AVAudioFifo` + `swr_convert`：

```cpp
struct FfmpegAudioSource::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *fifo = nullptr;
    int streamIndex = -1;
    int sampleRate = 48000;
    int channels = 2;
    int64_t durationUs = 0;
    int64_t fifoStartSample = 0;   // source sample index of fifo[0]
    bool drainSent = false;

    ~Impl()
    {
        if (fifo) av_audio_fifo_free(fifo);
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
    }
};

bool FfmpegAudioSource::open(...)
{
    // avformat_open_input -> find_stream_info -> av_find_best_stream(AUDIO)
    // -> avcodec_alloc_context3/parameters_to_context/open2.
    // swr_alloc_set_opts2(&swr, &outLayout(48k, channels, FLT), AV_SAMPLE_FMT_FLT,
    //                     inLayout, inFormat, sampleRate, AV_SAMPLE_FMT_NONE, 0, nullptr)
    // fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, channels, 1);
    // durationUs = av_rescale_q(audio->duration, audio->time_base, AV_TIME_BASE_Q);
}

bool FfmpegAudioSource::seek(int64_t sourceUs, QString *error)
{
    const int64_t timestamp = av_rescale_q(sourceUs, AV_TIME_BASE_Q,
                                           m_impl->format->streams[m_impl->streamIndex]->time_base);
    if (av_seek_frame(m_impl->format, m_impl->streamIndex, timestamp,
                      AVSEEK_FLAG_BACKWARD) < 0) {
        setError(error, QStringLiteral("音频定位失败。"));
        return false;
    }
    avcodec_flush_buffers(m_impl->codec);
    av_audio_fifo_reset(m_impl->fifo);
    m_impl->fifoStartSample = 0;
    m_impl->drainSent = false;
    m_impl->fifoStartSample = sampleIndexForUs(sourceUs, m_impl->sampleRate);
    return true;
}

bool FfmpegAudioSource::decodeMore(QString *error)
{
    // Receive a frame; on EAGAIN read packets, skip non-audio streams.
    // For each decoded frame:
    //   int64_t pts = frame->best_effort_timestamp (fallback frame->pts);
    //   int64_t frameStart = pts == AV_NOPTS_VALUE ? fifoStartSample + fifoFrames
    //                        : av_rescale_q(pts, stream->time_base, AV_TIME_BASE_Q) * sampleRate / 1e6;
    //   If fifo empty, fifoStartSample = frameStart.
    //   swr_convert into a scratch buffer of frame->nb_samples * channels.
    //   av_audio_fifo_write(fifo, scratch, converted);
}

bool FfmpegAudioSource::readRange(int64_t startUs, int frames, float *out, QString *error)
{
    const int64_t startSample = sampleIndexForUs(startUs, m_impl->sampleRate);
    const int64_t fifoFrames = av_audio_fifo_size(m_impl->fifo);
    const int64_t availableEnd = m_impl->fifoStartSample + fifoFrames;
    if (fifoFrames == 0 || startSample < m_impl->fifoStartSample
        || startSample > availableEnd + m_impl->sampleRate) {
        if (!seek(startUs, error))
            return false;
    }
    // Drop samples before startSample from the FIFO.
    while (m_impl->fifoStartSample < startSample && av_audio_fifo_size(m_impl->fifo) > 0) {
        const int drop = int(qMin<int64_t>(startSample - m_impl->fifoStartSample,
                                           av_audio_fifo_size(m_impl->fifo)));
        av_audio_fifo_drain(m_impl->fifo, drop);
        m_impl->fifoStartSample += drop;
    }
    while (av_audio_fifo_size(m_impl->fifo) < frames) {
        if (!decodeMore(error))
            return false;
    }
    return av_audio_fifo_read(m_impl->fifo, out, frames) == frames;
}
```

失败/EOF 行为：`decodeMore` 返回 false 但 `error` 为空表示 EOF；`readRange` 用 `std::fill` 把剩余样本补 0 并返回 true，避免播放到素材末尾时中断混音。

- [ ] **Step 4: 实现变速处理**

`SpeedProcessor.h`：

```cpp
#pragma once

namespace VideoEditor {

void resampleLinear(const float *src, int srcFrames, int channels, double step,
                    float *dst, int dstFrames);

class OlaTimeStretcher {
public:
    explicit OlaTimeStretcher(int channels);
    void reset();
    void push(const float *interleaved, int frames);
    int pull(float *out, int maxFrames, double speed);
    int bufferedFrames() const { return int(m_inputFrames); }
    static constexpr int kWindowFrames = 960; // 20 ms at 48 kHz
    static constexpr int kHopFrames = 480;    // 10 ms

private:
    int m_channels;
    std::vector<float> m_input;
    std::vector<float> m_accumulator;
    int m_inputRead = 0;
    int m_inputFrames = 0;
};

} // namespace VideoEditor
```

`resampleLinear`：对每个输出帧 `pos = i * step`，取 `floor(pos)`/`ceil(pos)` 线性插值，边界复制最后一帧。

`OlaTimeStretcher` 实现：

```cpp
OlaTimeStretcher::OlaTimeStretcher(int channels)
    : m_channels(channels)
    , m_accumulator(size_t(kWindowFrames) * size_t(channels), 0.0f)
{
}

void OlaTimeStretcher::reset()
{
    m_input.clear();
    std::fill(m_accumulator.begin(), m_accumulator.end(), 0.0f);
    m_inputRead = 0;
    m_inputFrames = 0;
}

void OlaTimeStretcher::push(const float *interleaved, int frames)
{
    m_input.insert(m_input.end(), interleaved,
                   interleaved + size_t(frames) * size_t(m_channels));
    m_inputFrames += frames;
}

int OlaTimeStretcher::pull(float *out, int maxFrames, double speed)
{
    static const std::vector<float> window = [] {
        std::vector<float> values(kWindowFrames);
        for (int i = 0; i < kWindowFrames; ++i)
            values[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / (kWindowFrames - 1));
        return values;
    }();

    speed = qBound(0.25, speed, 4.0);
    const int analysisHop = int(std::lround(kHopFrames * speed));
    int produced = 0;

    while (produced + kHopFrames <= maxFrames) {
        if (m_inputRead + kWindowFrames > m_inputFrames)
            break;
        for (int i = 0; i < kWindowFrames; ++i) {
            const float gain = window[size_t(i)];
            for (int c = 0; c < m_channels; ++c) {
                m_accumulator[size_t(i) * m_channels + c] +=
                    m_input[size_t(m_inputRead + i) * m_channels + c] * gain;
            }
        }
        for (int i = 0; i < kHopFrames; ++i) {
            for (int c = 0; c < m_channels; ++c) {
                out[size_t(produced + i) * m_channels + c] =
                    m_accumulator[size_t(i) * m_channels + c];
            }
        }
        produced += kHopFrames;
        m_accumulator.erase(m_accumulator.begin(),
                            m_accumulator.begin() + kHopFrames * m_channels);
        m_accumulator.insert(m_accumulator.end(),
                             size_t(kHopFrames) * m_channels, 0.0f);
        m_inputRead += analysisHop;
    }

    if (m_inputRead > 48000) {
        m_input.erase(m_input.begin(),
                      m_input.begin() + m_inputRead * m_channels);
        m_inputFrames -= m_inputRead;
        m_inputRead = 0;
    }
    return produced;
}
```

`ClipAudioReader::render`：

```cpp
void ClipAudioReader::render(const Clip &clip, int64_t blockLocalUs, int frames,
                             float *out)
{
    std::fill(out, out + frames * m_channels, 0.0f);
    if (clip.speedMilli == 1000) {
        m_source->readRange(clip.sourceInUs + blockLocalUs, frames, out, nullptr);
        return;
    }
    const double speed = clip.speedMilli / 1000.0;
    const int needed = int(std::ceil(frames * speed)) + OlaTimeStretcher::kWindowFrames + 2;
    m_sourceScratch.resize(size_t(needed) * m_channels);
    if (!m_source->readRange(sourceTimeAt(clip, blockLocalUs), needed,
                             m_sourceScratch.data(), nullptr)) {
        return;
    }
    if (!clip.preservePitch) {
        resampleLinear(m_sourceScratch.data(), needed, m_channels, speed, out, frames);
        return;
    }
    m_stretcher->push(m_sourceScratch.data(), needed);
    const int produced = m_stretcher->pull(out, frames, speed);
    // pull() may return fewer frames while the window warms up; the tail stays
    // silent, which is inaudible at 10 ms block boundaries.
    Q_UNUSED(produced);
}
```

`ClipAudioReader` 每个音轨片段持有自己的 `FfmpegAudioSource` 与 `OlaTimeStretcher`；同一素材的多个片段各自独立解码（M2 优先正确性；内存/句柄数量由活动片段数限制）。

- [ ] **Step 5: 实现混音与限幅**

`TimelineMixer.h` 声明 `MixStats`/`clipGainAt`/`panGains`/`TimelineMixer`。`TimelineMixer.cpp`：

```cpp
float clipGainAt(const Clip &clip, int64_t localTimeUs)
{
    if (!clip.enabled || clip.audio.muted)
        return 0.0f;
    float gain = clip.audio.volumeMilli / 1000.0f;

    const QList<VolumeKeyframe> &keyframes = clip.audio.keyframes;
    if (!keyframes.isEmpty()) {
        float keyframeGain = keyframes.first().volumeMilli / 1000.0f;
        if (localTimeUs <= keyframes.first().timeUs) {
            keyframeGain = keyframes.first().volumeMilli / 1000.0f;
        } else if (localTimeUs >= keyframes.last().timeUs) {
            keyframeGain = keyframes.last().volumeMilli / 1000.0f;
        } else {
            for (int i = 1; i < keyframes.size(); ++i) {
                if (localTimeUs <= keyframes.at(i).timeUs) {
                    const VolumeKeyframe &a = keyframes.at(i - 1);
                    const VolumeKeyframe &b = keyframes.at(i);
                    const double t = double(localTimeUs - a.timeUs)
                                     / double(b.timeUs - a.timeUs);
                    keyframeGain = float((a.volumeMilli
                                          + (b.volumeMilli - a.volumeMilli) * t)
                                         / 1000.0);
                    break;
                }
            }
        }
        gain *= keyframeGain;
    }

    const int64_t durationUs = qMax<int64_t>(1, clip.timelineDurationUs());
    if (clip.audio.fadeInUs > 0 && localTimeUs < clip.audio.fadeInUs)
        gain *= float(localTimeUs) / float(clip.audio.fadeInUs);
    const int64_t fadeOutStart = durationUs - clip.audio.fadeOutUs;
    if (clip.audio.fadeOutUs > 0 && localTimeUs > fadeOutStart)
        gain *= float(qMax<int64_t>(0, durationUs - localTimeUs))
                / float(clip.audio.fadeOutUs);
    return qBound(0.0f, gain, 2.0f);
}

void panGains(int panMilli, float *left, float *right)
{
    const float pan = qBound(-1.0f, panMilli / 1000.0f, 1.0f);
    const float angle = (pan + 1.0f) * 3.14159265f / 4.0f;
    *left = std::cos(angle);
    *right = std::sin(angle);
}

void TimelineMixer::render(float *out, int frames, MixStats *stats)
{
    std::fill(out, out + size_t(frames) * m_channels, 0.0f);
    const int64_t blockStartUs = positionForFrameCount(m_framesRendered);
    const int64_t blockEndUs = positionForFrameCount(m_framesRendered + frames);

    const TimelineEvaluator evaluator(m_project);
    QList<AudioLayer> layers = evaluator.audioRange(blockStartUs, blockEndUs);

    bool anySolo = false;
    for (const AudioLayer &layer : layers)
        anySolo = anySolo || layer.trackSolo;
    QList<AudioLayer> audible;
    for (const AudioLayer &layer : layers) {
        if (layer.trackMuted || (anySolo && !layer.trackSolo))
            continue;
        audible.append(layer);
    }

    float blockPeak = 0.0f;
    std::vector<float> scratch(size_t(frames) * m_channels);
    for (const AudioLayer &layer : audible) {
        ClipAudioReader *reader = readerFor(layer.clip.assetId);
        if (!reader)
            continue;
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        reader->render(layer.clip, layer.localTimeUs, frames, scratch.data());

        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
        panGains(layer.clip.audio.panMilli, &leftGain, &rightGain);
        for (int i = 0; i < frames; ++i) {
            const int64_t localUs = layer.localTimeUs
                                    + int64_t(i) * 1'000'000 / m_sampleRate;
            const float gain = clipGainAt(layer.clip, localUs);
            const float left = scratch[size_t(i) * 2] * gain * leftGain;
            const float right = scratch[size_t(i) * 2 + 1] * gain * rightGain;
            out[size_t(i) * 2] += left;
            out[size_t(i) * 2 + 1] += right;
            blockPeak = qMax(blockPeak, qMax(std::abs(left), std::abs(right)));
        }
    }

    // Master limiter: scale the whole block so nothing exceeds 0.98 full scale.
    // The gain is per block (10 ms), short enough to be inaudible for preview
    // and guarantees the "no clipping" requirement.
    if (blockPeak > 0.98f) {
        const float scale = 0.98f / blockPeak;
        for (int i = 0; i < frames * m_channels; ++i)
            out[i] *= scale;
        blockPeak = 0.98f;
    }
    if (stats) {
        stats->peak = blockPeak;
        stats->activeLayers = audible.size();
    }
    m_framesRendered += frames;
}

int64_t TimelineMixer::positionUs() const
{
    return positionForFrameCount(m_framesRendered);
}

int64_t TimelineMixer::positionForFrameCount(int64_t frames) const
{
    return m_startPositionUs + (frames * 1'000'000 + m_sampleRate / 2) / m_sampleRate;
}

void TimelineMixer::setPosition(int64_t positionUs)
{
    m_startPositionUs = qMax<int64_t>(0, positionUs);
    m_framesRendered = 0;
    for (auto &reader : m_readers)
        reader->reset();
}
```

`readerFor(assetId)` 从 `m_project.assets` 查素材并懒创建 `ClipAudioReader`；`setProject` 时清空 readers（避免采样率/路径变化后复用错误解码器）。

- [ ] **Step 6: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "AudioSpeed|AudioMix|AudioSync" --output-on-failure
```

预期：PASS；`sync.mp4` 缺失时该测试 SKIP。

- [ ] **Step 7: 提交**

```bash
git add src/media/audio src/media/CMakeLists.txt tests/media tests/CMakeLists.txt
git commit -m "feat(media): add FFmpeg audio decoding, speed processing and 48 kHz mixing"
```

---

### Task 8: WASAPI 输出、音频主时钟与播放引擎

**Files:**
- Create: `src/media/audio/AudioSink.h`
- Create: `src/media/audio/WasapiOutput.h`
- Create: `src/media/audio/WasapiOutput.cpp`
- Create: `src/media/audio/AudioClock.h`
- Create: `src/media/audio/AudioEngine.h`
- Create: `src/media/audio/AudioEngine.cpp`
- Create: `tests/media/AudioEngineTest.cpp`
- Modify: `src/media/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `TimelineMixer`、`Project`、`TimelineSettings`、`QThread`、WASAPI COM。
- Produces:
  - `class IAudioSink { virtual bool start(QString*) = 0; virtual void stop() = 0; virtual int write(const float*, int frames) = 0; virtual int sampleRate() const = 0; virtual int channels() const = 0; virtual int64_t playedFrames() const = 0; }`。
  - `class NullAudioSink : public IAudioSink`（可 `setPaceRealTime(bool)`，默认 false）。
  - `WasapiOutput::create(int sampleRate, int channels, QString *error) -> std::unique_ptr<WasapiOutput>`。
  - `int64_t frameIndexForClock(int64_t clockUs, Rational fps)`（= `microsToFrames`）。
  - `class AudioClock { void reset(int64_t); void setPosition(int64_t); int64_t positionUs() const; }`（原子）。
  - `class AudioEngine : public QObject`：`setProject(Project)`、`play(int64_t)`、`pause()`、`seek(int64_t)`、`shutdown()`、`positionUs()`、`hasOutputDevice()`、`lastDeviceError()`、`setSinkFactoryForTest(std::function<std::unique_ptr<IAudioSink>(int,int,QString*)>)`；信号 `positionChanged(qint64)`、`deviceFailed(QString)`。

- [ ] **Step 1: 写失败测试**

`tests/media/AudioEngineTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <QTest>

#include "AudioClock.h"
#include "AudioEngine.h"
#include "AudioSink.h"

using namespace VideoEditor;

namespace {

class CountingSink : public IAudioSink {
public:
    explicit CountingSink(int sampleRate, int channels)
        : m_sampleRate(sampleRate), m_channels(channels) {}

    bool start(QString *) override { m_running = true; return true; }
    void stop() override { m_running = false; }
    int write(const float *, int frames) override
    {
        m_written.fetch_add(frames);
        return frames;
    }
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override { return m_written.load(); }
    bool running() const { return m_running; }

private:
    int m_sampleRate;
    int m_channels;
    std::atomic<int64_t> m_written{0};
    std::atomic_bool m_running{false};
};

} // namespace

TEST(AudioEngine, StartsPauseAndSeekWithInjectedSink)
{
    AudioEngine engine;
    engine.setSinkFactoryForTest([](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(new CountingSink(sampleRate, channels));
    });
    ASSERT_TRUE(engine.hasOutputDevice());

    Project project;
    project.timeline.sampleRate = 48000;
    engine.setProject(project);
    engine.play(0);
    QTest::qWait(60);
    const int64_t first = engine.positionUs();
    engine.pause();
    EXPECT_GE(first, 0);
    engine.play(2'000'000);
    QTest::qWait(40);
    EXPECT_GE(engine.positionUs(), 2'000'000);
    engine.shutdown();
}

TEST(AudioEngine, FallsBackWhenDeviceOpenFails)
{
    AudioEngine engine;
    engine.setSinkFactoryForTest([](int, int, QString *error) {
        if (error)
            *error = QStringLiteral("没有可用的音频输出设备。");
        return std::unique_ptr<IAudioSink>();
    });
    EXPECT_FALSE(engine.hasOutputDevice());
    EXPECT_TRUE(engine.lastDeviceError().contains(QStringLiteral("音频")));
    engine.setProject(Project{});
    engine.play(0);          // must not crash or block
    engine.pause();
    engine.shutdown();
}

TEST(AudioClock, ResetAndSetAreMonotonicPerPlayback)
{
    AudioClock clock;
    clock.reset(1'000'000);
    EXPECT_EQ(clock.positionUs(), 1'000'000);
    clock.setPosition(1'500'000);
    EXPECT_EQ(clock.positionUs(), 1'500'000);
    clock.reset(0);
    EXPECT_EQ(clock.positionUs(), 0);
}

TEST(AudioClock, FrameIndexRoundsToNearestFrame)
{
    const Rational fps{30000, 1001};
    EXPECT_EQ(frameIndexForClock(0, fps), 0);
    EXPECT_EQ(frameIndexForClock(33'367, fps), 1);
    EXPECT_EQ(frameIndexForClock(1'001'000, fps), 30);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R AudioEngine --output-on-failure
```

预期：编译失败，找不到 `AudioEngine.h`。

- [ ] **Step 3: 实现 WASAPI 输出**

`WasapiOutput.cpp` 的创建与写循环：

```cpp
std::unique_ptr<WasapiOutput> WasapiOutput::create(int sampleRate, int channels,
                                                   QString *error)
{
    auto output = std::unique_ptr<WasapiOutput>(new WasapiOutput);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        setError(error, QStringLiteral("音频初始化失败（0x%1）。").arg(quint32(hr), 0, 16));
        return nullptr;
    }
    output->m_comInitialized = SUCCEEDED(hr);

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { /* "无法访问音频设备管理器。" */ return nullptr; }
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output->m_device);
    if (FAILED(hr)) { /* "没有可用的默认音频输出设备。" */ return nullptr; }

    ComPtr<IAudioClient> client;
    hr = output->m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(client.GetAddressOf()));
    if (FAILED(hr)) { /* "无法打开音频输出。" */ return nullptr; }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = WORD(channels);
    format.nSamplesPerSec = DWORD(sampleRate);
    format.wBitsPerSample = 32;
    format.nBlockAlign = WORD(channels * 4);
    format.nAvgBytesPerSec = DWORD(sampleRate * channels * 4);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            200'000, 0, &format, nullptr);
    if (FAILED(hr)) {
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.wBitsPerSample = 16; // last resort: 16-bit PCM shared mode
        format.nBlockAlign = WORD(channels * 2);
        format.nAvgBytesPerSec = DWORD(sampleRate * channels * 2);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200'000, 0,
                                &format, nullptr);
    }
    if (FAILED(hr)) { /* "音频设备不支持 48 kHz 浮点输出。" */ return nullptr; }

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    client->GetService(IID_PPV_ARGS(&output->m_render));
    output->m_client = client;
    output->m_bufferFrames = bufferFrames;
    return output;
}

int WasapiOutput::write(const float *samples, int frames)
{
    if (!m_client || !m_render || frames <= 0)
        return 0;
    UINT32 padding = 0;
    if (FAILED(m_client->GetCurrentPadding(&padding)))
        return 0;
    const UINT32 available = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
    const int writable = int(qMin<UINT32>(UINT32(frames), available));
    if (writable <= 0) {
        Sleep(1);
        return 0;
    }
    BYTE *buffer = nullptr;
    if (FAILED(m_render->GetBuffer(writable, &buffer)))
        return 0;
    memcpy(buffer, samples, size_t(writable) * m_channels * sizeof(float));
    m_render->ReleaseBuffer(writable, 0);
    m_writtenFrames.fetch_add(writable);
    return writable;
}

int64_t WasapiOutput::playedFrames() const
{
    UINT32 padding = 0;
    if (m_client && SUCCEEDED(m_client->GetCurrentPadding(&padding)))
        return qMax<int64_t>(0, m_writtenFrames.load() - int64_t(padding));
    return m_writtenFrames.load();
}
```

`start()` 调用 `IAudioClient::Start()`，`stop()` 调用 `Stop()`。析构 `CoUninitialize`（仅当本对象初始化成功）。

`AudioSink.h` 里 `NullAudioSink::write` 直接返回 frames，`playedFrames` 累加；`setPaceRealTime(true)` 时 `Sleep(frames*1000/sampleRate)`。

- [ ] **Step 4: 实现时钟与播放引擎**

`AudioClock.h`：

```cpp
#pragma once

#include <atomic>
#include <cstdint>

#include "TimeBase.h"

namespace VideoEditor {

class AudioClock {
public:
    void reset(int64_t positionUs) { m_positionUs.store(positionUs); }
    void setPosition(int64_t positionUs) { m_positionUs.store(positionUs); }
    int64_t positionUs() const { return m_positionUs.load(); }

private:
    std::atomic<int64_t> m_positionUs{0};
};

inline int64_t frameIndexForClock(int64_t clockUs, Rational fps)
{
    return microsToFrames(qMax<int64_t>(0, clockUs), fps);
}

} // namespace VideoEditor
```

`AudioEngine` 在独立 `QThread` 中运行，拥有 `TimelineMixer`、`IAudioSink` 和 `AudioClock`（时钟也可由 GUI 线程无锁读取）：

```cpp
void AudioEngine::play(int64_t positionUs)
{
    // The engine object lives on the audio thread; play() must return so the
    // thread's event loop can deliver pause()/seek()/shutdown().
    if (m_playing)
        return;
    m_mixer.setPosition(positionUs);
    m_clock->reset(positionUs);
    m_playStartUs = positionUs;
    if (m_sink && !m_sink->start(&m_deviceError)) {
        setDeviceError(m_deviceError);
        return;                       // silent fallback; no playback loop
    }
    m_playing = true;
    m_renderTimer.start(10);          // 10 ms block cadence on the audio thread
    emit positionChanged(m_clock->positionUs());
}

void AudioEngine::onRenderTimer()
{
    if (!m_playing)
        return;
    if (m_pendingSeekUs >= 0) {
        m_sink->stop();
        m_mixer.setPosition(m_pendingSeekUs);
        m_clock->reset(m_pendingSeekUs);
        m_playStartUs = m_pendingSeekUs;
        m_pendingSeekUs = -1;
        m_sink->start(nullptr);
    }

    m_mixer.render(m_block.data(), kBlockFrames, &m_stats);
    int written = 0;
    while (written < kBlockFrames) {
        const int n = m_sink->write(m_block.data() + written * m_channels,
                                    kBlockFrames - written);
        if (n <= 0)
            break;                    // device buffer full; retry next tick
        written += n;
    }
    const int64_t consumedUs = m_sink->playedFrames() * 1'000'000 / m_sampleRate;
    m_clock->setPosition(qMax<int64_t>(0, m_playStartUs + consumedUs));
    if (++m_notifyCounter >= 3) {     // ~30 Hz
        m_notifyCounter = 0;
        emit positionChanged(m_clock->positionUs());
    }
}

void AudioEngine::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_renderTimer.stop();
    if (m_sink)
        m_sink->stop();
}

void AudioEngine::seek(int64_t positionUs)
{
    m_pendingSeekUs.store(qMax<int64_t>(0, positionUs));
}
```

`play()`/`pause()`/`seek()`/`shutdown()`/`setProject()` 都由 GUI 线程通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 投递到音频线程（因此它们必须是 `public slots`）；`positionUs()` 直接读 `AudioClock` 的原子值，供视频定时器使用。`seek()` 只写入原子目标，`onRenderTimer()` 在下一个 10 ms 块处理，避免在设备写入期间重置混音器。`setSinkFactoryForTest()` 立即按工厂创建/替换 sink，并更新 `hasOutputDevice()`；测试在构造后调用它也能生效。

无设备时 `hasOutputDevice()` 返回 false，`lastDeviceError()` 返回中文原因；`play()` 仍设置时钟并按壁钟由预览控制器推进（Task 10）。

- [ ] **Step 5: CMake 链接 ole32**

`src/media/CMakeLists.txt` 的 `target_link_libraries(videoeditor_media ...)` 增加 `ole32 uuid`（PRIVATE）。`tests/media/AudioEngineTest.cpp` 使用 `QTest::qWait`，`media_tests` 增加 `Qt6::Test`（已存在）。

- [ ] **Step 6: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "AudioEngine|AudioClock|AudioSync" --output-on-failure
```

预期：PASS（测试全部使用注入 sink，不依赖本机音频设备）。

- [ ] **Step 7: 提交**

```bash
git add src/media/audio src/media/CMakeLists.txt tests/media tests/CMakeLists.txt
git commit -m "feat(media): add WASAPI output, audio master clock and playback engine"
```

---

### Task 9: 波形生成、缓存与图像提供器

**Files:**
- Create: `src/media/audio/Waveform.h`
- Create: `src/media/audio/Waveform.cpp`
- Create: `src/media/audio/WaveformService.h`
- Create: `src/media/audio/WaveformService.cpp`
- Create: `src/ui/WaveformImageProvider.h`
- Create: `src/ui/WaveformImageProvider.cpp`
- Create: `tests/media/WaveformTest.cpp`
- Create: `tests/ui/WaveformImageProviderTest.cpp`
- Modify: `src/media/CMakeLists.txt`
- Modify: `src/ui/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `FfmpegAudioSource::open/readRange`、`MediaProbe::probe`、`fingerprintFile`、`CacheStore` 风格路径、`QImage`/`QPainter`。
- Produces:
  - `struct WaveformPeaks { static constexpr int kBucketsPerSecond = 100; QList<float> minValues, maxValues; int64_t durationUs; int bucketCount() const; bool isEmpty() const; }`。
  - `WaveformPeaks computeWaveform(const QString &path, QString *error = nullptr)`。
  - `QByteArray encodeWaveform(const WaveformPeaks&)`（魔数 `VEWF`、版本 1、int16 min/max）。
  - `std::optional<WaveformPeaks> decodeWaveform(const QByteArray&)`（校验魔数/版本/长度）。
  - `class WaveformService : public QObject { WaveformService(QString cacheRoot, QObject *parent = nullptr); void request(const QString &assetId, const QString &path); bool hasPeaks(const QString &assetId) const; WaveformPeaks peaks(const QString &assetId) const; QString cacheFilePath(const QString &path) const; signals: void ready(QString assetId); }`。
  - `class WaveformImageProvider : public QQuickImageProvider { WaveformImageProvider(WaveformService *service); QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override; }`，id 格式 `"<assetId>/<widthPx>"`。

- [ ] **Step 1: 写失败测试**

`tests/media/WaveformTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "Waveform.h"
#include "WaveformService.h"

using namespace VideoEditor;

namespace {

QString tonePath()
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
        .filePath(QStringLiteral("tone.wav"));
}

} // namespace

TEST(Waveform, ComputesOneHundredBucketsPerSecond)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    QString error;
    const WaveformPeaks peaks = computeWaveform(tonePath(), &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_GE(peaks.bucketCount(), 199);
    EXPECT_LE(peaks.bucketCount(), 201);
    float loudest = 0.0f;
    for (int i = 50; i < peaks.bucketCount() - 50; ++i)
        loudest = std::max(loudest, peaks.maxValues.at(i));
    EXPECT_GT(loudest, 0.1f);
    EXPECT_LT(peaks.maxValues.at(10), peaks.maxValues.at(100) + 0.5f);
}

TEST(Waveform, EncodesAndDecodesExactly)
{
    WaveformPeaks peaks;
    peaks.durationUs = 2'000'000;
    peaks.minValues = {-1.0f, -0.5f, 0.0f};
    peaks.maxValues = {1.0f, 0.25f, 0.75f};
    const QByteArray bytes = encodeWaveform(peaks);
    const auto decoded = decodeWaveform(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->durationUs, peaks.durationUs);
    ASSERT_EQ(decoded->bucketCount(), 3);
    EXPECT_NEAR(decoded->minValues.at(1), -0.5f, 1.0f / 32767.0f);
    EXPECT_NEAR(decoded->maxValues.at(2), 0.75f, 1.0f / 32767.0f);
}

TEST(Waveform, RejectsCorruptFiles)
{
    EXPECT_FALSE(decodeWaveform(QByteArray("not a waveform")).has_value());
    QByteArray truncated = encodeWaveform(WaveformPeaks{});
    truncated.chop(4);
    EXPECT_FALSE(decodeWaveform(truncated).has_value());
}

TEST(WaveformService, CachesPeaksOnDiskAndReloads)
{
    if (!QFileInfo::exists(tonePath()))
        GTEST_SKIP() << "missing tone.wav";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    {
        WaveformService service(dir.path());
        QSignalSpy ready(&service, &WaveformService::ready);
        service.request(QStringLiteral("asset-a"), tonePath());
        // Spy must exist before request(), otherwise a fast worker can emit
        // ready() before the test starts listening.

        if (!ready.wait(5000))
            GTEST_SKIP() << "waveform worker did not finish";
        EXPECT_TRUE(service.hasPeaks(QStringLiteral("asset-a")));
        EXPECT_GT(service.peaks(QStringLiteral("asset-a")).bucketCount(), 0);
        EXPECT_TRUE(QFileInfo::exists(service.cacheFilePath(tonePath())));
    }

    // A fresh service must load the cached file instead of recomputing.
    WaveformService second(dir.path());
    QSignalSpy ready(&second, &WaveformService::ready);
    second.request(QStringLiteral("asset-a"), tonePath());
    if (!ready.wait(5000))
        GTEST_SKIP() << "waveform worker did not finish";
    EXPECT_GT(second.peaks(QStringLiteral("asset-a")).bucketCount(), 0);
}
```

`tests/ui/WaveformImageProviderTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>

#include "WaveformImageProvider.h"
#include "WaveformService.h"

using namespace VideoEditor;

TEST(WaveformImageProvider, DrawsRequestedSize)
{
    const QString tone = QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR))
                             .filePath(QStringLiteral("tone.wav"));
    if (!QFileInfo::exists(tone))
        GTEST_SKIP() << "missing tone.wav";

    WaveformService service(QDir::tempPath());
    QSignalSpy ready(&service, &WaveformService::ready);
    service.request(QStringLiteral("asset-a"), tone);
    if (!ready.wait(5000))
        GTEST_SKIP() << "waveform worker did not finish";

    WaveformImageProvider provider(&service);
    QSize size;
    const QImage image = provider.requestImage(
        QStringLiteral("asset-a/300"), &size, QSize(300, 56));
    EXPECT_EQ(image.size(), QSize(300, 56));
    bool hasInk = false;
    for (int y = 0; y < image.height() && !hasInk; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                hasInk = true;
                break;
            }
        }
    }
    EXPECT_TRUE(hasInk);
}

TEST(WaveformImageProvider, UnknownAssetIsTransparent)
{
    WaveformService service(QDir::tempPath());
    WaveformImageProvider provider(&service);
    QSize size;
    const QImage image = provider.requestImage(
        QStringLiteral("missing/100"), &size, QSize(100, 40));
    EXPECT_EQ(image.size(), QSize(100, 40));
    EXPECT_EQ(QImage(image).pixelColor(50, 20).alpha(), 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "Waveform" --output-on-failure
```

预期：编译失败，找不到 `Waveform.h`。

- [ ] **Step 3: 实现峰值计算与编码**

`src/media/audio/Waveform.cpp`：

```cpp
WaveformPeaks computeWaveform(const QString &path, QString *error)
{
    WaveformPeaks peaks;
    const auto info = MediaProbe::probe(path, error);
    if (!info)
        return peaks;
    peaks.durationUs = info->durationUs;
    const int buckets = int((peaks.durationUs + 9'999) / 10'000);
    peaks.minValues.assign(buckets, 0.0f);
    peaks.maxValues.assign(buckets, 0.0f);

    auto source = FfmpegAudioSource::open(path, 48000, 2, error);
    if (!source)
        return WaveformPeaks{};

    constexpr int kSamplesPerBucket = 480;   // 10 ms at 48 kHz
    constexpr int kBucketsPerRead = 10;      // 100 ms
    std::vector<float> block(size_t(kSamplesPerBucket) * kBucketsPerRead * 2);
    int bucket = 0;
    while (bucket < buckets) {
        const int64_t startUs = int64_t(bucket) * 10'000;
        if (!source->readRange(startUs, int(block.size() / 2), block.data(), error))
            break;
        const int availableBuckets = qMin(kBucketsPerRead, buckets - bucket);
        for (int b = 0; b < availableBuckets; ++b) {
            float minimum = 0.0f;
            float maximum = 0.0f;
            for (int i = 0; i < kSamplesPerBucket; ++i) {
                const size_t index = (size_t(b) * kSamplesPerBucket + i) * 2;
                const float value = 0.5f * (block[index] + block[index + 1]);
                minimum = qMin(minimum, value);
                maximum = qMax(maximum, value);
            }
            peaks.minValues[bucket] = minimum;
            peaks.maxValues[bucket] = maximum;
            ++bucket;
        }
    }
    return peaks;
}

QByteArray encodeWaveform(const WaveformPeaks &peaks)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("VEWF", 4);
    stream << quint16(1) << qint64(peaks.durationUs) << qint32(peaks.bucketCount());
    for (int i = 0; i < peaks.bucketCount(); ++i) {
        stream << qint16(qRound(qBound(-1.0f, peaks.minValues.at(i), 1.0f) * 32767.0f))
               << qint16(qRound(qBound(-1.0f, peaks.maxValues.at(i), 1.0f) * 32767.0f));
    }
    return bytes;
}

std::optional<WaveformPeaks> decodeWaveform(const QByteArray &bytes)
{
    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::LittleEndian);
    char magic[4] = {};
    quint16 version = 0;
    qint64 durationUs = 0;
    qint32 count = 0;
    if (stream.readRawData(magic, 4) != 4 || memcmp(magic, "VEWF", 4) != 0)
        return std::nullopt;
    stream >> version >> durationUs >> count;
    if (version != 1 || count < 0 || stream.status() != QDataStream::Ok)
        return std::nullopt;
    if (bytes.size() < 4 + 2 + 8 + 4 + count * 4)
        return std::nullopt;
    WaveformPeaks peaks;
    peaks.durationUs = durationUs;
    for (int i = 0; i < count; ++i) {
        qint16 minimum = 0;
        qint16 maximum = 0;
        stream >> minimum >> maximum;
        peaks.minValues.append(minimum / 32767.0f);
        peaks.maxValues.append(maximum / 32767.0f);
    }
    return peaks;
}
```

- [ ] **Step 4: 实现异步服务与图像提供器**

`WaveformService`：把工作放 `QThread`，worker 用 lambda 连接 `request` 信号；缓存路径 `cacheRoot/waveform/<fingerprintFile(path)>.wf`。读取缓存失败或缺失则计算并 `QSaveFile` 原子写入。`peaks()`/`hasPeaks()` 用 `QReadLocker`。

`WaveformImageProvider::requestImage`：

```cpp
QImage WaveformImageProvider::requestImage(const QString &id, QSize *size,
                                           const QSize &requestedSize)
{
    const QStringList parts = id.split(QLatin1Char('/'));
    const int width = qMax(1, requestedSize.width() > 0
                                  ? requestedSize.width()
                                  : (parts.size() > 1 ? parts.at(1).toInt() : 400));
    const int height = qMax(1, requestedSize.height() > 0 ? requestedSize.height() : 56);
    if (size)
        *size = QSize(width, height);

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (!m_service || parts.isEmpty())
        return image;
    const WaveformPeaks peaks = m_service->peaks(parts.first());
    if (peaks.isEmpty())
        return image;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(0x7f, 0xc8, 0xff, 220));
    const double center = height / 2.0;
    for (int x = 0; x < width; ++x) {
        const int from = int(int64_t(x) * peaks.bucketCount() / width);
        const int to = qMax(from + 1, int(int64_t(x + 1) * peaks.bucketCount() / width));
        float minimum = 0.0f;
        float maximum = 0.0f;
        for (int b = from; b < qMin(to, peaks.bucketCount()); ++b) {
            minimum = qMin(minimum, peaks.minValues.at(b));
            maximum = qMax(maximum, peaks.maxValues.at(b));
        }
        const double top = center - maximum * center;
        const double bottom = center - minimum * center;
        painter.drawLine(QPointF(x, top), QPointF(x, bottom));
    }
    return image;
}
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "Waveform" --output-on-failure
```

预期：PASS；缓存测试证明第二次读取走磁盘缓存。

- [ ] **Step 6: 提交**

```bash
git add src/media src/ui src/media/CMakeLists.txt src/ui/CMakeLists.txt tests
git commit -m "feat(media): add waveform peaks, disk cache and QML image provider"
```

---

### Task 10: D3D11 多层合成、字幕光栅化与时间线预览

**Files:**
- Create: `src/media/render/FrameSelector.h`
- Create: `src/media/render/FrameSelector.cpp`
- Create: `src/media/render/SubtitleRasterizer.h`
- Create: `src/media/render/SubtitleRasterizer.cpp`
- Create: `src/media/render/LayerCompositor.h`
- Create: `src/media/render/LayerCompositor.cpp`
- Create: `src/media/render/ClipVideoSource.h`
- Create: `src/media/render/ClipVideoSource.cpp`
- Create: `src/media/render/TimelineRenderWorker.h`
- Create: `src/media/render/TimelineRenderWorker.cpp`
- Create: `src/media/render/LayerGeometry.h`
- Create: `src/media/render/LayerGeometry.cpp`
- Modify: `src/ui/PreviewController.h`
- Modify: `src/ui/PreviewController.cpp`
- Create: `tests/media/FrameSelectorTest.cpp`
- Create: `tests/media/SubtitleRasterizerTest.cpp`
- Create: `tests/media/CompositorTest.cpp`
- Modify: `src/media/CMakeLists.txt`
- Modify: `src/ui/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `TimelineEvaluator::frameAt`、`Project::assetById`、`createVideoDecoder`、`Nv12Converter`、`RgbaConverter`、`SharedTextureRing`、`FrameReadback`、`DecodedFrame`、`D3D11Context`、`SubtitleLayer`、`Transform`、`QImage`。
- Produces:
  - `enum class FrameFetchAction { Reuse, Decode, Seek }`；`struct FrameFetchPlan { FrameFetchAction action; int maxFramesToDrop; }`；`FrameFetchPlan planFrameFetch(int64_t currentPtsUs, int64_t targetUs, int64_t frameDurationUs, int64_t seekThresholdUs = 500'000)`。
  - `struct LayerRect { float x, y, w, h; }`；`LayerRect layerRectFor(const Transform&, int sourceW, int sourceH, int outW, int outH)`。
  - `QImage SubtitleRasterizer::render(const QList<SubtitleLayer>&, int width, int height, const QList<SubtitleStyle> &styles)`；`QString SubtitleRasterizer::cacheKey(const QList<SubtitleLayer>&, int width, int height)`。
  - `class LayerCompositor { bool initialize(D3D11Context&, int width, int height, QString*); void begin(ID3D11RenderTargetView*, bool clearBlack); bool drawFrame(const DecodedFrame&, const LayerRect&, float opacity, QString*); bool drawImage(const QImage&, const LayerRect&, float opacity, QString*); void end(); }`。
  - `class ClipVideoSource { static std::unique_ptr<ClipVideoSource> open(const Asset&, D3D11Context&, const QString &proxyPath, QString*); const DecodedFrame *frameAt(int64_t sourceUs, QString*); int width() const; int height() const; }`。
  - `class TimelineRenderWorker : public QObject`（槽 `setProject(Project, QString, quint64)`、`renderFrame(qint64, quint64)`、`close()`；信号 `frameReady(int, QSize, qint64, quintptr, quintptr, quintptr, quint64)`、`error(QString)`）。
  - `PreviewController` 新增：`Q_PROPERTY(bool timelineMode READ timelineMode NOTIFY mediaChanged)`、`Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY durationChanged)`、`void openTimeline(const Project&, qint64)`、`void setAudioEngine(AudioEngine*)`、`Q_INVOKABLE void seekFrame(qint64)`、信号 `timelineFramePresented(qint64 timelineUs)`（每次时间线帧在场景图呈现后发出，供自检/性能门禁计数）。

- [ ] **Step 1: 写失败测试**

`tests/media/FrameSelectorTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "FrameSelector.h"

using namespace VideoEditor;

TEST(FrameSelector, ReusesFrameInsideItsDisplayWindow)
{
    const FrameFetchPlan plan = planFrameFetch(1'000'000, 1'010'000, 33'333);
    EXPECT_EQ(plan.action, FrameFetchAction::Reuse);
}

TEST(FrameSelector, DecodesForwardAndCountsDrops)
{
    const FrameFetchPlan plan = planFrameFetch(1'000'000, 1'100'000, 33'333);
    EXPECT_EQ(plan.action, FrameFetchAction::Decode);
    EXPECT_EQ(plan.maxFramesToDrop, 2);
    EXPECT_LE(plan.maxFramesToDrop, 3);
}

TEST(FrameSelector, SeeksBackwards)
{
    EXPECT_EQ(planFrameFetch(2'000'000, 1'500'000, 33'333).action,
              FrameFetchAction::Seek);
}

TEST(FrameSelector, SeeksAcrossLargeJumps)
{
    EXPECT_EQ(planFrameFetch(0, 2'000'000, 33'333).action, FrameFetchAction::Seek);
    EXPECT_EQ(planFrameFetch(-1, 0, 33'333).action, FrameFetchAction::Seek);
}
```

`tests/media/SubtitleRasterizerTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QImage>

#include "SubtitleRasterizer.h"

using namespace VideoEditor;

namespace {

SubtitleLayer layer(const QString &text)
{
    SubtitleLayer value;
    value.trackId = QStringLiteral("s1");
    value.cueId = QStringLiteral("cue-1");
    value.text = text;
    value.style = SubtitleStyle{};
    value.positionXPermille = 500;
    value.positionYPermille = 880;
    return value;
}

} // namespace

TEST(SubtitleRasterizer, EmptyListProducesTransparentImage)
{
    const QImage image = SubtitleRasterizer::render({}, 640, 360, {SubtitleStyle{}});
    ASSERT_EQ(image.size(), QSize(640, 360));
    for (int y = 0; y < image.height(); y += 13) {
        for (int x = 0; x < image.width(); x += 13)
            ASSERT_EQ(qAlpha(image.pixel(x, y)), 0);
    }
}

TEST(SubtitleRasterizer, DrawsChineseTextNearBottomCenter)
{
    const QImage image = SubtitleRasterizer::render(
        {layer(QStringLiteral("测试字幕"))}, 640, 360, {SubtitleStyle{}});
    ASSERT_EQ(image.size(), QSize(640, 360));
    int ink = 0;
    int minX = 640;
    int maxX = 0;
    int minY = 360;
    int maxY = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) == 0)
                continue;
            ++ink;
            minX = qMin(minX, x);
            maxX = qMax(maxX, x);
            minY = qMin(minY, y);
            maxY = qMax(maxY, y);
        }
    }
    EXPECT_GT(ink, 50);
    EXPECT_GT(minY, 200);          // lower third
    EXPECT_GT((minX + maxX) / 2, 200);
    EXPECT_LT((minX + maxX) / 2, 440);
}

TEST(SubtitleRasterizer, CacheKeyChangesWithText)
{
    const QString a = SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360);
    const QString b = SubtitleRasterizer::cacheKey({layer(QStringLiteral("B"))}, 640, 360);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, SubtitleRasterizer::cacheKey({layer(QStringLiteral("A"))}, 640, 360));
}
```

`tests/media/CompositorTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include <QImage>

#include "D3D11Context.h"
#include "FrameReadback.h"
#include "LayerCompositor.h"
#include "SharedTextureRing.h"

using namespace VideoEditor;

TEST(Compositor, BlendsTwoImagesAndReadsBack)
{
    QString error;
    auto context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    LayerCompositor compositor;
    ASSERT_TRUE(compositor.initialize(*context, 64, 64, &error)) << error.toStdString();

    QImage red(64, 64, QImage::Format_ARGB32_Premultiplied);
    red.fill(Qt::red);
    QImage blue(64, 64, QImage::Format_ARGB32_Premultiplied);
    blue.fill(QColor(0, 0, 255, 128));

    compositor.begin(ring.rtv(0), true);
    ASSERT_TRUE(compositor.drawImage(red, LayerRect{0, 0, 1, 1}, 1.0f, &error))
        << error.toStdString();
    ASSERT_TRUE(compositor.drawImage(blue, LayerRect{0, 0, 0.5f, 1}, 0.5f, &error))
        << error.toStdString();
    compositor.end();

    const std::vector<uint8_t> pixels =
        readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(pixels.size(), size_t(64 * 64 * 4)) << error.toStdString();
    const auto at = [&pixels](int x, int y) {
        const size_t index = (size_t(y) * 64 + x) * 4;
        return std::array<int, 3>{pixels[index], pixels[index + 1], pixels[index + 2]};
    };
    const auto left = at(8, 32);
    const auto right = at(56, 32);
    EXPECT_GT(left[0], 100);       // red + blue blend
    EXPECT_GT(left[2], 40);
    EXPECT_GT(right[0], 200);      // untouched red
    EXPECT_LT(right[2], 60);
}

TEST(LayerGeometry, FitsAndOffsetsTransform)
{
    const LayerRect fit = layerRectFor(Transform{}, 1920, 1080, 1920, 1080);
    EXPECT_FLOAT_EQ(fit.x, 0.0f);
    EXPECT_FLOAT_EQ(fit.w, 1.0f);

    Transform zoom;
    zoom.scaleMilli = 2000;
    zoom.offsetXPermille = 250;
    const LayerRect zoomed = layerRectFor(zoom, 1920, 1080, 1920, 1080);
    EXPECT_NEAR(zoomed.w, 2.0f, 1e-4f);
    // Centered 2x layer starts at -0.5; a +0.25 frame-width offset moves it to -0.25.
    EXPECT_NEAR(zoomed.x, -0.25f, 1e-4f);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "FrameSelector|SubtitleRasterizer|Compositor|LayerGeometry" --output-on-failure
```

预期：编译失败，找不到新头文件。

- [ ] **Step 3: 实现帧选择与几何**

`FrameSelector.cpp`：

```cpp
FrameFetchPlan planFrameFetch(int64_t currentPtsUs, int64_t targetUs,
                              int64_t frameDurationUs, int64_t seekThresholdUs)
{
    frameDurationUs = qMax<int64_t>(1, frameDurationUs);
    if (currentPtsUs < 0)
        return {FrameFetchAction::Seek, 0};
    if (targetUs < currentPtsUs)
        return {FrameFetchAction::Seek, 0};
    if (targetUs < currentPtsUs + frameDurationUs)
        return {FrameFetchAction::Reuse, 0};
    const int64_t delta = targetUs - currentPtsUs;
    if (delta > seekThresholdUs)
        return {FrameFetchAction::Seek, 0};
    const int drops = int(delta / frameDurationUs);
    return {FrameFetchAction::Decode, qBound(0, drops, 120)};
}
```

`LayerGeometry.cpp`：

```cpp
LayerRect layerRectFor(const Transform &transform, int sourceW, int sourceH,
                       int outW, int outH)
{
    if (sourceW <= 0 || sourceH <= 0 || outW <= 0 || outH <= 0)
        return {0, 0, 1, 1};
    const float fit = qMin(float(outW) / sourceW, float(outH) / sourceH);
    const float scale = fit * (transform.scaleMilli / 1000.0f);
    const float w = sourceW * scale / outW;
    const float h = sourceH * scale / outH;
    const float x = 0.5f * (1.0f - w) + transform.offsetXPermille / 1000.0f;
    const float y = 0.5f * (1.0f - h) + transform.offsetYPermille / 1000.0f;
    return {x, y, w, h};
}
```

`SubtitleRasterizer.cpp`：按安全区（左右 5%、底 5%）计算 `QFont`，`QPainterPath` 画描边，`QPainter::drawText` 居中；多行按 `\n` 分行，行高 `fontMetrics.height()`，整体以 `positionY` 为锚点向上堆叠；背景用圆角矩形填充 `backgroundArgb`。`cacheKey` 由每个 cue 的 id + `qHash(text)` + styleId + 位置 + 尺寸拼接。

- [ ] **Step 4: 实现合成器与片段源**

`LayerCompositor` 初始化：编译两个着色器（顶点按 `dest/uv` 常量缓冲生成四边形；像素采样 `Texture2D<float4>` 乘 opacity），创建 `ID3D11BlendState`（`SrcAlpha`/`InvSrcAlpha`，`D3D11_BLEND_OP_ADD`），创建 `ID3D11SamplerState`（线性、Clamp）。`begin()` 设置 OM 目标和视口，清屏黑（或透明黑），设置混合状态。`end()` 解除绑定。

`drawImage`：把 `QImage::convertToFormat(QImage::Format_RGBA8888)` 的像素 `UpdateSubresource` 到按尺寸缓存的 `ID3D11Texture2D`；创建/复用 SRV；设置常量缓冲后 `Draw(4,0)`。`drawFrame` 对 `DecodedFrame::Kind::CpuRgba` 走同一条上传路径；对已归一化的 RGBA texture 走 SRV 路径（`ClipVideoSource` 保证传入的是 RGBA）。

`ClipVideoSource::open`：

```cpp
auto source = std::unique_ptr<ClipVideoSource>(new ClipVideoSource);
QVariant info = asset.info; // MediaInfo copy
source->m_decoder = createVideoDecoder(path, context, asset.info).decoder;
if (!source->m_decoder) return nullptr;
source->m_frameDurationUs = framesToMicros(
    1, asset.info.frameRate.isValid() ? asset.info.frameRate : Rational{30, 1});
const int rotation = ((asset.info.rotationDegrees % 360) + 360) % 360;
source->m_displaySize = (rotation == 90 || rotation == 270)
                            ? QSize(asset.info.height, asset.info.width)
                            : QSize(asset.info.width, asset.info.height);
// Create converted RGBA texture at display size, RENDER_TARGET | SHADER_RESOURCE.
```

`frameAt`：

```cpp
const DecodedFrame *ClipVideoSource::frameAt(int64_t sourceUs, QString *error)
{
    const FrameFetchPlan plan = planFrameFetch(
        m_hasFrame ? m_lastPtsUs : -1, sourceUs, m_frameDurationUs);
    if (plan.action == FrameFetchAction::Reuse && m_hasFrame)
        return &m_frame;

    if (plan.action == FrameFetchAction::Seek) {
        if (!m_decoder->seek(sourceUs, error))
            return nullptr;
        m_hasFrame = false;
    }
    int drops = plan.action == FrameFetchAction::Decode ? plan.maxFramesToDrop : 0;
    while (true) {
        DecodedFrame frame;
        if (!m_decoder->decodeNext(frame, error))
            return m_hasFrame ? &m_frame : nullptr;
        if (!convertToRgba(frame, error))
            return nullptr;
        m_frame = std::move(frame);
        m_lastPtsUs = m_frame.ptsUs;
        m_hasFrame = true;
        if (m_lastPtsUs + m_frameDurationUs > sourceUs || drops-- <= 0)
            break;
    }
    return &m_frame;
}
```

`convertToRgba` 使用 `Nv12Converter`（硬件纹理）或 `RgbaConverter::convertRgba`（CPU 帧）把帧转到 `m_converted`，旋转角取 `-asset.info.rotationDegrees * pi / 180`；随后 `m_frame.kind = HardwareTexture; m_frame.texture = m_converted.Get(); m_frame.subresource = 0;`，使合成器只需处理一种格式。CPU 帧的 `rgba` 在转换后可释放。

`TimelineRenderWorker::setProject` 创建 `D3D11Context`（`create(true)`，失败回退 `create(false)`）、按工程分辨率创建 `SharedTextureRing`、`LayerCompositor`；`renderFrame`：

```cpp
const FrameComposition frame = TimelineEvaluator(m_project).frameAt(timelineUs);
const int slot = m_ring->nextWriteIndex();
IDXGIKeyedMutex *mutex = m_ring->mutex(slot);
if (mutex->AcquireSync(0, 50) != S_OK)
    return;                       // consumer still holds the slot; skip this frame
m_compositor.begin(m_ring->rtv(slot), true);

for (const VideoLayer &layer : frame.video) {
    const Asset *asset = m_project.assetById(layer.assetId);
    if (!asset)
        continue;
    ClipVideoSource *source = sourceFor(layer);   // keyed by clipId
    const DecodedFrame *decoded = source->frameAt(layer.sourceTimeUs, &error);
    if (!decoded) {
        emit this->error(error);
        continue;
    }
    const LayerRect rect = layerRectFor(layer.transform, source->width(),
                                        source->height(), m_width, m_height);
    m_compositor.drawFrame(*decoded, rect, layer.transform.opacityMilli / 1000.0f,
                           &error);
}
const QImage subtitles = m_subtitleCache.imageFor(frame.subtitles);
if (!subtitles.isNull())
    m_compositor.drawImage(subtitles, LayerRect{0, 0, 1, 1}, 1.0f, &error);
m_compositor.end();
mutex->ReleaseSync(1);
m_ring->advanceWriteIndex();
emit frameReady(slot, QSize(m_width, m_height), timelineUs,
                reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                reinterpret_cast<quintptr>(m_ring->sharedHandle(2)),
                m_generation);
```

字幕缓存：保存 `lastKey`、`lastImage` 与上传后的纹理；仅当 `SubtitleRasterizer::cacheKey` 变化时重新光栅化。

`TimelineRenderWorker.h` 在 `namespace VideoEditor` 之外声明 `Q_DECLARE_METATYPE(VideoEditor::Project)`，`PreviewController` 构造函数中调用 `qRegisterMetaType<VideoEditor::Project>("VideoEditor::Project")`，否则排队调用 `setProject(Project, QString, quint64)` 会在运行时报“无法排队未注册类型”。

- [ ] **Step 5: 扩展 PreviewController 的时间线模式**

`PreviewController.h` 增加：

```cpp
#include "Project.h"

class AudioEngine;
class TimelineRenderWorker;

public:
    enum class Mode { Asset, Timeline };
    Mode mode() const { return m_mode; }
    bool timelineMode() const { return m_mode == Mode::Timeline; }
    qint64 durationUs() const { return m_durationUs; }
    void openTimeline(const Project &project, qint64 durationUs);
    void setAudioEngine(AudioEngine *engine);
    Q_INVOKABLE void seekFrame(qint64 frame);
private:
    void onTimelineFrameReady(int slot, const QSize &size, qint64 ptsUs,
                              quintptr h0, quintptr h1, quintptr h2,
                              quint64 generation);
    void onTimelineTick();
    void requestTimelineFrame(int64_t timelineUs);
    Mode m_mode = Mode::Asset;
    TimelineRenderWorker *m_timelineWorker = nullptr;
    AudioEngine *m_audioEngine = nullptr;
    Project m_timelineProject;
    QTimer m_presentationTimer;
    QElapsedTimer m_wallClock;
    int64_t m_playStartUs = 0;
    int64_t m_lastTimelineFrame = -1;
    Rational m_timelineFps{30, 1};
```

`PreviewController.cpp` 构造函数创建 `m_timelineWorker`、移到独立线程（与解码线程分开，避免互阻塞），连接 `frameReady`、`error`；`m_presentationTimer` 连接 `onTimelineTick`。`openTimeline` 切模式、清空资产预览、把 `Project` 经 `Q_ARG` 发给 worker（`Q_DECLARE_METATYPE(VideoEditor::Project)`）。`play()` 在时间线模式：

```cpp
if (m_mode == Mode::Timeline) {
    if (m_playing) return;
    if (m_audioEngine && m_audioEngine->hasOutputDevice()) {
        // AudioEngine lives on its own thread; never call its slots directly.
        QMetaObject::invokeMethod(m_audioEngine, "setProject",
                                  Qt::QueuedConnection,
                                  Q_ARG(VideoEditor::Project, m_timelineProject));
        QMetaObject::invokeMethod(m_audioEngine, "play", Qt::QueuedConnection,
                                  Q_ARG(qint64, m_positionUs));
    } else {
        m_wallClock.restart();
        m_playStartUs = m_positionUs;
    }
    m_playing = true;
    m_lastTimelineFrame = -1;
    m_presentationTimer.start(qMax(1, int(1000.0 * m_timelineFps.den / m_timelineFps.num)));
    emit playingChanged();
    onTimelineTick();
    return;
}
```

`onTimelineTick` 使用音频时钟（或壁钟），计算帧号、更新 `m_positionUs`，只有帧号变化才请求渲染。`onTimelineFrameReady` 与既有 `onFrameReady` 相同的代际过滤和环形缓冲释放逻辑，但 `rotation = 0`、尺寸为工程分辨率。

`seekMs` 在时间线模式改为 `seekFrame(microsToFrames(ms*1000, fps))`；`seekFrame` 暂停音频、`m_audioEngine->seek(targetUs)`、重置壁钟起点、请求目标帧。

- [ ] **Step 6: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "FrameSelector|SubtitleRasterizer|Compositor|LayerGeometry" --output-on-failure
```

预期：PASS；无 D3D11 时 `Compositor` 测试 SKIP。`SubtitleRasterizer` 在 `render_tests` 中以 `QT_QPA_PLATFORM=offscreen` 运行，中文描边测试通过。

- [ ] **Step 7: 提交**

```bash
git add src/media/render src/media/CMakeLists.txt src/ui tests/media tests/CMakeLists.txt
git commit -m "feat(render): add D3D11 timeline compositor and subtitle rasterization"
```

---

### Task 11: 时间线 QML 界面、检查器与字幕编辑

**Files:**
- Create: `src/app/TimelinePanel.qml`
- Create: `src/app/TimelineClipDelegate.qml`
- Create: `src/app/TimelineCueDelegate.qml`
- Create: `src/app/TrackHeaderDelegate.qml`
- Create: `src/app/InspectorPanel.qml`
- Create: `src/app/SubtitlePanel.qml`
- Modify: `src/app/Main.qml`
- Modify: `src/app/AppController.h`
- Modify: `src/app/AppController.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/app/main.cpp`
- Create: `tests/app/TimelineUiTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `TimelineController`（全部 Q_INVOKABLE）、`TimelineModel` 角色（含 `TrackTypeRole`）、`TrackListModel`、`WaveformImageProvider`、`PreviewController::openTimeline`、`ProjectDocument::project()`、`MediaProbe`。
- Produces:
  - `AppController` 属性：`Q_PROPERTY(VideoEditor::TimelineController *timeline READ timeline CONSTANT)`、`Q_PROPERTY(VideoEditor::AudioEngine *audioEngine READ audioEngine CONSTANT)`、`Q_PROPERTY(VideoEditor::WaveformService *waveforms READ waveforms CONSTANT)`。
  - `AppController::addAssetToTimeline(const QString &assetId)`、`refreshTimelinePreview()`、`subtitleStyles()`、`selftestTimeline()`、`selftestPerf()`（Task 12 完成两个 selftest 的阈值部分）、信号 `timelineFramePresented()`。
  - `TimelineController::addClipForTest(const QString &trackId, const Clip &clip)`：公开但仅供自检/测试，内部调用 `TimelineEditor::addClip`。
  - QML 组件 `TimelinePanel`、`TimelineClipDelegate`、`TimelineCueDelegate`、`TrackHeaderDelegate`、`InspectorPanel`、`SubtitlePanel`。

- [ ] **Step 1: 写失败测试**

`tests/app/TimelineUiTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "AppController.h"
#include "PreviewController.h"
#include "TimelineController.h"
#include "TimelineModel.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(TimelineUi, ControllerIsExposedAndEditable)
{
    AppController controller;
    ASSERT_NE(controller.timeline(), nullptr);
    ASSERT_NE(controller.timeline()->timelineModel(), nullptr);
    EXPECT_NE(controller.audioEngine(), nullptr);

    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    const QString assetId = controller.document()->project().assets.first().id;

    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    ASSERT_FALSE(clipId.isEmpty());
    EXPECT_EQ(controller.timeline()->timelineModel()->rowCount(), 2); // video + audio

    controller.timeline()->seekUs(1'000'000);
    controller.timeline()->splitAtPlayhead();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 2);
    controller.undo();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.size(), 1);
}

TEST(TimelineUi, ImportsSrtAndExportsIt)
{
    AppController controller;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString srtPath = dir.filePath(QStringLiteral("in.srt"));
    QFile file(srtPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("1\n00:00:00,000 --> 00:00:01,000\n字幕测试\n");
    file.close();

    EXPECT_EQ(controller.timeline()->importSrtFile(srtPath), QStringLiteral("1"));
    ASSERT_EQ(controller.document()->project().tracks.at(2).cues.size(), 1);

    const QString outPath = dir.filePath(QStringLiteral("out.srt"));
    ASSERT_TRUE(controller.timeline()->exportSrtFile(outPath));
    QFile out(outPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_TRUE(out.readAll().contains(QStringLiteral("字幕测试").toUtf8()));
}

TEST(TimelineUi, OpenTimelineSwitchesPreviewMode)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(path)});
    const QString assetId = controller.document()->project().assets.first().id;
    controller.timeline()->addAssetToTimeline(assetId, QStringLiteral("track-v1"), 0);
    controller.refreshTimelinePreview();
    EXPECT_TRUE(controller.preview()->timelineMode());
    EXPECT_GT(controller.preview()->durationUs(), 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R TimelineUi --output-on-failure
```

预期：编译失败，`AppController::timeline()` 不存在。

- [ ] **Step 3: 装配控制器**

`AppController.h` 增加前向声明与属性（见 Interfaces），成员：

```cpp
    TimelineController *m_timeline = nullptr;
    AudioEngine *m_audio = nullptr;
    WaveformService *m_waveforms = nullptr;
    QThread *m_audioThread = nullptr;
```

构造函数顺序：

```cpp
    m_timeline = new TimelineController(*m_document, this);
    m_waveforms = new WaveformService(QDir(appData).filePath(QStringLiteral("cache")), this);
    m_audio = new AudioEngine;
    m_audioThread = new QThread(this);
    m_audio->moveToThread(m_audioThread);
    m_audioThread->start();
    m_preview->setAudioEngine(m_audio);
    connect(m_preview, &PreviewController::timelineFramePresented,
            this, &AppController::timelineFramePresented);
```

`importUrls` 在导入音频素材成功后调用 `m_waveforms->request(asset.id, asset.path)`。`openProject`、`restoreRecovery` 与 `addAssetToTimeline` 末尾调用公开的 `refreshTimelinePreview()`，内容：

```cpp
void AppController::refreshTimelinePreview()
{
    const Project &project = m_document->project();
    const int64_t duration = TimelineEvaluator(project).durationUs();
    m_timeline->seekUs(0);
    m_preview->openTimeline(project, duration);
}
```

`addAssetToTimeline(assetId)`：选择第一条同类型轨道（视频→`track-v1`，音频→`track-a1`），在播放头位置调用 `m_timeline->addAssetToTimeline`，失败时 `setLastError`。

析构：`m_audio->shutdown(); m_audioThread->quit(); m_audioThread->wait();`。

`main.cpp`：

```cpp
    engine.addImageProvider(QStringLiteral("waveform"),
                            new VideoEditor::WaveformImageProvider(controller.waveforms()));
    engine.rootContext()->setContextProperty(QStringLiteral("Timeline"),
                                             controller.timeline());
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"),
                                             controller.audioEngine());
```

`src/app/CMakeLists.txt` 的 `QML_FILES` 增加 6 个新 QML 文件。

- [ ] **Step 4: 实现时间线 QML**

`src/app/TimelinePanel.qml`：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property real trackHeight: 56
    property real rulerHeight: 28
    property real headerWidth: 110
    property real pixelsPerSecond: Timeline.pixelsPerSecond
    property real contentWidth: Math.max(width - headerWidth,
                                         Timeline.durationUs / 1e6 * pixelsPerSecond + 400)

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Track headers
        Column {
            Layout.preferredWidth: root.headerWidth
            Layout.fillHeight: true
            Item { width: 1; height: root.rulerHeight }
            Repeater {
                model: Timeline.trackModel
                TrackHeaderDelegate {
                    width: root.headerWidth
                    height: model.type === "subtitle" ? 40 : root.trackHeight
                    trackId: model.trackId
                    name: model.name
                    typeLabel: model.typeLabel
                    muted: model.muted
                    solo: model.solo
                    locked: model.locked
                }
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: root.contentWidth
            contentHeight: root.rulerHeight + root.trackHeight * 4
            clip: true
            ScrollBar.horizontal: ScrollBar {}

            Item {
                width: flick.contentWidth
                height: flick.contentHeight

                // Ruler and playhead
                Rectangle {
                    id: ruler
                    width: parent.width
                    height: root.rulerHeight
                    color: "#202020"
                    Repeater {
                        model: Math.ceil(width / (root.pixelsPerSecond))
                        Rectangle {
                            x: index * root.pixelsPerSecond
                            width: 1
                            height: 6
                            color: "#666666"
                        }
                    }
                }
                Rectangle {
                    id: playhead
                    x: Timeline.positionUs / 1e6 * root.pixelsPerSecond
                    y: 0
                    width: 2
                    height: parent.height
                    color: "#ff5b5b"
                    z: 20
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: (mouse) => {
                        if (mouse.y < root.rulerHeight)
                            Timeline.seekUs(Math.round(mouse.x / root.pixelsPerSecond * 1e6))
                    }
                }

                // Clips and cues share one model; delegates position themselves.
                Repeater {
                    model: Timeline.timelineModel
                    delegate: Loader {
                        sourceComponent: model.kind === "cue" ? cueComponent : clipComponent
                    }
                    Component {
                        id: clipComponent
                        TimelineClipDelegate {
                            trackId: model.trackId
                            itemId: model.itemId
                            assetId: model.assetId
                            startUs: model.startUs
                            durationUs: model.durationUs
                            speedMilli: model.speedMilli
                            linked: model.linked
                            trackType: model.trackType
                            selected: model.selected
                            y: root.rulerHeight + model.trackIndex * root.trackHeight
                            height: root.trackHeight - 4
                            timeline: root
                        }
                    }
                    Component {
                        id: cueComponent
                        TimelineCueDelegate {
                            trackId: model.trackId
                            itemId: model.itemId
                            text: model.text
                            startUs: model.startUs
                            durationUs: model.durationUs
                            selected: model.selected
                            y: root.rulerHeight + model.trackIndex * root.trackHeight
                            height: 36
                            timeline: root
                        }
                    }
                }
            }
        }
    }
}
```

`TimelineClipDelegate.qml`：

```qml
import QtQuick
import QtQuick.Controls

Rectangle {
    id: clip
    property var timeline
    property string trackId
    property string itemId
    property string assetId
    property real startUs
    property real durationUs
    property int speedMilli
    property bool linked
    property string trackType
    property bool selected: false

    x: startUs / 1e6 * timeline.pixelsPerSecond
    width: Math.max(8, durationUs / 1e6 * timeline.pixelsPerSecond)
    radius: 3
    color: selected ? "#3d6ea8" : (trackType === "audio" ? "#2b5d3f" : "#3a3a55")
    border.color: selected ? "#8fc4ff" : "#111111"
    border.width: 1
    clip: true

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 4
        text: (speedMilli !== 1000 ? (speedMilli / 1000).toFixed(2) + "x · " : "")
              + (linked ? "链接 · " : "")
        color: "#e8e8e8"
        font.pixelSize: 11
    }

    Image {
        visible: trackType === "audio"
        anchors.fill: parent
        anchors.topMargin: 14
        asynchronous: true
        cache: false
        fillMode: Image.Stretch
        source: visible ? "image://waveform/" + assetId + "/"
                          + Math.max(1, Math.round(width)) : ""
    }

    // Drag body: one command on release, no model updates while moving.
    MouseArea {
        id: body
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        property real pressX: 0
        property real originalX: 0
        onPressed: (mouse) => {
            pressX = mouse.x
            originalX = clip.x
            Timeline.selectClip(itemId)
        }
        onPositionChanged: (mouse) => {
            if (pressed)
                clip.x = Math.max(0, originalX + mouse.x - pressX)
        }
        onReleased: (mouse) => {
            const proposed = Math.round(clip.x / timeline.pixelsPerSecond * 1e6)
            Timeline.moveClip(itemId, Timeline.snapUs(proposed))
            clip.x = Qt.binding(() => startUs / 1e6 * timeline.pixelsPerSecond)
        }
    }

    // Trim handles.
    Rectangle {
        width: 8
        height: parent.height
        color: "#8fc4ff"
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            property real pressX: 0
            onPressed: (mouse) => { pressX = mouse.x; Timeline.selectClip(itemId) }
            onReleased: (mouse) => {
                const deltaUs = Math.round((mouse.x - pressX)
                                           / timeline.pixelsPerSecond * 1e6)
                Timeline.trimStart(itemId, Timeline.snapUs(startUs + deltaUs))
            }
        }
    }
    Rectangle {
        anchors.right: parent.right
        width: 8
        height: parent.height
        color: "#8fc4ff"
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            property real pressX: 0
            onPressed: (mouse) => { pressX = mouse.x; Timeline.selectClip(itemId) }
            onReleased: (mouse) => {
                const deltaUs = Math.round((mouse.x - pressX)
                                           / timeline.pixelsPerSecond * 1e6)
                Timeline.trimEnd(itemId, Timeline.snapUs(startUs + durationUs + deltaUs))
            }
        }
    }
}
```

`TimelineCueDelegate.qml`：

```qml
import QtQuick
import QtQuick.Controls

Rectangle {
    id: cue
    property var timeline
    property string trackId
    property string itemId
    property string text
    property real startUs
    property real durationUs
    property bool selected: false

    radius: 3
    color: selected ? "#6b5a2b" : "#3d3520"
    border.color: "#111111"
    x: startUs / 1e6 * timeline.pixelsPerSecond
    width: Math.max(8, durationUs / 1e6 * timeline.pixelsPerSecond)
    clip: true

    Text {
        anchors.fill: parent
        anchors.margins: 4
        text: cue.text
        color: "#f0e6c8"
        elide: Text.ElideRight
        font.pixelSize: 12
    }

    MouseArea {
        anchors.fill: parent
        onClicked: Timeline.selectCue(itemId)
        onDoubleClicked: editor.visible = true
    }
    TextArea {
        id: editor
        anchors.fill: parent
        visible: false
        text: cue.text
        onEditingFinished: {
            Timeline.setCueText(itemId, text)
            visible = false
        }
    }
}
```

`TrackHeaderDelegate.qml`：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    property string trackId
    property string name
    property string typeLabel
    property bool muted
    property bool solo
    property bool locked

    color: "#202020"
    border.color: "#101010"
    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        Label { text: name; color: "#dddddd"; Layout.fillWidth: true }
        Label { text: typeLabel; color: "#888888"; font.pixelSize: 10 }
        ToolButton {
            text: "M"
            checkable: true
            checked: muted
            onClicked: Timeline.setTrackMuted(trackId, checked)
        }
        ToolButton {
            text: "S"
            checkable: true
            checked: solo
            onClicked: Timeline.setTrackSolo(trackId, checked)
        }
        ToolButton {
            text: "锁"
            checkable: true
            checked: locked
            onClicked: Timeline.setTrackLocked(trackId, checked)
        }
    }
}
```

`SubtitlePanel.qml`：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    spacing: 6
    Label { text: "字幕"; font.bold: true; color: "#dddddd" }

    RowLayout {
        Button { text: "在播放头添加"; onClicked: Timeline.addCueAtPlayhead() }
        Button { text: "删除选中"; onClicked: Timeline.deleteSelectedCue() }
    }
    RowLayout {
        Button { text: "导入 SRT"; onClicked: srtImport.open() }
        Button { text: "导出 SRT"; onClicked: srtExport.open() }
    }
    ComboBox {
        id: styleBox
        Layout.fillWidth: true
        model: App.subtitleStyles()
        textRole: "name"
        valueRole: "id"
        onActivated: Timeline.applySubtitleStyle(currentValue)
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: Timeline.timelineModel
        delegate: ItemDelegate {
            width: ListView.view.width
            visible: model.kind === "cue"
            height: visible ? implicitHeight : 0
            text: model.text
            onClicked: Timeline.selectCue(model.itemId)
        }
    }

    FileDialog {
        id: srtImport
        title: "导入 SRT"
        nameFilters: ["字幕文件 (*.srt)"]
        onAccepted: Timeline.importSrtFile(selectedFile)
    }
    FileDialog {
        id: srtExport
        title: "导出 SRT"
        nameFilters: ["字幕文件 (*.srt)"]
        fileMode: FileDialog.SaveFile
        onAccepted: Timeline.exportSrtFile(selectedFile)
    }
}
```

`AppController::subtitleStyles()` 返回 `QVariantList`，每项 `{{"id", id}, {"name", name}}`，供 `ComboBox` 的 `textRole/valueRole` 使用。

`InspectorPanel.qml`：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    ColumnLayout {
        width: parent.width
        spacing: 8

        Label { text: "片段属性"; font.bold: true; color: "#dddddd" }
        Label { text: Timeline.selectedClipId ? "已选择片段" : "未选择片段"; color: "#999999" }

        RowLayout {
            Label { text: "变速"; color: "#cccccc" }
            ComboBox {
                model: ["0.25x", "0.5x", "1x", "1.5x", "2x", "4x"]
                currentIndex: 2
                enabled: Timeline.selectedClipId !== ""
                onActivated: {
                    const speeds = [250, 500, 1000, 1500, 2000, 4000]
                    Timeline.setClipSpeed(Timeline.selectedClipId, speeds[index],
                                          preservePitch.checked)
                }
            }
        }
        CheckBox { id: preservePitch; text: "保持音高"; checked: true; enabled: Timeline.selectedClipId !== "" }

        Label { text: "音量"; color: "#cccccc" }
        Slider {
            from: 0; to: 200; value: 100
            enabled: Timeline.selectedClipId !== ""
            onMoved: Timeline.setClipVolume(Timeline.selectedClipId, Math.round(value * 10))
        }
        Label { text: "声像"; color: "#cccccc" }
        Slider {
            from: -100; to: 100; value: 0
            enabled: Timeline.selectedClipId !== ""
            onMoved: Timeline.setClipPan(Timeline.selectedClipId, Math.round(value * 10))
        }
        Label { text: "淡入（秒）"; color: "#cccccc" }
        Slider {
            from: 0; to: 5; value: 0
            enabled: Timeline.selectedClipId !== ""
            onMoved: Timeline.setClipFades(Timeline.selectedClipId,
                                           Math.round(value * 1e6),
                                           Math.round(fadeOut.value * 1e6))
        }
        Label { text: "淡出（秒）"; color: "#cccccc" }
        Slider {
            id: fadeOut
            from: 0; to: 5; value: 0
            enabled: Timeline.selectedClipId !== ""
            onMoved: Timeline.setClipFades(Timeline.selectedClipId,
                                           Math.round(fadeIn.value * 1e6),
                                           Math.round(value * 1e6))
        }

        Label { text: "画面缩放"; color: "#cccccc" }
        Slider {
            from: 10; to: 400; value: 100
            enabled: Timeline.selectedClipId !== ""
            onMoved: Timeline.setClipTransform(Timeline.selectedClipId,
                                               Math.round(value * 10), 0, 0, 1000)
        }
        Label { text: "不透明度"; color: "#cccccc" }
        Slider {
            from: 0; to: 100; value: 100
            enabled: Timeline.selectedClipId !== ""
            onMoved: Timeline.setClipTransform(Timeline.selectedClipId,
                                               1000, 0, 0, Math.round(value * 10))
        }
    }
}
```

`SubtitlePanel.qml`：列出字幕轨 cues（`Repeater` 绑 `Timeline.timelineModel` 过滤 kind=="cue"），每条可编辑文本、选择样式 `ComboBox`（`builtinSubtitleStyles()` 通过 `App.subtitleStyles()` 暴露，需在 `AppController` 增加 `QVariantList subtitleStyles() const`），以及“导入 SRT”“导出 SRT”按钮打开 `FileDialog`。

`Main.qml` 布局：

```qml
ApplicationWindow {
    // header unchanged, add: 分割 / 删除 / 波纹删除 / 复制 / 粘贴 / 吸附 / 缩放
    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Button { text: "导入素材"; onClicked: fileDialog.open() }
            Button { text: "打开工程"; onClicked: openProjectDialog.open() }
            Button { text: "保存工程"; onClicked: saveProjectDialog.open() }
            Button { text: "撤销"; enabled: App.canUndo; onClicked: App.undo() }
            Button { text: "重做"; enabled: App.canRedo; onClicked: App.redo() }
            Button { text: "分割"; onClicked: Timeline.splitAtPlayhead() }
            Button { text: "删除"; onClicked: Timeline.deleteSelected(false) }
            Button { text: "波纹删除"; onClicked: Timeline.deleteSelected(true) }
            Button { text: "复制"; onClicked: Timeline.copySelected() }
            Button { text: "粘贴"; onClicked: Timeline.pasteAtPlayhead() }
            CheckBox { text: "吸附"; checked: Timeline.snapEnabled
                       onToggled: Timeline.snapEnabled = checked }
            Slider { from: 20; to: 400; value: Timeline.pixelsPerSecond
                     onMoved: Timeline.pixelsPerSecond = value; width: 120 }
            Item { Layout.fillWidth: true }
            Label { text: Preview.decodeMode; color: "#cccccc" }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 1

        Rectangle { // media library
            Layout.preferredWidth: 260
            Layout.fillHeight: true
            color: "#1b1b1b"
            ListView {
                anchors.fill: parent
                model: App.mediaModel
                delegate: ItemDelegate {
                    width: ListView.view.width
                    text: model.name + "\n" + model.codec + " · " + model.resolution
                    onClicked: App.openAsset(model.id)
                    onDoubleClicked: App.addAssetToTimeline(model.id)
                }
            }
        }

        ColumnLayout { // preview + timeline
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Rectangle { anchors.fill: parent; color: "black" }
                VideoPreview { id: previewItem; anchors.fill: parent }
                Component.onCompleted: Preview.item = previewItem
            }
            Rectangle {
                Layout.preferredHeight: 300
                Layout.fillWidth: true
                color: "#151515"
                TimelinePanel { anchors.fill: parent }
            }
        }

        Rectangle {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            color: "#1b1b1b"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                InspectorPanel { Layout.fillWidth: true; Layout.preferredHeight: 380 }
                SubtitlePanel { Layout.fillWidth: true; Layout.fillHeight: true }
            }
        }
    }
}
```

底部传输条改为 `Timeline.positionUs` / `Timeline.durationUs`，`Preview` 负责播放/暂停和逐帧，时间线模式由 `Preview` 的时钟驱动。

- [ ] **Step 5: 运行测试与界面自检**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R "TimelineUi|TimelineController|TimelineModel" --output-on-failure
windeployqt --qmldir src/app build/windows-x64-release/Release/VideoEditor.exe
./build/windows-x64-release/Release/VideoEditor.exe
```

预期：应用打开后可见媒体库、预览、三轨时间线（V1/A1/S1）与检查器；导入 `tests/fixtures/h264.mp4` 后双击素材，时间线出现视频+音频链接片段；拖动、裁切、分割、撤销正常；导入 SRT 后字幕轨出现条目；播放时音画同步。

- [ ] **Step 6: 提交**

```bash
git add src/app tests/app tests/CMakeLists.txt
git commit -m "feat(ui): add timeline editor QML, inspector and subtitle panel"
```

---

### Task 12: 集成自检、性能验证、文档与验收

**Files:**
- Modify: `src/app/AppController.h`
- Modify: `src/app/AppController.cpp`
- Modify: `src/app/main.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`
- Create: `tests/app/TimelineIntegrationTest.cpp`
- Modify: `tests/app/AppControllerTest.cpp`

**Interfaces:**
- Consumes: `MediaProbe`、`ProjectDocument`、`TimelineController`、`PreviewController`、`AudioEngine`、`TimelineEvaluator`、所有既有测试目标。
- Produces:
  - `AppController::selftestTimeline()`、`AppController::selftestPerf()`、信号 `timelineFramePresented()`。
  - CTest：`timeline_selftest`、`timeline_perf`（仅 Release、`LABELS gpu;performance`）。

- [ ] **Step 1: 写失败的集成测试**

`tests/app/TimelineIntegrationTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "AppController.h"
#include "ProjectSerializer.h"
#include "TimelineController.h"
#include "TimelineEvaluator.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(TimelineIntegration, EditsSurviveSaveAndReload)
{
    AppController controller;
    const QString video = fixture(QStringLiteral("h264.mp4"));
    const QString audio = fixture(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(video) || !QFileInfo::exists(audio))
        GTEST_SKIP() << "missing fixtures";

    controller.importUrls({QUrl::fromLocalFile(video), QUrl::fromLocalFile(audio)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 2);
    const QString videoId = controller.document()->project().assets.at(0).id;
    const QString audioId = controller.document()->project().assets.at(1).id;

    controller.timeline()->addAssetToTimeline(videoId, QStringLiteral("track-v1"), 0);
    controller.timeline()->addAssetToTimeline(audioId, QStringLiteral("track-a1"), 0);
    controller.timeline()->seekUs(1'000'000);
    controller.timeline()->setClipSpeed(
        controller.document()->project().tracks.at(0).clips.first().id, 1500, true);
    controller.timeline()->addCueAtPlayhead();
    const QString cueId = controller.document()->project().tracks.at(2).cues.first().id;
    controller.timeline()->setCueText(cueId, QStringLiteral("保存测试"));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("project.veproj"));
    QString error;
    ASSERT_TRUE(controller.document()->save(path, &error)) << error.toStdString();

    ProjectDocument reopened;
    ASSERT_TRUE(reopened.load(path, &error)) << error.toStdString();
    EXPECT_EQ(reopened.project().tracks.size(), 3);
    EXPECT_EQ(reopened.project().tracks.at(0).clips.first().speedMilli, 1500);
    EXPECT_EQ(reopened.project().tracks.at(2).cues.first().text,
              QStringLiteral("保存测试"));
    EXPECT_EQ(reopened.project().assets.size(), 2);

    const int64_t duration = TimelineEvaluator(reopened.project()).durationUs();
    EXPECT_GT(duration, 0);
}

TEST(TimelineIntegration, UndoHistoryDepthIsAtLeastOneHundred)
{
    AppController controller;
    const QString video = fixture(QStringLiteral("h264.mp4"));
    if (!QFileInfo::exists(video))
        GTEST_SKIP() << "missing h264.mp4";
    controller.importUrls({QUrl::fromLocalFile(video)});
    const QString assetId = controller.document()->project().assets.first().id;
    const QString clipId = controller.timeline()->addAssetToTimeline(
        assetId, QStringLiteral("track-v1"), 0);
    for (int i = 1; i <= 105; ++i)
        controller.timeline()->moveClip(clipId, i * 10'000);
    EXPECT_EQ(controller.document()->undoStack()->undoLimit(), 100);
    controller.document()->undoStack()->undo();
    EXPECT_EQ(controller.document()->project().tracks.at(0).clips.first().timelineStartUs,
              1'040'000);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release -R TimelineIntegration --output-on-failure
```

预期：编译失败，`TimelineIntegrationTest` 需要加入 `tests/CMakeLists.txt`；同时确认保存/加载与 100 步撤销的行为。

- [ ] **Step 3: 实现时间线与性能自检**

`AppController::selftestTimeline()`：用 `tests/fixtures` 构造一个完整工程（不依赖工程文件）：

```cpp
void AppController::selftestTimeline()
{
    const QString root = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR);
    importUrls({QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("h264.mp4"))),
                QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("tone.wav"))),
                QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("bgm.mp3")))});
    if (m_document->project().assets.size() < 3)
        return;

    const QString videoId = m_document->project().assets.at(0).id;
    const QString toneId = m_document->project().assets.at(1).id;
    const QString bgmId = m_document->project().assets.at(2).id;

    m_timeline->addAssetToTimeline(videoId, QStringLiteral("track-v1"), 0);

    // Add two more video tracks so the self-test covers 3 video layers.
    m_timeline->addTrack(0);
    m_timeline->addTrack(0);
    QStringList videoTracks;
    for (const Track &track : m_document->project().tracks) {
        if (track.type == TrackType::Video)
            videoTracks.append(track.id);
    }
    const QString v2 = videoTracks.at(videoTracks.size() - 2);
    const QString v3 = videoTracks.last();

    Clip overlay;
    overlay.id = QStringLiteral("perf-v2");
    overlay.assetId = videoId;
    overlay.sourceInUs = 0;
    overlay.sourceOutUs = 2'000'000;
    overlay.timelineStartUs = 0;
    overlay.transform.scaleMilli = 500;
    overlay.transform.offsetXPermille = 300;
    m_timeline->addClipForTest(v2, overlay);
    Clip overlay2 = overlay;
    overlay2.id = QStringLiteral("perf-v3");
    overlay2.transform.offsetXPermille = -300;
    m_timeline->addClipForTest(v3, overlay2);

    m_timeline->addAssetToTimeline(toneId, QStringLiteral("track-a1"), 0);
    m_timeline->addAssetToTimeline(bgmId, QStringLiteral("track-a1"), 0);

    QString subtitleTrackId;
    for (const Track &track : m_document->project().tracks) {
        if (track.type == TrackType::Subtitle)
            subtitleTrackId = track.id;
    }
    for (int i = 0; i < 3; ++i) {
        m_timeline->seekUs(i * 1'000'000);
        m_timeline->addCueAtPlayhead();
        const Track *subtitleTrack = m_document->trackById(subtitleTrackId);
        const QString cueId = subtitleTrack->cues.last().id;
        m_timeline->setCueText(cueId, QStringLiteral("多轨自检字幕 %1").arg(i + 1));
    }

    refreshTimelinePreview();
    m_timeline->seekUs(0);
    m_preview->play();
}
```

需要 `TimelineController::addClipForTest(trackId, clip)`（内部调用 `TimelineEditor::addClip`，公开但只给测试/自检使用，不暴露给 QML）。`addTrack(0)` 后新轨道 id 由控制器生成，因此上面从 `project().tracks` 里按顺序取最后两条视频轨。`selftestTimeline` 由 `main.cpp` 的 `--selftest-timeline` 调用；`timelineFramePresented` 计数达到 30 后 `exit(0)`，15 秒超时 `exit(2)`。

`AppController::selftestPerf()`：调用同一工程构造，`m_preview->play()`，用 `QElapsedTimer` 记录 5 秒内 `timelineFramePresented` 次数，打印：

```cpp
    const double fps = frames / elapsedSeconds;
    qInfo("TIMELINE_FPS=%.1f", fps);
    QCoreApplication::exit(fps >= 24.0 ? 0 : 3);
```

`main.cpp` 增加 `--selftest-timeline`、`--selftest-perf` 选项，分别用 `QTimer::singleShot(0, ...)` 启动，连接 `timelineFramePresented` 做计数；`--selftest-perf` 在 5 秒后打印并退出。

- [ ] **Step 4: 注册 CTest**

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(render_tests
  media/SubtitleRasterizerTest.cpp
  media/CompositorTest.cpp
  TestGuiMain.cpp)
target_link_libraries(render_tests PRIVATE
  videoeditor_media videoeditor_core GTest::gtest Qt6::Gui Qt6::Test
  d3d11 dxgi d3dcompiler)
target_compile_definitions(render_tests PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(render_tests PROPERTIES
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(ui_tests
  ui/TimelineModelTest.cpp
  ui/TimelineControllerTest.cpp
  ui/WaveformImageProviderTest.cpp
  TestGuiMain.cpp)
target_link_libraries(ui_tests PRIVATE
  videoeditor_ui GTest::gtest Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
target_compile_definitions(ui_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(ui_tests PROPERTIES
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_test(NAME timeline_selftest
         COMMAND VideoEditor --selftest-timeline)
set_tests_properties(timeline_selftest PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 30
  LABELS "gpu")

add_test(NAME timeline_perf
         COMMAND VideoEditor --selftest-perf)
set_tests_properties(timeline_perf PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 60
  CONFIGURATIONS Release
  LABELS "gpu;performance")
```

`app_tests` 增加 `app/TimelineUiTest.cpp`、`app/TimelineIntegrationTest.cpp`，并链接 `videoeditor_ui`（通过 `videoeditor_app` 的 PUBLIC 依赖已可用）。`media_tests` 增加 `media/FrameSelectorTest.cpp`、`media/AudioMixTest.cpp`、`media/AudioSpeedTest.cpp`、`media/AudioSyncTest.cpp`、`media/AudioEngineTest.cpp`、`media/WaveformTest.cpp`、`media/AudioProbeTest.cpp`。

- [ ] **Step 5: 运行全部测试**

```bash
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release --output-on-failure
```

预期：M1 的 `core_tests`、`media_tests`、`app_tests`、`preview_*` 全部保持通过；新增 `TimelineIntegration`、`TimelineUi`、`AudioMix`、`AudioSpeed`、`AudioSync`、`AudioEngine`、`Waveform`、`Srt`、`SubtitleEditor`、`TimelineModel`、`TimelineController`、`FrameSelector`、`SubtitleRasterizer`、`Compositor` 通过；`timeline_selftest` 通过。无桌面会话时 GPU 测试 SKIP 或以 2 退出，需手动确认。

- [ ] **Step 6: 更新 README 与手动验收清单**

`README.md` 的标题与简介改为 M2；增加：

- 时间线操作：拖动、裁切手柄、`S` 分割、`Delete` 删除、`Shift+Delete` 波纹删除、`Ctrl+C/Ctrl+V`、吸附开关、`+/-` 缩放。
- 音频：双击媒体库素材添加到时间线；检查器调整音量/声像/淡入淡出；音频轨显示波形；原声与 BGM 分轨静音/独奏。
- 字幕：字幕轨双击编辑；`SubtitlePanel` 导入/导出 SRT；样式预设 `default`、`boxed`、`bold-outline`。
- 变速：检查器 0.25–4x、保持音高；变速后仍可裁切/分割。
- 已知限制：OLA 时域伸缩为预览质量；不做速度曲线/光流；M3 负责导出。
- 手动验收（对照 spec §11.2）：
  1. 用 `h264.mp4` + `bgm.mp3` 做 3 轨片段，播放 60 秒，肉眼与逐帧检查无黑帧、无音画漂移。
  2. 片段 0.25x 与 4x 各做一次裁切、分割、撤销，确认时长与画面正常。
  3. BGM 设音量 200%、淡入 2 秒、淡出 2 秒、声像 -50，确认无削波（检查器/日志峰值 ≤ 0.98）。
  4. 导入 `tests/fixtures` 外的真实 SRT（含中文、CRLF、BOM），导出后用播放器打开确认时间轴一致。
  5. 保存工程、重启、打开，确认轨道、片段、字幕、变速与音量关键帧全部保留。
  6. 拔掉音频设备（或禁用默认输出）后播放，确认提示“无音频输出，静音预览”且预览继续。

- [ ] **Step 7: 提交**

```bash
git add src/app tests README.md
git commit -m "test(m2): add timeline self-tests, performance gate and M2 documentation"
```

---

## Self-Review

### 1. Spec coverage

| Spec requirement | Owning task | Verification |
|---|---|---|
| §3.1 多轨视频剪辑：移动、裁切、分割、波纹删除、音视频链接、逐帧定位、吸附 | Task 3, 4, 6, 11 | `TimelineEditorTest`（move/trim/split/ripple/link/snap）、`TimelineControllerTest`、Task 11 手动 UAT |
| §3.1 背景音乐：导入、波形、剪切、音量、声像、淡入淡出、音量关键帧 | Task 2, 7, 9, 11 | `AudioProbe`、`AudioMix`、`Waveform`、Task 11 Inspector |
| §3.1 字幕：独立轨、逐条编辑、样式/位置、SRT 双向、烧录预览 | Task 1, 5, 10, 11 | `SrtTest`、`SubtitleEditorTest`、`SubtitleRasterizerTest`、`TimelineCueDelegate` |
| §3.1 变速：片段级 0.25–4x、可选保持音高 | Task 3, 4, 7, 11 | `TimelineTest`、`TimelineEditor.SpeedChange`、`AudioSpeed` |
| §4.1 工程版本化、原子保存、v1 兼容 | Task 1, 12 | `ProjectSerializer.MigratesVersionOneProject`、`TimelineIntegration.EditsSurviveSaveAndReload` |
| §4.2 轨道分层/顺序调整、复制粘贴、撤销 ≥100 步 | Task 4, 6, 12 | `TimelineEditor.ReordersTracks`、`CopyPaste`、`UndoHistoryDepth` |
| §4.2 逐帧、时间码、缩放 | Task 3, 6, 11 | `TimeBase.FormatsDropFrameTimecode`、`TimelineController.stepFrames`、缩放滑块 |
| §4.3 48 kHz 浮点混音、WASAPI、无削波、原声/BGM 独立 | Task 7, 8 | `AudioMix.Limiter`、`Solo`、`AudioEngine` |
| §4.4 字幕样式/预设、SRT 编码处理、位置 | Task 5, 10 | `Srt.DecodesUtf16Le`、`SubtitleRasterizer.DrawsChinese`、`ApplyStyle` |
| §4.5 变速时长映射、保持音高、变速后仍可编辑 | Task 3, 4, 7 | `TimelineDurationFromSpeed`、`OlaPreservesPitch`、`SpeedChangeKeepsLinkedPairs` |
| §5.5 音频主时钟、视频按时间戳对齐、落后丢帧 | Task 8, 10 | `AudioSync`、`FrameSelector`、`PreviewController::onTimelineTick` |
| §6.1 独立解码线程、共享纹理桥接、无 CPU 回读 | Task 10 | `TimelineRenderWorker` + `FrameBridge` 复用；`Compositor` GPU 测试 |
| §6.2 Timeline Evaluator `frameAt/audioRange` | Task 3 | `TimelineEvaluatorTest` |
| §7.2 Track/Clip/SubtitleCue 字段 | Task 1 | schema roundtrip |
| §7.4 缓存、可重建 | Task 9 | `WaveformService.CachesPeaksOnDiskAndReloads` |
| §8.3 预览数据流 | Task 10, 11 | self-test + UAT |
| §9.1 多层顺序合成、字幕最上层、合成参数最小集 | Task 3, 10 | `OrdersVideoLayersBottomToTop`、`Compositor.BlendsTwoImages` |
| §9.2 时域伸缩、实时轻量算法、导出更高质量 | Task 7 | `OlaPreservesPitch`；README 说明 M3 升级 |
| §10 音画不同步以音频时钟重新对齐 | Task 8 | `AudioClock`、`AudioSync.MixerPosition` |
| §11.1 单元测试（模型/命令/求值/变速/SRT/时间码） | Task 1–6 | 上述测试 |
| §11.2 剪切/分割/变速无黑帧/无漂移、字幕逐帧、音画 ≤1 帧 | Task 3, 4, 7, 10, 12 | `AudioSync`、`FrameSelector`、UAT |
| §11.3 1080p 三视频轨+两音频轨+一字幕轨 ≥24 fps | Task 12 | `timeline_perf` |
| M1 回归 | Task 12 | 全量 `ctest` |

### 2. Placeholder scan

- 计划中没有 “TBD/TODO/实现细节略/类似 Task N”。每个任务给出精确文件、接口签名、可编译的测试与关键算法；长类（`TimelineEditor`、`TimelineMixer`、`TimelineRenderWorker`）给出核心方法完整实现，辅助方法给出精确语义与调用关系。
- Task 4 的 `deleteClip` 草稿含两段代码，实施者必须以“波纹语义必须精确”之后的最终版本为准；Task 6 明确要求把 `addClip` 重构为 `addClips` 以保证“一次撤销”，不要让视频与音频分两次 push。
- Task 10 中 `ClipVideoSource` 统一把硬件 NV12 与软件 RGBA 转成一张 RGBA 纹理，合成器只处理 RGBA，避免两套着色器分支；这是实现时的硬约束，不得退回直接在合成器里采样 NV12。

### 3. Type consistency

- `speedMilli`（int，250–4000）在所有任务中一致；`sourceTimeAt`/`scaleBySpeed`/`unscaleBySpeed` 只出现在 Task 1 定义，其他任务只调用。
- `FrameComposition`/`VideoLayer`/`AudioLayer`/`SubtitleLayer` 字段在 Task 3 定义；Task 10 与 Task 7 使用相同字段名（`sourceTimeUs`、`localTimeUs`、`transform`、`clip`）。
- `Project::assetById` 在 Task 1 定义，Task 2/10 使用；`Project::ensureDefaultTracks` 在 v1 迁移与 `ProjectDocument` 构造中调用。
- `TimelineEditCommand`/`TrackEdit` 在 Task 4 定义，Task 5 的 `editsForCues` 复用同一结构；`ProjectDocument::applyTrackEdit` 同时写 clips 与 cues，避免字幕命令另起一套机制。
- `IAudioSink` 的 `playedFrames()` 在 `AudioEngine` 与测试 sink 中一致；`frameIndexForClock` 是 `microsToFrames` 的包装，Task 7 的 `AudioSyncTest` 与 Task 8 的 `AudioClockTest` 共用。
- `WaveformPeaks::kBucketsPerSecond = 100` 与 `computeWaveform` 的 10 ms 桶、`WaveformImageProvider` 的映射一致。

### 4. Review Focus → tests

1. 链接音视频在不同帧率/非关键帧下移动/裁切/分割：Task 4 `MovesLinkedClipsAsOneUndoStep`、`TrimsLinkedClipsAndKeepsSourceAligned`、`SplitsLinkedClipsAndRegroupsRightHalves`；Task 3 `AppliesSpeedToSourceMapping`。
2. 0.25/4x 与短片段：Task 3 `SurvivesLongTimelineArithmeticAtFourX`、Task 4 `RejectsTrimsShorterThanOneFrame`、Task 7 `ClipReaderConsumesSourceAtClipSpeed`。
3. 真实 SRT 编码与格式：Task 5 `ParsesCrLfWithBomAndIndexLines`、`AcceptsShortMillisecondFields`、`DecodesUtf16Le`、`ReportsBadTimecodeWithCueIndex`、`ExportsUtf8BomCrLfAndRoundTrips`。
4. 无音频设备/欠载：Task 8 `FallsBackWhenDeviceOpenFails`、`StartsPauseAndSeekWithInjectedSink`；Task 12 手动验收第 6 项。
5. 素材丢失/空轨/禁用轨/超长片段/v1：Task 1 `MigratesVersionOneProject`、Task 2 `ProbesAllBackgroundMusicFormats`、Task 3 `ReturnsGapsAndSkipsDisabledTracks`、Task 10 `Compositor` 在缺失素材时跳过该层（`assetById == nullptr` 分支）并继续合成；Task 12 `EditsSurviveSaveAndReload`。

### 5. Open risks (carried into execution)

- **WASAPI 设备差异**：共享模式下 `AUTOCONVERTPCM` 在部分驱动可能失败，已准备 16-bit PCM 回退；若仍失败，按“无设备”路径静音预览并提示，不影响编辑与保存。
- **OLA 质量**：20 ms Hann/50% 重叠在强瞬态素材上可能有轻微梳状感；这是规格允许的实时轻量算法，M3 导出将替换为更高质量实现。
- **多解码器并发**：三层 1080p 硬件解码在低端 GPU 上可能达不到 24 fps；`timeline_perf` 会把问题暴露在 Release 门禁中，回退策略是降低同时活跃层数并仍按音画同步丢帧。
- **字幕字体**：`Microsoft YaHei` 在精简 Windows 安装可能缺失，`QFont` 会回退到系统字体；渲染测试只验证有字形输出，不绑定具体字体。
- **Task 4 的命令快照粒度**：整轨 clips 列表作为 before/after，编辑 1000 个片段的轨道时每条命令约 200 KB；M2 可接受，M5 若成为问题再改为按片段 id 的增量补丁。

