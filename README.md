# VideoEditor

Windows 桌面视频编辑器（M1 媒体基础）。M1 覆盖媒体探测、工程模型与撤销/保存/崩溃恢复、D3D11 硬件解码与软件回退、Qt Quick 预览播放、以及高分辨率素材的代理生成。导出、音频、字幕与 AI 功能不在 M1 范围内。

- 平台：Windows x64，D3D11（不支持 OpenGL/Vulkan 预览路径）
- 语言/框架：C++20、Qt 6.8（Qt Quick）
- 编码：源码与界面文本均为 UTF-8，面向用户的错误提示为中文

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

## 测试

```bash
ctest --preset windows-x64-release --output-on-failure
```

- `core_tests`、`media_tests`、`app_tests` 为 GoogleTest 单元/集成测试，不需要桌面会话。
- `preview_*` 为端到端预览自检：打开各格式素材，在 Qt Quick 窗口中呈现首帧后退出。成功时通过 `qInfo` 打印 `SELFTEST_FRAME_OK`；GUI 子系统构建中该日志可能不会出现在终端，应以退出码 0 为准。它们需要交互式桌面会话与可用的 D3D11 设备；无桌面会话时通常由应用内部的 15 秒定时器以退出码 2 结束，而不是 CTest 的 30 秒超时。
- `preview_selftest` 是 `h264.mp4` 的原有自检；`preview_h264` 为同一素材的格式自检别名。

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

## M1 已知限制

- 不播放音频；音画同步与音频编辑属于 M2。
- 硬件解码路径只接受 NV12 8-bit；P010/10-bit 自动回退软件解码并转换为 8-bit 预览。
- 时间码为非丢帧时间码；丢帧时间码在 M2 与音频时钟一起加入。
- WMV/VC-1 解码由 FFmpeg 提供；自动测试使用 WMV2 容器样本，VC-1 需用真实样本（`tests/fixtures/manual/vc1.wmv`）验证。
- 代理使用 MJPEG 全帧内编码，存放在本地缓存目录，可删除重建；M1 不提供代理保留/清理策略。
- 应用随包分发 LGPL 版 `ffmpeg.exe` 与 FFmpeg DLL，用于代理生成；该构建不含 libx264/libx265，素材生成需另行使用 GPL FFmpeg CLI。
- M1 不包含导出、音频、字幕与 AI 功能。

## 缓存与恢复文件位置

工程自动保存与代理缓存位于 Qt 的 `AppLocalDataLocation`（`VideoEditor/VideoEditor`）下：

- `recovery/project.autosave.json`：崩溃恢复用的自动保存
- `cache/`：代理文件

删除缓存目录后重新导入即可重建代理。
