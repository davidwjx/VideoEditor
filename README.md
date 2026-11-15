# VideoEditor

Windows 桌面视频编辑器（M2 多轨编辑、音频与字幕；M3 导出）。M1 提供媒体探测、工程模型与撤销/保存/崩溃恢复、D3D11 硬件解码与软件回退、Qt Quick 预览播放与高分辨率代理；M2 在此基础上加入多轨时间线编辑、原声/BGM 混音与波形、字幕轨与 SRT 双向导入导出、片段变速，以及 3 视频轨 + 2 音频轨 + 1 字幕轨的 1080p 实时预览性能门禁（三条视频轨均使用原生 1920×1080 测试源）；M3 加入 MPEG-4 Part 2 / H.264 / H.265 导出（硬件优先、断点续跑、成片校验）。AI 功能属于后续里程碑。

- 平台：Windows x64，D3D11（不支持 OpenGL/Vulkan 预览路径）
- 语言/框架：C++20、Qt 6.8（Qt Quick）
- 编码：源码与界面文本均为 UTF-8，面向用户的错误提示为中文
- 音频：WASAPI 共享模式，48 kHz 浮点混音，音频时钟为预览主时钟

## 环境

- Windows 10 21H2+ / Windows 11 x64
- Visual Studio 2022（MSVC 19.4x，使用 “Visual Studio 17 2022” 生成器）
- CMake 3.27+（本机使用 Visual Studio 自带 CMake 3.29.5，无需单独安装）
- Qt 6.8.3 MSVC 2022 64-bit：`C:/Qt/6.8.3/msvc2022_64`
- FFmpeg 7.1 LGPL shared 预编译包（运行时 DLL、头文件与导入库），路径由 `FFMPEG_ROOT` 指定
- GoogleTest 1.15.2：配置时若未找到系统安装，通过 CMake `FetchContent` 自动下载
- 可选：带 libx264/libx265 的 FFmpeg CLI，仅用于生成测试素材和代理；不要将该 GPL 构建链接进应用

项目不使用 vcpkg；依赖通过上面的 Qt 安装、预编译 FFmpeg 与 FetchContent 提供。

## 构建

在 Git Bash / PowerShell 中执行：

```bash
# Git Bash：把 Visual Studio 自带的 CMake/CTest 加入 PATH
export PATH="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:$PATH"

cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release --output-on-failure
```

`CMakePresets.json` 已固定：

- 生成器：`Visual Studio 17 2022`，架构 `x64`
- `CMAKE_PREFIX_PATH`：`C:/Qt/6.8.3/msvc2022_64`
- `FFMPEG_ROOT`：本机 LGPL FFmpeg 7.1 预编译包路径
- 构建与测试配置：`Release`

其他机器上构建时，按需修改 `CMakePresets.json` 中的 `CMAKE_PREFIX_PATH` 与 `FFMPEG_ROOT`。

## 运行

Visual Studio 生成器的可执行文件位于：

```
build/windows-x64-release/Release/VideoEditor.exe
```

首次运行前部署 Qt 运行时（应用目录已由构建脚本复制 FFmpeg DLL 与 `ffmpeg.exe`）：

```bash
"C:/Qt/6.8.3/msvc2022_64/bin/windeployqt.exe" --qmldir src/app \
  build/windows-x64-release/Release/VideoEditor.exe
```

然后直接启动 `build/windows-x64-release/Release/VideoEditor.exe`。

代理生成优先使用应用目录下的 `ffmpeg.exe`，其次读取 `VIDEOEDITOR_FFMPEG`，最后使用 PATH 中的 `ffmpeg`。

## M2 编辑功能

### 时间线

- 从媒体库双击素材即可添加到当前视频/音频轨；拖动片段改变位置，拖动片段两端手柄裁切。
- `S` 在播放头处分割，`Delete` 删除选中片段，`Shift+Delete` 波纹删除（后续片段前移），`Ctrl+C` / `Ctrl+V` 复制粘贴。
- 拖动时默认吸附到片段边缘、播放头与其他轨道的边界；工具栏可关闭吸附。
- `+` / `=` 放大时间线，`-` 缩小；工具栏缩放滑块与快捷键同步。
- 链接的视频与音频片段作为一个整体移动、裁切、分割；轨道可静音、独奏、锁定，并可上下调整顺序。
- 撤销历史保留最近 100 步；工程保存为 `.veproj`（原子写入，兼容 v1 工程）。

### 音频

- 视频原声与 BGM 分轨放置，可分别静音/独奏；轨道头可添加/删除/上下移动轨道并选定目标轨（双击素材落到目标轨，V2 的原声自动配对 A2），音频轨自动显示与裁切/变速范围一致的波形（峰值缓存于本地，可删除重建）。
- 检查器可调整片段音量（0–200%）、声像与淡入/淡出时长；检查器还提供“音量关键帧”列表，可按当前播放头添加关键帧（使用片段当前音量）、删除单条关键帧，混音器按关键帧列表线性插值。关键帧时间相对于片段起点，超出片段范围或无效的时间会被拒绝。
- 混音在 48 kHz 浮点域完成并带限幅，音量 200% + 淡入淡出不会削波。
- 无可用音频设备或设备被拔出时显示“无音频输出，静音预览”，预览按壁钟继续，编辑与保存不受影响。

### 字幕

- 独立字幕轨；在 `SubtitlePanel` 点击“在播放头添加”新建字幕，双击时间线字幕条就地编辑文本；选中后可编辑开始/结束秒数（保留文本、样式与位置）。
- `SubtitlePanel` 可导入/导出 SRT；支持 UTF-8、UTF-8 BOM、UTF-16LE 与 CRLF，中文文本可正常往返。
- 内置样式预设：`default`（默认）、`boxed`（黑底白字）、`bold-outline`（粗体描边）。
- 选中字幕后，`SubtitlePanel` 的 X/Y 滑块可调整该条字幕在画面中的位置（0–1000‰，水平/垂直）；拖动过程不提交，松手后作为一个撤销步骤提交，文本与样式保持不变。

### 变速

- 检查器提供 0.25x、0.5x、1x、1.5x、2x、4x，也可直接输入自定义速度（0.25–4x）；“保持音高”默认关闭，可勾选启用 OLA 时域伸缩。
- 变速后片段时长按速度重新映射，仍可继续裁切、分割、移动与撤销；链接的音视频对保持同步。

## M3 导出

### 入口与选项

- 主窗口工具栏“导出”打开导出对话框；导出在独立工作线程运行，显示阶段、百分比、编码速度与预计剩余时间，可随时“取消”。
- 预设：匹配工程（H.264，跟随工程分辨率/帧率）、H.264 1080p30、H.264 720p30、H.265 1080p30（硬件）、MPEG-4 Part 2 1080p30。
- 可调项：编码格式（H.264 / H.265 / MPEG-4 Part 2）、质量档（低/中/高，按分辨率与帧率推导码率）或显式码率（100–200000 kbps）、优先硬件编码、烧录字幕、同时导出 SRT、输出路径。
- 分辨率范围 16×16–7680×4320，宽高必须为偶数；帧率 1–120 fps，输出为恒定帧率（CFR）。音频固定为 AAC-LC 48 kHz 双声道 192 kbps；无音频的工程也会写入一条静音 AAC 轨。
- 输出默认写入系统“视频”目录下的 `导出_yyyyMMdd_HHmmss.mp4`；未填扩展名时自动补 `.mp4`。SRT 边车默认与输出同名（`.srt`，UTF-8），也可单独指定路径。
- 字幕默认烧录进画面；关闭烧录仍可勾选“同时导出 SRT”，MP4 内不含字幕轨。

### 编码器

| 编码 | 路径 | 编码器 |
|---|---|---|
| MPEG-4 Part 2 | 软件 | FFmpeg 原生 `mpeg4`（始终可用） |
| H.264 | 硬件优先 | NVIDIA NVENC `h264_nvenc` → Intel Quick Sync `h264_qsv` → AMD AMF `h264_amf` → Media Foundation 硬件 MFT `h264_mf` |
| H.264 | 软件回退 | 默认 Media Foundation 软件 MFT `h264_mf`；可选 `libopenh264`（仅显式设置 `VIDEOEDITOR_H264_SOFTWARE=openh264`） |
| H.265 / HEVC | 仅硬件 | NVIDIA NVENC `hevc_nvenc` → Intel Quick Sync `hevc_qsv` → AMD AMF `hevc_amf` → Media Foundation 硬件 `hevc_mf` |

- 打开导出对话框时按目标分辨率/帧率逐个打开候选编码器并做 1–3 帧试编码，探测失败的编码器被跳过；后续调用使用进程内缓存。
- H.264 默认“优先硬件编码”；取消勾选后优先选择软件编码（默认 MF 软件 MFT）。硬件探测失败时自动回退软件并提示“速度较慢”；没有可用 H.264 编码器时禁用“开始导出”。
- H.265 只接受硬件编码器，绝不静默改用软件或其他编码。无可用硬件时对话框显示不可用原因（NVENC / Quick Sync / AMF / Media Foundation 均不可用），并禁用“开始导出”。
- 硬件要求：NVENC 需要 NVIDIA 显卡与驱动，QSV 需要 Intel 核显、驱动与 MFX 运行时，AMF 需要 AMD 显卡与驱动，Media Foundation 需要 Windows 自带的 MF 组件。没有对应硬件时该编码器仅探测失败并被跳过，不影响其他编码。
- 授权：应用只链接/分发 LGPL 版 FFmpeg；MPEG-4 为原生编码，Media Foundation H.264/HEVC 由 Windows 授权。不链接或分发 libx264/libx265。`libopenh264` 仅在操作者确认 Cisco 专利条款后通过环境变量显式启用，默认不选。

### 断点续跑

- 导出时在输出旁创建 `<输出文件>.parts/` 目录，保存已完成的 MP4 分段与 `checkpoint.json`。
- 取消、导出失败或进程中断后，已完成的分段与检查点保留；重新打开导出对话框且工程、编码器与预设未变化时显示“继续上次导出”，从断点继续。
- 修改工程、导出设置或输出路径会改变检查点哈希，续跑失效并从第一段重新导出；续跑前会逐段核对分段帧数，并确认硬件编码器的参数集（SPS/PPS）未变化，否则自动从头导出。
- 成功导出后分段目录与检查点会被清理。

### 成片校验

- 封装完成后重新探测临时文件：容器为 MP4、视频编码/分辨率/帧率符合设置、音轨为 AAC 48 kHz 双声道、视频包数等于时间线帧数、音视频时长差不超过一帧，并实际解码首个视频帧。
- 校验失败视为导出失败：删除临时文件，不写出最终文件（同名旧文件保持不变），保留可续跑的分段，并在对话框中显示中文错误。

### M3 已知限制

- 不支持 WMV/VC-1 导出；可导入并预览，但导出仅支持 MPEG-4 Part 2、H.264 与 H.265。
- 不支持软字幕轨：字幕只能烧录进画面，或作为 SRT 边车导出。
- 不支持 HDR 端到端：导出统一为 8-bit SDR（NV12 或 YUV420P）。
- H.265 不提供软件编码；没有受支持的硬件时无法选择 H.265。
- 音频固定 AAC-LC 48 kHz 双声道，不支持 5.1、其他采样率或无压缩音轨。
- 输出为恒定帧率；不提供可变帧率封装。编码器不启用 B 帧，编码参数（GOP、预设、B 帧）不开放微调。
- 1080p 硬件 H.264 ≥1× 实时的性能门禁依赖独显与交互式桌面会话；无硬件时相关测试会跳过。

## 测试

```bash
ctest --preset windows-x64-release --output-on-failure
```

- `core_tests`、`media_tests`、`app_tests`、`ui_tests`、`render_tests` 为 GoogleTest 单元/集成测试，不需要桌面会话；`TimelineIntegration` 覆盖保存/重载与 100 步撤销，`AppController.SelftestTimelineBuildsThreeVideoTwoAudioOneSubtitleProject` 校验多轨自检工程结构。
- `export_tests`（标签 `gpu`）覆盖导出格式矩阵（三种编码 + AAC）、预览/离线合成分辨率一致性、字幕烧录与 SRT 边车、音频限幅、取消/续跑/磁盘不足/中文路径，以及 1080p30 硬件 H.264 ≥1× 实时门禁。需要 D3D11 设备；某编码器不可用时对应用例 `GTEST_SKIP`，没有硬件时 H.265 的“禁用并说明原因”仍会执行。
- `app_qml_tests`（CTest 名 `TimelineQml`）加载 `Main.qml` 并通过真实 Qt Quick 事件链验证拖拽、裁切等交互；`export_qml_tests`（CTest 名 `ExportQml`）校验导出入口、预设/高级选项布局与 H.265 禁用原因。
- `preview_*` 为端到端预览自检：打开各格式素材，在 Qt Quick 窗口中呈现首帧后退出。成功时通过 `qInfo` 打印 `SELFTEST_FRAME_OK`；GUI 子系统构建中该日志可能不会出现在终端，应以退出码 0 为准。它们需要交互式桌面会话与可用的 D3D11 设备；无桌面会话时通常由应用内部的 15 秒定时器以退出码 2 结束，而不是 CTest 的 30 秒超时。
- `preview_selftest` 是 `h264.mp4` 的原有自检；`preview_h264` 为同一素材的格式自检别名。
- `timeline_selftest`（`VideoEditor --selftest-timeline`）用测试素材构造 1920×1080 的 3 视频轨 + 2 音频轨 + 1 字幕轨工程，播放并在呈现 30 帧后以 0 退出，内部 15 秒超时以 2 退出。
- `timeline_perf`（`VideoEditor --selftest-perf`，仅 Release，标签 `gpu;performance`）对同一工程预热 1 秒后统计 5 秒内实际呈现的帧数，打印 `TIMELINE_FPS=...` 与命中 3 层的帧数，低于 24 fps 或不足 90% 的帧包含全部 3 个视频层时以 3 退出；内部 20 秒超时以 4 退出。性能工程的视频源是真实原生 1920×1080 `perf1080.mp4`（三条视频轨各两次放置），不是从 640×360 素材放大。
- 两者都需要交互式桌面会话与 D3D11。设置 `QT_ASSUME_STDERR_HAS_CONSOLE=1` 可在终端看到 GUI 程序打印的 `TIMELINE_FPS`、帧数、层数与工程统计。
- `export_selftest`（`VideoEditor --selftest-export`，标签 `gpu;performance`，超时 180 秒）构造同一 3 视频轨 + 2 音频轨 + 1 字幕轨工程，改为 640×360p30 后依次用 MPEG-4 Part 2、H.264 与 H.265（有硬件时）导出到临时目录并校验成片，打印 `EXPORT_SELFTEST_OK codecs=... maxAvDiffUs=...`，任何编码失败或音画时差超过一帧时以 5 退出；无对应硬件时打印 `EXPORT_SELFTEST_SKIP` 并跳过该编码。该自检不创建 QML 窗口，但仍需要可用的 D3D11 设备；编码器探测与导出在独立 MTA 线程执行，与正式导出的线程模型一致。

```bash
# 只跑导出自检与导出相关测试（gtest 用例名以 Export 开头）
ctest --preset windows-x64-release -R "export_selftest|Export" --output-on-failure
```

## 测试素材

测试素材位于 `tests/fixtures/`。重新生成时使用带 libx264/libx265 的 GPL FFmpeg CLI（仅用于生成素材）：

```powershell
$env:VIDEOEDITOR_FFMPEG = "C:/Users/David/AppData/Local/Microsoft/WinGet/Packages/BtbN.FFmpeg.GPL.Shared.7.1_Microsoft.Winget.Source_8wekyb3d8bbwe/ffmpeg-n7.1.5-12-g1fdbca85aa-win64-gpl-shared-7.1/bin/ffmpeg.exe"
./tools/generate_fixtures.ps1
```

VC-1 自动测试使用 WMV2 容器样本；如拿到真实 VC-1 素材，可放入 `tests/fixtures/manual/vc1.wmv` 并用导入预览手动验证。

## 分辨率与代理

- 1080p 是推荐性能基线，不是分辨率上限；解码按素材自身实际分辨率进行，关闭代理时预览尺寸与帧呈现也按原分辨率处理。
- 高于 1080p 的素材默认生成 540p 代理（保持宽高比），以保证编辑时的预览流畅度；代理本身不是原分辨率。
- 关闭“使用代理”即可按素材原分辨率预览（例如 2560×1440 的高分辨率素材）；当前已打开的素材会立即重新打开并切换预览源。
- 代理为 MJPEG 全帧内编码，存放在本地缓存目录，可删除重建；M1 不做代理保留策略。

## M2 已知限制

- 变速使用 20 ms Hann 窗、50% 重叠的 OLA 时域伸缩，属于实时预览质量；强瞬态素材可能有轻微梳状感。M3 导出会替换为更高质量的离线实现。
- 不提供速度曲线与光流插帧；变速为片段级 0.25–4x。
- 音量关键帧仅支持离散列表的新增/删除（按播放头时间与片段当前音量插入），不提供关键帧曲线编辑器；字幕位置通过 X/Y 滑块调整，不支持在预览画面中直接拖拽。
- 音频输出为 WASAPI 共享模式；无设备或设备被独占时静音预览，不能自动切换到其他设备。
- 硬件解码路径只接受 NV12 8-bit；P010/10-bit 自动回退软件解码并转换为 8-bit 预览。
- 时间码由 `TimelineController` 按工程 FPS 生成，并遵循 `dropFrameTimecode` 工程设置；M2 界面暂不提供丢帧时间码开关。
- WMV/VC-1 解码由 FFmpeg 提供；自动测试使用 WMV2 容器样本，VC-1 需用真实样本（`tests/fixtures/manual/vc1.wmv`）验证。
- 代理使用 MJPEG 全帧内编码，存放在本地缓存目录，可删除重建；目前不提供代理保留/清理策略。
- 应用随包分发 LGPL 版 `ffmpeg.exe` 与 FFmpeg DLL，用于代理生成；该构建不含 libx264/libx265，素材生成需另行使用 GPL FFmpeg CLI。
- M2 不包含导出、AI 功能；导出由 M3 负责。

## 手动验收清单（spec §11.2）

以下项目需要交互式桌面会话、人工目视/听音与硬件操作。最终修复波次已在带桌面会话与 D3D11 的环境执行了可自动化/可脚本化部分（`ctest` 全绿，`timeline_selftest` 30 帧、`timeline_perf` 5 秒统计均通过；详见 `.superpowers/sdd/2026-11-15-m2-editing-audio-subtitles/final-fix-report.md`）。仍属人工残留的是主观逐帧目检、真实听音与物理拔插设备：

1. 用 `h264.mp4` + `bgm.mp3` 做 3 轨片段，播放 60 秒；肉眼与逐帧检查无黑帧、无音画漂移。
2. 对同一片段分别设 0.25x 与 4x，各做一次裁切、分割、撤销；确认时长与画面正常。
3. BGM 设音量 200%、淡入 2 秒、淡出 2 秒、声像 -50；确认无削波（检查器/日志峰值 ≤ 0.98）。
4. 导入 `tests/fixtures` 外的真实 SRT（含中文、CRLF、BOM），导出后用播放器打开，确认时间轴一致。
5. 在检查器中添加/删除音量关键帧、在字幕面板调整位置后保存工程，重启应用并重新打开，确认轨道、片段、字幕、变速、音量关键帧与字幕位置全部保留。
6. 拔掉音频设备（或禁用默认输出）后播放，确认提示“无音频输出，静音预览”且预览继续。

## 缓存与恢复文件位置

工程自动保存、代理与波形缓存位于 Qt 的 `AppLocalDataLocation`（`VideoEditor/VideoEditor`）下：

- `recovery/project.autosave.json`：崩溃恢复用的自动保存
- `cache/`：代理文件与 `waveform/` 波形峰值缓存

删除缓存目录后重新导入即可重建代理与波形。
