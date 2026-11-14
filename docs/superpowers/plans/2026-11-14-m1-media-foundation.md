# M1 媒体基础实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付一个 Windows 桌面应用：可导入 MP4（MPEG-4 Part 2 / H.264 / H.265）与 WMV/VC-1，使用 D3D11VA 硬件解码（失败时软件回退），在 Qt Quick 中稳定预览，并具备工程保存/撤销、代理媒体与缓存、基础错误提示。

**Architecture:** Qt 6 Quick/QML 界面 + C++ 核心。工程与媒体探测在 core/media 静态库；解码运行在独立线程与独立 D3D11 设备上，NV12/RGBA 帧在 GPU 内转换到共享纹理，再由 Qt 场景图线程通过 `QNativeInterface::QSGD3D11Texture::fromNative()` 包装为 `QSGTexture` 显示。全程不经过 CPU 读回。

**Tech Stack:** C++20、Qt 6.8 LTS（Core/Gui/Quick/QuickControls2/Qml/Test）、FFmpeg 7.1（LGPL，libavformat/libavcodec/libavutil/libswscale）、Direct3D 11、GoogleTest、CMake 3.27+、MSVC 2022、vcpkg。

**Spec:** [docs/superpowers/specs/2026-11-14-video-editor-design.md](../specs/2026-11-14-video-editor-design.md)

## Global Constraints

- 仅支持 Windows 10 21H2+ / Windows 11 x64；编译器 MSVC 2022 19.4x；Windows SDK 10.0.26100。
- Qt 固定 6.8.x；FFmpeg 固定 7.1.x；CMake ≥ 3.27；C++20。
- 图形后端只能是 D3D11：`QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11)`，不写 Vulkan/OpenGL/D3D12 路径。
- 所有源码 UTF-8；面向用户的错误、按钮、提示使用中文；标识符与日志使用英文。
- 时间统一用 `int64_t` 微秒；帧率用 `Rational{num, den}`，禁止用浮点累加计算时间。
- 原始素材只读；任何功能不得修改源文件；缓存目录在 `QStandardPaths::AppLocalDataLocation` 下。
- 运行时不发起任何网络请求。
- 单元测试不得强制依赖 GPU：无法创建 D3D11 设备或视频解码器时，测试用 `GTEST_SKIP()` 跳过并打印原因。
- 每个任务结束必须 `cmake --build`、`ctest --output-on-failure` 通过后提交。
- 所有提交信息末尾附加一行：`Co-Authored-By: Claude Code <noreply@anthropic.com>`。
- M1 不实现音频播放、字幕、时间线剪辑、AI 消除、导出；这些属于后续里程碑。

## Review Focus

以下五类输入/条件最容易让用户感知到错误，且规格没有逐条展开。每个测试都放进拥有该代码的任务：

1. **带旋转元数据的手机视频**：预览必须按显示矩阵方向显示，不能横躺。测试在 Task 3（探测 90°）和 Task 7（旋转值传入帧发布器）。
2. **中文、空格、Unicode 路径**：导入、探测、解码不能因路径编码失败。测试在 Task 3 和 Task 7。
3. **变帧率（VFR）素材**：时长、帧率、逐帧步进不能漂移；解码时间戳必须单调递增。测试在 Task 3 和 Task 7。
4. **无音频、多视频流、损坏文件**：必须选择最佳视频流、允许无音频、损坏文件给出可操作提示而不是崩溃。测试在 Task 3。
5. **GPU 不支持视频解码或高于 1080p 的素材**：必须回退软件解码；高于 1080p 默认触发代理计划，但 1080p 只是性能基线，关闭代理后按素材原分辨率预览。测试在 Task 7 和 Task 9。

---

## Prerequisites

这些命令只需执行一次，属于环境准备，不是任务步骤。

```powershell
# 1. 安装 vcpkg
git clone https://github.com/microsoft/vcpkg C:/vcpkg
C:/vcpkg/bootstrap-vcpkg.bat
setx VCPKG_ROOT C:/vcpkg

# 2. 安装 Qt 6.8.3 MSVC 2022 64-bit
py -m pip install aqtinstall
py -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:/Qt
setx QTDIR C:/Qt/6.8.3/msvc2022_64

# 3. 验证工具（新开终端后）
cmake --version        # >= 3.27
ninja --version
where cl               # MSVC 编译器
where ffmpeg           # 仅用于生成测试素材；建议使用带 libx264/libx265 的 GPL 构建
```

FFmpeg 运行库由 vcpkg manifest 安装；测试素材脚本需要系统 `ffmpeg.exe`，它只用于生成素材，不随应用分发。

---

### Task 1: 工程骨架、构建系统与测试基线

**Files:**
- Create: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `vcpkg.json`
- Create: `cmake/FindFFmpeg.cmake`
- Create: `src/CMakeLists.txt`
- Create: `src/core/CMakeLists.txt`
- Create: `src/core/AppInfo.h`
- Create: `src/core/AppInfo.cpp`
- Create: `src/app/CMakeLists.txt`
- Create: `src/app/main.cpp`
- Create: `src/app/Main.qml`
- Create: `tests/CMakeLists.txt`
- Create: `tests/core/AppInfoTest.cpp`

**Interfaces:**
- Consumes: 无。
- Produces: `VideoEditor::appVersion() -> QString`；构建目标 `videoeditor_core`、可执行文件 `VideoEditor`；CTest 测试 `app_info_test`。

- [ ] **Step 1: 初始化仓库与目录**

```bash
cd d:/works/VideoEditor
git init
mkdir -p src/core src/app src/media/probe src/media/decode src/media/proxy src/media/cache src/ui tests/core tests/media tests/app tests/fixtures cmake docs/superpowers/plans docs/superpowers/specs tools
git add docs
git commit -m "docs: add video editor design spec and M1 plan"
```

预期：`git log --oneline` 显示一次提交。

- [ ] **Step 2: 写构建文件**

`.gitignore`：

```gitignore
build/
out/
.vs/
.vscode/
*.user
*.suo
CMakeUserPresets.json
tests/fixtures/*.tmp
```

`vcpkg.json`：

```json
{
  "name": "videoeditor",
  "version": "0.1.0",
  "dependencies": [
    "ffmpeg",
    "gtest"
  ]
}
```

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.27)
project(VideoEditor VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Qml Test)
find_package(GTest CONFIG REQUIRED)

qt_standard_project_setup()

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
```

`CMakePresets.json`：

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "windows-x64-debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/windows-x64-debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows",
        "CMAKE_PREFIX_PATH": "$env{QTDIR}",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    {
      "name": "windows-x64-release",
      "inherits": "windows-x64-debug",
      "binaryDir": "${sourceDir}/build/windows-x64-release",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    }
  ],
  "buildPresets": [
    { "name": "windows-x64-debug", "configurePreset": "windows-x64-debug" },
    { "name": "windows-x64-release", "configurePreset": "windows-x64-release" }
  ],
  "testPresets": [
    {
      "name": "windows-x64-debug",
      "configurePreset": "windows-x64-debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "windows-x64-release",
      "configurePreset": "windows-x64-release",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

`cmake/FindFFmpeg.cmake`：

```cmake
set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "FFmpeg prefix")
if(NOT FFMPEG_ROOT AND DEFINED VCPKG_INSTALLED_DIR)
  set(FFMPEG_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include" REQUIRED)

set(_ffmpeg_libs avcodec avformat avutil swscale swresample)
foreach(_lib IN LISTS _ffmpeg_libs)
  find_library(FFMPEG_${_lib}_LIBRARY_RELEASE NAMES ${_lib} HINTS "${FFMPEG_ROOT}/lib")
  find_library(FFMPEG_${_lib}_LIBRARY_DEBUG NAMES ${_lib} HINTS "${FFMPEG_ROOT}/debug/lib")
  if(NOT FFMPEG_${_lib}_LIBRARY_RELEASE)
    message(FATAL_ERROR "Missing FFmpeg library: ${_lib}")
  endif()
  if(NOT TARGET FFmpeg::${_lib})
    add_library(FFmpeg::${_lib} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${_lib} PROPERTIES
      IMPORTED_LOCATION_RELEASE "${FFMPEG_${_lib}_LIBRARY_RELEASE}"
      IMPORTED_LOCATION_DEBUG "${FFMPEG_${_lib}_LIBRARY_DEBUG}"
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
  endif()
endforeach()

set(FFmpeg_FOUND TRUE)
```

`src/CMakeLists.txt`：

```cmake
add_subdirectory(core)
add_subdirectory(app)
```

`src/core/CMakeLists.txt`：

```cmake
add_library(videoeditor_core STATIC
  AppInfo.h
  AppInfo.cpp
)
target_include_directories(videoeditor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_core PUBLIC Qt6::Core)
```

`src/app/CMakeLists.txt`：

```cmake
qt_add_executable(VideoEditor main.cpp)
qt_add_qml_module(VideoEditor
  URI VideoEditor
  VERSION 1.0
  QML_FILES Main.qml
)
target_link_libraries(VideoEditor PRIVATE Qt6::Quick Qt6::QuickControls2 videoeditor_core)
```

`tests/CMakeLists.txt`：

```cmake
include(GoogleTest)

set(TEST_ENV_MOD
  "PATH=path_list_prepend:$ENV{QTDIR}/bin"
  "PATH=path_list_prepend:$ENV{VCPKG_ROOT}/installed/x64-windows/bin")

add_executable(app_info_test core/AppInfoTest.cpp)
target_link_libraries(app_info_test PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(app_info_test PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 3: 写 AppInfo 与测试**

`src/core/AppInfo.h`：

```cpp
#pragma once

#include <QString>

namespace VideoEditor {

QString appVersion();

} // namespace VideoEditor
```

`src/core/AppInfo.cpp`：

```cpp
#include "AppInfo.h"

namespace VideoEditor {

QString appVersion()
{
    return QStringLiteral("0.1.0");
}

} // namespace VideoEditor
```

`tests/core/AppInfoTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "AppInfo.h"

TEST(AppInfoTest, ReturnsSemanticVersion)
{
    EXPECT_EQ(VideoEditor::appVersion(), QStringLiteral("0.1.0"));
}
```

- [ ] **Step 4: 写最小可运行界面**

`src/app/main.cpp`：

```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include "AppInfo.h"

int main(int argc, char *argv[])
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("VideoEditor"));
    QGuiApplication::setApplicationName(QStringLiteral("VideoEditor"));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             VideoEditor::appVersion());
    engine.loadFromModule(QStringLiteral("VideoEditor"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
```

`src/app/Main.qml`：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "VideoEditor " + appVersion
    color: "#111111"

    Label {
        anchors.centerIn: parent
        text: "媒体库为空，点击“导入视频”开始（M1 骨架）"
        color: "#dddddd"
    }
}
```

- [ ] **Step 5: 配置、构建、运行测试**

```bash
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：配置成功；`app_info_test` 通过。

- [ ] **Step 6: 运行应用做冒烟检查**

```powershell
windeployqt --qmldir src/app build/windows-x64-debug/src/app/VideoEditor.exe
./build/windows-x64-debug/src/app/VideoEditor.exe
```

预期：出现 1280×800 深色窗口，标题包含 `0.1.0`，中央提示文字可见。关闭窗口。

- [ ] **Step 7: 提交**

```bash
git add .gitignore CMakeLists.txt CMakePresets.json vcpkg.json cmake src tests
git commit -m "build: bootstrap Qt Quick app with CMake and GoogleTest"
```

---

### Task 2: 时间基、媒体信息与编码兼容性

**Files:**
- Create: `src/core/TimeBase.h`
- Create: `src/core/TimeBase.cpp`
- Create: `src/core/MediaInfo.h`
- Create: `src/core/CodecSupport.h`
- Create: `src/core/CodecSupport.cpp`
- Modify: `src/core/CMakeLists.txt`
- Create: `tests/core/TimeBaseTest.cpp`
- Create: `tests/core/CodecSupportTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: 无。
- Produces:
  - `VideoEditor::Rational { int num; int den; bool isValid() const; double toDouble() const; }`
  - `VideoEditor::framesToMicros(int64_t, Rational) -> int64_t`
  - `VideoEditor::microsToFrames(int64_t, Rational) -> int64_t`
  - `VideoEditor::formatTimecode(int64_t, Rational) -> QString`
  - `VideoEditor::formatDuration(int64_t) -> QString`
  - `VideoEditor::MediaInfo` 结构体
  - `VideoEditor::classifyVideoCodec(const QString&) -> CodecVerdict`

- [ ] **Step 1: 写失败测试**

`tests/core/TimeBaseTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "TimeBase.h"

using namespace VideoEditor;

TEST(TimeBase, ConvertsFramesAtThirtyFps)
{
    EXPECT_EQ(framesToMicros(30, Rational{30, 1}), 1'000'000);
    EXPECT_EQ(framesToMicros(1, Rational{30, 1}), 33'333);
}

TEST(TimeBase, ConvertsNtscRateWithoutDrift)
{
    EXPECT_EQ(framesToMicros(30, Rational{30000, 1001}), 1'001'000);
    EXPECT_EQ(microsToFrames(1'001'000, Rational{30000, 1001}), 30);
}

TEST(TimeBase, FormatsTimecodeAndDuration)
{
    EXPECT_EQ(formatTimecode(3'661'000'000, Rational{30, 1}),
              QStringLiteral("01:01:01:00"));
    EXPECT_EQ(formatDuration(3'661'000'000),
              QStringLiteral("01:01:01.000"));
}

TEST(TimeBase, ClampsNegativeValues)
{
    EXPECT_EQ(formatTimecode(-5, Rational{30, 1}), QStringLiteral("00:00:00:00"));
    EXPECT_EQ(formatDuration(-5), QStringLiteral("00:00:00.000"));
}
```

`tests/core/CodecSupportTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "CodecSupport.h"

using namespace VideoEditor;

TEST(CodecSupport, RecognizesSupportedCodecs)
{
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("mpeg4")).kind,
              VideoCodecKind::Mpeg4Part2);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("h264")).kind,
              VideoCodecKind::H264);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("hevc")).kind,
              VideoCodecKind::H265);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("wmv3")).kind,
              VideoCodecKind::WmvVc1);
    EXPECT_EQ(classifyVideoCodec(QStringLiteral("vc1")).kind,
              VideoCodecKind::WmvVc1);
    EXPECT_TRUE(classifyVideoCodec(QStringLiteral("h264")).supported);
}

TEST(CodecSupport, RejectsUnknownCodecWithActionableMessage)
{
    const CodecVerdict verdict = classifyVideoCodec(QStringLiteral("av1"));
    EXPECT_FALSE(verdict.supported);
    EXPECT_TRUE(verdict.reason.contains(QStringLiteral("不支持")));
    EXPECT_TRUE(verdict.reason.contains(QStringLiteral("转码")));
}
```

修改 `tests/CMakeLists.txt`，追加：

```cmake
add_executable(core_tests
  core/AppInfoTest.cpp
  core/TimeBaseTest.cpp
  core/CodecSupportTest.cpp
)
target_link_libraries(core_tests PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(core_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：编译失败，提示找不到 `TimeBase.h` / `CodecSupport.h`。

- [ ] **Step 3: 实现时间基**

`src/core/TimeBase.h`：

```cpp
#pragma once

#include <cstdint>
#include <QString>

namespace VideoEditor {

struct Rational {
    int num = 0;
    int den = 1;

    bool isValid() const { return num > 0 && den > 0; }
    double toDouble() const { return isValid() ? double(num) / double(den) : 0.0; }
    bool operator==(const Rational &) const = default;
};

constexpr int64_t kMicrosPerSecond = 1'000'000;

int64_t framesToMicros(int64_t frames, Rational fps);
int64_t microsToFrames(int64_t micros, Rational fps);
QString formatTimecode(int64_t micros, Rational fps);
QString formatDuration(int64_t micros);

} // namespace VideoEditor
```

`src/core/TimeBase.cpp`：

```cpp
#include "TimeBase.h"

#include <cmath>

namespace VideoEditor {

namespace {

int64_t clampNonNegative(int64_t value)
{
    return value < 0 ? 0 : value;
}

} // namespace

int64_t framesToMicros(int64_t frames, Rational fps)
{
    if (!fps.isValid())
        return 0;
    const long double micros = static_cast<long double>(frames) * kMicrosPerSecond
                               * fps.den / fps.num;
    return static_cast<int64_t>(std::llround(micros));
}

int64_t microsToFrames(int64_t micros, Rational fps)
{
    if (!fps.isValid())
        return 0;
    const long double frames = static_cast<long double>(micros) * fps.num
                               / (kMicrosPerSecond * static_cast<long double>(fps.den));
    return static_cast<int64_t>(std::llround(frames));
}

QString formatTimecode(int64_t micros, Rational fps)
{
    micros = clampNonNegative(micros);
    if (!fps.isValid())
        fps = Rational{30, 1};

    const int nominalFps = std::max(1, static_cast<int>(std::llround(fps.toDouble())));
    const int64_t totalFrames = microsToFrames(micros, fps);
    const int64_t frames = totalFrames % nominalFps;
    const int64_t totalSeconds = totalFrames / nominalFps;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;

    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}

QString formatDuration(int64_t micros)
{
    micros = clampNonNegative(micros);
    const int64_t totalMs = micros / 1000;
    const int64_t ms = totalMs % 1000;
    const int64_t totalSeconds = totalMs / 1000;
    const int64_t seconds = totalSeconds % 60;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t hours = totalSeconds / 3600;

    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

} // namespace VideoEditor
```

- [ ] **Step 4: 实现媒体信息与编码判定**

`src/core/MediaInfo.h`：

```cpp
#pragma once

#include <cstdint>
#include <QString>

#include "TimeBase.h"

namespace VideoEditor {

struct MediaInfo {
    QString path;
    QString container;
    QString videoCodec;
    QString audioCodec;
    int width = 0;
    int height = 0;
    Rational frameRate;
    int64_t durationUs = 0;
    int rotationDegrees = 0;
    bool hasAudio = false;
    bool supported = false;
    QString unsupportedReason;

    bool operator==(const MediaInfo &) const = default;
};

} // namespace VideoEditor
```

`src/core/CodecSupport.h`：

```cpp
#pragma once

#include <QString>

namespace VideoEditor {

enum class VideoCodecKind {
    Mpeg4Part2,
    H264,
    H265,
    WmvVc1,
    Unsupported
};

struct CodecVerdict {
    VideoCodecKind kind = VideoCodecKind::Unsupported;
    bool supported = false;
    QString reason;
};

CodecVerdict classifyVideoCodec(const QString &ffmpegName);
QString friendlyCodecName(VideoCodecKind kind);

} // namespace VideoEditor
```

`src/core/CodecSupport.cpp`：

```cpp
#include "CodecSupport.h"

namespace VideoEditor {

CodecVerdict classifyVideoCodec(const QString &ffmpegName)
{
    const QString name = ffmpegName.trimmed().toLower();

    if (name == QStringLiteral("mpeg4"))
        return {VideoCodecKind::Mpeg4Part2, true, {}};
    if (name == QStringLiteral("h264"))
        return {VideoCodecKind::H264, true, {}};
    if (name == QStringLiteral("hevc") || name == QStringLiteral("h265"))
        return {VideoCodecKind::H265, true, {}};
    if (name == QStringLiteral("wmv1") || name == QStringLiteral("wmv2")
        || name == QStringLiteral("wmv3") || name == QStringLiteral("vc1")) {
        return {VideoCodecKind::WmvVc1, true, {}};
    }

    return {VideoCodecKind::Unsupported,
            false,
            QStringLiteral("不支持的视频编码（%1）。请先转码为 H.264 或 H.265 的 MP4。")
                .arg(ffmpegName)};
}

QString friendlyCodecName(VideoCodecKind kind)
{
    switch (kind) {
    case VideoCodecKind::Mpeg4Part2:
        return QStringLiteral("MPEG-4 Part 2");
    case VideoCodecKind::H264:
        return QStringLiteral("H.264");
    case VideoCodecKind::H265:
        return QStringLiteral("H.265");
    case VideoCodecKind::WmvVc1:
        return QStringLiteral("WMV/VC-1");
    case VideoCodecKind::Unsupported:
        return QStringLiteral("未知编码");
    }
    return QStringLiteral("未知编码");
}

} // namespace VideoEditor
```

修改 `src/core/CMakeLists.txt`：

```cmake
add_library(videoeditor_core STATIC
  AppInfo.h
  AppInfo.cpp
  TimeBase.h
  TimeBase.cpp
  MediaInfo.h
  CodecSupport.h
  CodecSupport.cpp
)
target_include_directories(videoeditor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_core PUBLIC Qt6::Core)
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`app_info_test`、`core_tests` 全部通过。

- [ ] **Step 6: 提交**

```bash
git add src/core tests
git commit -m "feat(core): add time base, media info and codec support"
```

---

### Task 3: FFmpeg 探测、错误映射与测试素材

**Files:**
- Create: `src/media/CMakeLists.txt`
- Create: `src/media/probe/MediaProbe.h`
- Create: `src/media/probe/MediaProbe.cpp`
- Create: `src/media/probe/FfmpegError.h`
- Create: `src/media/probe/FfmpegError.cpp`
- Modify: `src/CMakeLists.txt`
- Create: `tools/generate_fixtures.ps1`
- Create: `tests/fixtures/`（由脚本生成并提交）
- Create: `tests/TestMain.cpp`
- Create: `tests/media/MediaProbeTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MediaInfo`、`classifyVideoCodec()`、`Rational`。
- Produces: `VideoEditor::MediaProbe::probe(const QString&, QString*) -> std::optional<MediaInfo>`。

- [ ] **Step 1: 生成测试素材**

`tools/generate_fixtures.ps1`：

```powershell
$ErrorActionPreference = "Stop"
$ffmpeg = if ($env:VIDEOEDITOR_FFMPEG) { $env:VIDEOEDITOR_FFMPEG } else { "ffmpeg" }
$out = Join-Path $PSScriptRoot "..\tests\fixtures"
New-Item -ItemType Directory -Force $out | Out-Null

$src = "testsrc2=size=640x360:rate=30:duration=2"
$audio = "sine=frequency=440:sample_rate=48000:duration=2"

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v mpeg4 -q:v 3 -c:a aac -shortest (Join-Path $out "mpeg4.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest (Join-Path $out "h264.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx265 -pix_fmt yuv420p -tag:v hvc1 -c:a aac -shortest (Join-Path $out "h265.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx265 -pix_fmt yuv420p10le -tag:v hvc1 -c:a aac -shortest (Join-Path $out "h265_10bit.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v wmv2 -c:a wmav2 -shortest (Join-Path $out "wmv2.wmv")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=red:size=64x64:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p (Join-Path $out "red.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p -an (Join-Path $out "noaudio.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=30:duration=2" `
  -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" `
  -map 0:v -map 1:v -c:v libx264 -pix_fmt yuv420p (Join-Path $out "twovideo.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p -metadata:s:v:0 rotate=90 (Join-Path $out "rotated.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=30:duration=3" `
  -vf "select='if(lt(mod(n,3),2),1,0)'" -fps_mode vfr -c:v libx264 -pix_fmt yuv420p `
  (Join-Path $out "vfr.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=2560x1440:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p (Join-Path $out "highres.mp4")

Copy-Item (Join-Path $out "h264.mp4") (Join-Path $out "中文 文件名.mp4") -Force
Set-Content -Path (Join-Path $out "corrupt.mp4") -Value "this is not a video" -Encoding Ascii

Write-Host "Fixtures written to $out"
```

运行：

```powershell
./tools/generate_fixtures.ps1
```

预期：`tests/fixtures/` 下出现 13 个文件。若 `libx265` 缺失，先安装带 `libx265` 的 FFmpeg；测试素材生成工具可以使用 GPL 构建，应用本身只链接 LGPL 库。

- [ ] **Step 2: 写失败测试**

`tests/media/MediaProbeTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
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

void expectSupported(const MediaInfo &info)
{
    EXPECT_TRUE(info.supported) << info.unsupportedReason.toStdString();
    EXPECT_GT(info.width, 0);
    EXPECT_GT(info.height, 0);
    EXPECT_GT(info.durationUs, 0);
    EXPECT_TRUE(info.frameRate.isValid());
}

} // namespace

TEST(MediaProbe, ProbesMpeg4Mp4)
{
    const QString path = fixture(QStringLiteral("mpeg4.mp4"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->container, QStringLiteral("MP4"));
    EXPECT_EQ(info->videoCodec, QStringLiteral("mpeg4"));
    EXPECT_EQ(info->width, 640);
    EXPECT_EQ(info->height, 360);
    EXPECT_TRUE(info->hasAudio);
    expectSupported(*info);
}

TEST(MediaProbe, ProbesH264AndH265)
{
    for (const QString name : {QStringLiteral("h264.mp4"), QStringLiteral("h265.mp4")}) {
        const QString path = fixture(name);
        if (!requireFixture(path)) continue;
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        ASSERT_TRUE(info.has_value()) << error.toStdString();
        EXPECT_EQ(info->videoCodec, name.startsWith(QStringLiteral("h264"))
                                        ? QStringLiteral("h264")
                                        : QStringLiteral("hevc"));
        expectSupported(*info);
    }
}

TEST(MediaProbe, ProbesWmvContainer)
{
    const QString path = fixture(QStringLiteral("wmv2.wmv"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->container, QStringLiteral("WMV"));
    EXPECT_TRUE(info->supported);
    expectSupported(*info);
}

TEST(MediaProbe, ProbesOptionalVc1Sample)
{
    // FFmpeg 没有 VC-1 编码器，无法自动生成素材。放入真实样本后此测试生效。
    const QString path = fixture(QStringLiteral("manual/vc1.wmv"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->videoCodec, QStringLiteral("vc1"));
    EXPECT_TRUE(info->supported);
}

TEST(MediaProbe, ReadsRotationMetadata)
{
    const QString path = fixture(QStringLiteral("rotated.mp4"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->rotationDegrees, 90);
}

TEST(MediaProbe, HandlesUnicodePath)
{
    const QString path = fixture(QStringLiteral("中文 文件名.mp4"));
    if (!requireFixture(path)) return;
    QString error;
    const auto info = MediaProbe::probe(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->videoCodec, QStringLiteral("h264"));
}

TEST(MediaProbe, HandlesNoAudioAndMultipleVideoStreams)
{
    const QString noAudio = fixture(QStringLiteral("noaudio.mp4"));
    if (requireFixture(noAudio)) {
        const auto info = MediaProbe::probe(noAudio, nullptr);
        ASSERT_TRUE(info.has_value());
        EXPECT_FALSE(info->hasAudio);
        EXPECT_TRUE(info->supported);
    }

    const QString twoVideo = fixture(QStringLiteral("twovideo.mp4"));
    if (requireFixture(twoVideo)) {
        const auto info = MediaProbe::probe(twoVideo, nullptr);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->width, 320);
        EXPECT_EQ(info->height, 240);
        EXPECT_TRUE(info->supported);
    }
}

TEST(MediaProbe, HandlesVfrDuration)
{
    const QString path = fixture(QStringLiteral("vfr.mp4"));
    if (!requireFixture(path)) return;
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->supported);
    EXPECT_GT(info->durationUs, 2'000'000);
    EXPECT_LT(info->durationUs, 4'000'000);
}

TEST(MediaProbe, ReportsCorruptAndMissingFiles)
{
    QString error;
    EXPECT_FALSE(MediaProbe::probe(fixture(QStringLiteral("corrupt.mp4")), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("无法打开")));

    error.clear();
    EXPECT_FALSE(MediaProbe::probe(fixture(QStringLiteral("missing.mp4")), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("不存在")));
}
```

`tests/TestMain.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VideoEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VideoEditorTests"));

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

修改 `tests/CMakeLists.txt`，追加：

```cmake
add_executable(media_tests media/MediaProbeTest.cpp TestMain.cpp)
target_link_libraries(media_tests PRIVATE videoeditor_media GTest::gtest Qt6::Core)
target_compile_definitions(media_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(media_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 3: 运行测试确认失败**

```bash
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
```

预期：编译失败，提示 `videoeditor_media` 目标或 `MediaProbe.h` 不存在。

- [ ] **Step 4: 实现错误映射与探测**

`src/media/probe/FfmpegError.h`：

```cpp
#pragma once

#include <QString>

namespace VideoEditor {

QString ffmpegErrorString(int error);
QString cannotOpenMessage(const QString &path, int error);

} // namespace VideoEditor
```

`src/media/probe/FfmpegError.cpp`：

```cpp
#include "FfmpegError.h"

#include <QFileInfo>

extern "C" {
#include <libavutil/error.h>
}

namespace VideoEditor {

QString ffmpegErrorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

QString cannotOpenMessage(const QString &path, int error)
{
    return QStringLiteral("无法打开“%1”：%2\n请确认文件存在且未损坏，或先转码为 H.264/H.265 的 MP4。")
        .arg(QFileInfo(path).fileName(), ffmpegErrorString(error));
}

} // namespace VideoEditor
```

`src/media/probe/MediaProbe.h`：

```cpp
#pragma once

#include <optional>
#include <QString>

#include "MediaInfo.h"

namespace VideoEditor {

class MediaProbe {
public:
    static std::optional<MediaInfo> probe(const QString &path,
                                          QString *error = nullptr);
};

} // namespace VideoEditor
```

`src/media/probe/MediaProbe.cpp`：

```cpp
#include "MediaProbe.h"

#include <cmath>
#include <memory>

#include <QFileInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
}

#include "CodecSupport.h"
#include "FfmpegError.h"

namespace VideoEditor {

namespace {

struct FormatContextDeleter {
    void operator()(AVFormatContext *ctx) const
    {
        if (ctx)
            avformat_close_input(&ctx);
    }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

const AVPacketSideData *findDisplayMatrix(AVStream *stream)
{
    if (auto *side = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                             stream->codecpar->nb_coded_side_data,
                                             AV_PKT_DATA_DISPLAYMATRIX)) {
        return side;
    }
    return av_packet_side_data_get(stream->side_data, stream->nb_side_data,
                                   AV_PKT_DATA_DISPLAYMATRIX);
}

QString containerName(AVFormatContext *format)
{
    const QString name = QString::fromUtf8(format->iformat->name ? format->iformat->name : "");
    if (name.contains(QStringLiteral("mp4")) || name.contains(QStringLiteral("mov")))
        return QStringLiteral("MP4");
    if (name.contains(QStringLiteral("asf")))
        return QStringLiteral("WMV");
    return name;
}

int rotationFromStream(AVStream *stream)
{
    const AVPacketSideData *side = findDisplayMatrix(stream);
    if (!side || !side->data || side->size < 9 * sizeof(int32_t))
        return 0;
    const double counterClockwise = av_display_rotation_get(
        reinterpret_cast<const int32_t *>(side->data));
    if (!std::isfinite(counterClockwise))
        return 0;
    // 转换为顺时针角度，与预览旋转方向一致。
    int normalized = static_cast<int>(std::llround(-counterClockwise)) % 360;
    if (normalized < 0)
        normalized += 360;
    return normalized;
}

} // namespace

std::optional<MediaInfo> MediaProbe::probe(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) {
        setError(error, QStringLiteral("文件不存在：%1").arg(path));
        return std::nullopt;
    }

    AVFormatContext *rawFormat = nullptr;
    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&rawFormat, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, cannotOpenMessage(path, ret));
        return std::nullopt;
    }
    FormatContextPtr format(rawFormat);

    ret = avformat_find_stream_info(format.get(), nullptr);
    if (ret < 0) {
        setError(error, cannotOpenMessage(path, ret));
        return std::nullopt;
    }

    const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO,
                                               -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        setError(error, QStringLiteral("“%1”不包含视频流。").arg(QFileInfo(path).fileName()));
        return std::nullopt;
    }

    AVStream *video = format->streams[videoIndex];
    const AVCodecParameters *videoParams = video->codecpar;
    const QString videoCodec = QString::fromUtf8(avcodec_get_name(videoParams->codec_id));

    MediaInfo info;
    info.path = path;
    info.container = containerName(format.get());
    info.videoCodec = videoCodec;
    info.width = videoParams->width;
    info.height = videoParams->height;
    info.rotationDegrees = rotationFromStream(video);

    AVRational rate = av_guess_frame_rate(format.get(), video, nullptr);
    if (rate.num <= 0 || rate.den <= 0)
        rate = video->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0)
        rate = video->r_frame_rate;
    if (rate.num > 0 && rate.den > 0)
        info.frameRate = Rational{rate.num, rate.den};
    else
        info.frameRate = Rational{0, 1};

    if (video->duration != AV_NOPTS_VALUE) {
        info.durationUs = av_rescale_q(video->duration, video->time_base, AV_TIME_BASE_Q);
    } else if (format->duration != AV_NOPTS_VALUE) {
        info.durationUs = format->duration;
    }

    const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO,
                                               -1, -1, nullptr, 0);
    if (audioIndex >= 0) {
        info.hasAudio = true;
        info.audioCodec = QString::fromUtf8(
            avcodec_get_name(format->streams[audioIndex]->codecpar->codec_id));
    }

    const CodecVerdict verdict = classifyVideoCodec(info.videoCodec);
    info.supported = verdict.supported;
    info.unsupportedReason = verdict.reason;

    if (info.frameRate.isValid() && info.durationUs <= 0) {
        info.supported = false;
        info.unsupportedReason = QStringLiteral("无法读取视频时长，文件可能已损坏。");
    }

    return info;
}

} // namespace VideoEditor
```

`src/media/CMakeLists.txt`：

```cmake
find_package(FFmpeg REQUIRED)

add_library(videoeditor_media STATIC
  probe/MediaProbe.h
  probe/MediaProbe.cpp
  probe/FfmpegError.h
  probe/FfmpegError.cpp
)
target_include_directories(videoeditor_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_media
  PUBLIC videoeditor_core Qt6::Core
  PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
target_compile_definitions(videoeditor_media PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
```

修改 `src/CMakeLists.txt`：

```cmake
add_subdirectory(core)
add_subdirectory(media)
add_subdirectory(app)
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`media_tests` 全部通过；若某个素材缺失，对应测试标记为 skipped 并打印路径。

- [ ] **Step 6: 提交（包含测试素材）**

```bash
git add src/media src/CMakeLists.txt tests tools
git commit -m "feat(media): probe container and codec metadata with FFmpeg"
```

---

### Task 4: 工程模型、撤销、保存与崩溃恢复

**Files:**
- Create: `src/core/Project.h`
- Create: `src/core/ProjectSerializer.h`
- Create: `src/core/ProjectSerializer.cpp`
- Create: `src/core/ProjectStore.h`
- Create: `src/core/ProjectStore.cpp`
- Create: `src/core/ProjectDocument.h`
- Create: `src/core/ProjectDocument.cpp`
- Create: `src/core/commands/AddAssetCommand.h`
- Create: `src/core/commands/AddAssetCommand.cpp`
- Create: `src/core/commands/RemoveAssetCommand.h`
- Create: `src/core/commands/RemoveAssetCommand.cpp`
- Modify: `src/core/CMakeLists.txt`
- Create: `tests/core/ProjectSerializerTest.cpp`
- Create: `tests/core/ProjectDocumentTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Project`、`MediaInfo`、`ProjectSerializer`。
- Produces:
  - `Project { int schemaVersion = 1; QString name; QList<Asset> assets; }`
  - `ProjectSerializer::save/load/toJson/fromJson`
  - `ProjectStore::save/saveRecovery/loadRecovery/discardRecovery`
  - `ProjectDocument::addAsset/removeAsset/undo/redo/save/load/setProject`
  - `QUndoStack *ProjectDocument::undoStack()`

- [ ] **Step 1: 写失败测试**

`tests/core/ProjectSerializerTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "ProjectSerializer.h"
#include "ProjectStore.h"

using namespace VideoEditor;

namespace {

Project sampleProject()
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/示例.mp4");
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.audioCodec = QStringLiteral("aac");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30000, 1001};
    info.durationUs = 12'345'678;
    info.rotationDegrees = 90;
    info.hasAudio = true;
    info.supported = true;

    Project project;
    project.name = QStringLiteral("测试工程");
    project.assets.append(Asset{QStringLiteral("asset-1"), info.path, info, {}});
    return project;
}

} // namespace

TEST(ProjectSerializer, RoundTripsAllFields)
{
    const Project original = sampleProject();
    const QJsonObject json = ProjectSerializer::toJson(original);
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, original);
}

TEST(ProjectSerializer, RejectsNewerSchema)
{
    QJsonObject json = ProjectSerializer::toJson(sampleProject());
    json.insert(QStringLiteral("schemaVersion"), Project::kSchemaVersion + 1);
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(json, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("版本")));
}

TEST(ProjectSerializer, SavesAtomicallyAndLoads)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("project.veproj"));
    const Project original = sampleProject();

    QString error;
    ASSERT_TRUE(ProjectSerializer::save(original, path, &error)) << error.toStdString();
    const auto loaded = ProjectSerializer::load(path, &error);
    ASSERT_TRUE(loaded.has_value()) << error.toStdString();
    EXPECT_EQ(*loaded, original);
}

TEST(ProjectStore, SavesAndLoadsRecovery)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.filePath(QStringLiteral("recovery/project.autosave.json")));

    QString error;
    ASSERT_TRUE(store.saveRecovery(sampleProject(), &error)) << error.toStdString();
    const auto restored = store.loadRecovery(&error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(restored->assets.size(), 1);

    store.discardRecovery();
    EXPECT_FALSE(store.loadRecovery(&error).has_value());
}
```

`tests/core/ProjectDocumentTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include "ProjectDocument.h"

using namespace VideoEditor;

namespace {

Asset makeAsset(int index)
{
    MediaInfo info;
    info.path = QStringLiteral("C:/videos/%1.mp4").arg(index);
    info.container = QStringLiteral("MP4");
    info.videoCodec = QStringLiteral("h264");
    info.width = 1920;
    info.height = 1080;
    info.frameRate = Rational{30, 1};
    info.durationUs = 1'000'000;
    info.supported = true;
    return Asset{QStringLiteral("asset-%1").arg(index), info.path, info, {}};
}

} // namespace

TEST(ProjectDocument, AddRemoveUndoRedo)
{
    ProjectDocument doc;
    doc.addAsset(makeAsset(1));
    doc.addAsset(makeAsset(2));
    EXPECT_EQ(doc.project().assets.size(), 2);
    EXPECT_TRUE(doc.canUndo());

    doc.undo();
    EXPECT_EQ(doc.project().assets.size(), 1);
    EXPECT_TRUE(doc.canRedo());

    doc.redo();
    EXPECT_EQ(doc.project().assets.size(), 2);

    doc.removeAsset(QStringLiteral("asset-1"));
    EXPECT_EQ(doc.project().assets.size(), 1);
    doc.undo();
    EXPECT_EQ(doc.project().assets.size(), 2);
    EXPECT_EQ(doc.project().assets.first().id, QStringLiteral("asset-1"));
}

TEST(ProjectDocument, NewCommandClearsRedo)
{
    ProjectDocument doc;
    doc.addAsset(makeAsset(1));
    doc.undo();
    EXPECT_TRUE(doc.canRedo());
    doc.addAsset(makeAsset(2));
    EXPECT_FALSE(doc.canRedo());
}

TEST(ProjectDocument, UndoLimitKeepsLatestHundred)
{
    ProjectDocument doc;
    for (int i = 0; i < 105; ++i)
        doc.addAsset(makeAsset(i));

    for (int i = 0; i < 100; ++i)
        doc.undo();

    EXPECT_EQ(doc.project().assets.size(), 5);
}
```

修改 `tests/CMakeLists.txt`，把 `ProjectSerializerTest.cpp`、`ProjectDocumentTest.cpp` 加入 `core_tests`。

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
```

预期：编译失败，缺少 `Project.h`、`ProjectDocument.h`。

- [ ] **Step 3: 实现工程数据结构与序列化**

`src/core/Project.h`：

```cpp
#pragma once

#include <QList>
#include <QString>

#include "MediaInfo.h"

namespace VideoEditor {

struct Asset {
    QString id;
    QString path;
    MediaInfo info;
    QString proxyPath;

    bool operator==(const Asset &) const = default;
};

struct Project {
    static constexpr int kSchemaVersion = 1;

    QString name = QStringLiteral("未命名工程");
    QList<Asset> assets;

    bool operator==(const Project &) const = default;
};

} // namespace VideoEditor
```

`src/core/ProjectSerializer.h`：

```cpp
#pragma once

#include <optional>
#include <QJsonObject>
#include <QString>

#include "Project.h"

namespace VideoEditor {

class ProjectSerializer {
public:
    static QJsonObject toJson(const Project &project);
    static std::optional<Project> fromJson(const QJsonObject &json,
                                           QString *error = nullptr);
    static bool save(const Project &project, const QString &path,
                     QString *error = nullptr);
    static std::optional<Project> load(const QString &path,
                                       QString *error = nullptr);
};

} // namespace VideoEditor
```

`src/core/ProjectSerializer.cpp`：

```cpp
#include "ProjectSerializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QJsonObject rationalToJson(const Rational &value)
{
    return {{QStringLiteral("num"), value.num}, {QStringLiteral("den"), value.den}};
}

Rational rationalFromJson(const QJsonObject &json)
{
    return Rational{json.value(QStringLiteral("num")).toInt(0),
                    json.value(QStringLiteral("den")).toInt(1)};
}

QJsonObject infoToJson(const MediaInfo &info)
{
    return {
        {QStringLiteral("path"), info.path},
        {QStringLiteral("container"), info.container},
        {QStringLiteral("videoCodec"), info.videoCodec},
        {QStringLiteral("audioCodec"), info.audioCodec},
        {QStringLiteral("width"), info.width},
        {QStringLiteral("height"), info.height},
        {QStringLiteral("frameRate"), rationalToJson(info.frameRate)},
        {QStringLiteral("durationUs"), static_cast<double>(info.durationUs)},
        {QStringLiteral("rotationDegrees"), info.rotationDegrees},
        {QStringLiteral("hasAudio"), info.hasAudio},
        {QStringLiteral("supported"), info.supported},
        {QStringLiteral("unsupportedReason"), info.unsupportedReason},
    };
}

MediaInfo infoFromJson(const QJsonObject &json)
{
    MediaInfo info;
    info.path = json.value(QStringLiteral("path")).toString();
    info.container = json.value(QStringLiteral("container")).toString();
    info.videoCodec = json.value(QStringLiteral("videoCodec")).toString();
    info.audioCodec = json.value(QStringLiteral("audioCodec")).toString();
    info.width = json.value(QStringLiteral("width")).toInt();
    info.height = json.value(QStringLiteral("height")).toInt();
    info.frameRate = rationalFromJson(
        json.value(QStringLiteral("frameRate")).toObject());
    info.durationUs = static_cast<int64_t>(
        json.value(QStringLiteral("durationUs")).toDouble());
    info.rotationDegrees = json.value(QStringLiteral("rotationDegrees")).toInt();
    info.hasAudio = json.value(QStringLiteral("hasAudio")).toBool();
    info.supported = json.value(QStringLiteral("supported")).toBool();
    info.unsupportedReason = json.value(QStringLiteral("unsupportedReason")).toString();
    return info;
}

} // namespace

QJsonObject ProjectSerializer::toJson(const Project &project)
{
    QJsonArray assets;
    for (const Asset &asset : project.assets) {
        assets.append(QJsonObject{
            {QStringLiteral("id"), asset.id},
            {QStringLiteral("path"), asset.path},
            {QStringLiteral("info"), infoToJson(asset.info)},
            {QStringLiteral("proxyPath"), asset.proxyPath},
        });
    }

    return {
        {QStringLiteral("schemaVersion"), Project::kSchemaVersion},
        {QStringLiteral("name"), project.name},
        {QStringLiteral("assets"), assets},
    };
}

std::optional<Project> ProjectSerializer::fromJson(const QJsonObject &json,
                                                   QString *error)
{
    const int version = json.value(QStringLiteral("schemaVersion")).toInt(0);
    if (version <= 0) {
        setError(error, QStringLiteral("无效的工程文件：缺少 schemaVersion。"));
        return std::nullopt;
    }
    if (version > Project::kSchemaVersion) {
        setError(error, QStringLiteral("工程文件版本过新（%1），请升级应用。").arg(version));
        return std::nullopt;
    }

    Project project;
    project.name = json.value(QStringLiteral("name"))
                       .toString(QStringLiteral("未命名工程"));

    for (const QJsonValue &value : json.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject object = value.toObject();
        Asset asset;
        asset.id = object.value(QStringLiteral("id")).toString();
        asset.path = object.value(QStringLiteral("path")).toString();
        asset.info = infoFromJson(object.value(QStringLiteral("info")).toObject());
        asset.proxyPath = object.value(QStringLiteral("proxyPath")).toString();
        if (asset.id.isEmpty() || asset.path.isEmpty()) {
            setError(error, QStringLiteral("工程文件包含无效素材条目。"));
            return std::nullopt;
        }
        project.assets.append(asset);
    }

    return project;
}

bool ProjectSerializer::save(const Project &project, const QString &path,
                             QString *error)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(error, QStringLiteral("无法创建目录：%1").arg(info.absolutePath()));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("无法写入工程文件：%1").arg(file.errorString()));
        return false;
    }
    file.write(QJsonDocument(toJson(project)).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, QStringLiteral("保存工程失败：%1").arg(file.errorString()));
        return false;
    }
    return true;
}

std::optional<Project> ProjectSerializer::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("无法打开工程文件：%1").arg(file.errorString()));
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("工程文件不是有效的 JSON：%1")
                            .arg(parseError.errorString()));
        return std::nullopt;
    }

    return fromJson(document.object(), error);
}

} // namespace VideoEditor
```

- [ ] **Step 4: 实现恢复存储与工程文档**

`src/core/ProjectStore.h`：

```cpp
#pragma once

#include <optional>
#include <QString>

#include "Project.h"

namespace VideoEditor {

class ProjectStore {
public:
    explicit ProjectStore(QString recoveryFilePath);

    bool save(const Project &project, const QString &file,
              QString *error = nullptr) const;
    std::optional<Project> load(const QString &file,
                                QString *error = nullptr) const;
    bool saveRecovery(const Project &project, QString *error = nullptr) const;
    std::optional<Project> loadRecovery(QString *error = nullptr) const;
    void discardRecovery() const;
    QString recoveryFilePath() const;

private:
    QString m_recoveryFilePath;
};

} // namespace VideoEditor
```

`src/core/ProjectStore.cpp`：

```cpp
#include "ProjectStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "ProjectSerializer.h"

namespace VideoEditor {

ProjectStore::ProjectStore(QString recoveryFilePath)
    : m_recoveryFilePath(std::move(recoveryFilePath))
{
}

bool ProjectStore::save(const Project &project, const QString &file,
                        QString *error) const
{
    return ProjectSerializer::save(project, file, error);
}

std::optional<Project> ProjectStore::load(const QString &file, QString *error) const
{
    return ProjectSerializer::load(file, error);
}

bool ProjectStore::saveRecovery(const Project &project, QString *error) const
{
    return ProjectSerializer::save(project, m_recoveryFilePath, error);
}

std::optional<Project> ProjectStore::loadRecovery(QString *error) const
{
    if (!QFileInfo::exists(m_recoveryFilePath)) {
        if (error)
            *error = QStringLiteral("没有可恢复的工程。");
        return std::nullopt;
    }
    return ProjectSerializer::load(m_recoveryFilePath, error);
}

void ProjectStore::discardRecovery() const
{
    QFile::remove(m_recoveryFilePath);
}

QString ProjectStore::recoveryFilePath() const
{
    return m_recoveryFilePath;
}

} // namespace VideoEditor
```

`src/core/ProjectDocument.h`：

```cpp
#pragma once

#include <optional>
#include <QObject>
#include <QString>
#include <QUndoStack>

#include "Project.h"

namespace VideoEditor {

class AddAssetCommand;
class RemoveAssetCommand;

class ProjectDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)

public:
    explicit ProjectDocument(QObject *parent = nullptr);

    const Project &project() const { return m_project; }
    void setProject(Project project, const QString &filePath = {});

    void addAsset(const Asset &asset);
    void removeAsset(const QString &assetId);
    void setAssetProxyPath(const QString &assetId, const QString &proxyPath);

    bool save(const QString &path, QString *error = nullptr);
    bool load(const QString &path, QString *error = nullptr);

    QString filePath() const { return m_filePath; }
    bool isDirty() const { return !m_undoStack.isClean(); }
    bool canUndo() const { return m_undoStack.canUndo(); }
    bool canRedo() const { return m_undoStack.canRedo(); }
    QUndoStack *undoStack() { return &m_undoStack; }
    void undo() { m_undoStack.undo(); }
    void redo() { m_undoStack.redo(); }

signals:
    void changed();
    void assetsChanged();
    void undoStateChanged();
    void dirtyChanged();

private:
    friend class AddAssetCommand;
    friend class RemoveAssetCommand;

    void insertAssetInternal(const Asset &asset);
    void insertAssetInternalAt(int index, const Asset &asset);
    void removeAssetInternal(const QString &assetId);

    Project m_project;
    QUndoStack m_undoStack;
    QString m_filePath;
};

} // namespace VideoEditor
```

`src/core/ProjectDocument.cpp`：

```cpp
#include "ProjectDocument.h"

#include "ProjectSerializer.h"
#include "commands/AddAssetCommand.h"
#include "commands/RemoveAssetCommand.h"

namespace VideoEditor {

ProjectDocument::ProjectDocument(QObject *parent)
    : QObject(parent)
{
    m_undoStack.setUndoLimit(100);
    connect(&m_undoStack, &QUndoStack::indexChanged, this, [this] {
        emit changed();
        emit assetsChanged();
        emit undoStateChanged();
        emit dirtyChanged();
    });
    connect(&m_undoStack, &QUndoStack::cleanChanged, this, [this] {
        emit dirtyChanged();
    });
}

void ProjectDocument::setProject(Project project, const QString &filePath)
{
    m_project = std::move(project);
    m_filePath = filePath;
    m_undoStack.clear();
    m_undoStack.setClean();
    emit changed();
    emit assetsChanged();
    emit undoStateChanged();
    emit dirtyChanged();
}

void ProjectDocument::addAsset(const Asset &asset)
{
    m_undoStack.push(new AddAssetCommand(this, asset));
}

void ProjectDocument::removeAsset(const QString &assetId)
{
    for (const Asset &asset : m_project.assets) {
        if (asset.id == assetId) {
            m_undoStack.push(new RemoveAssetCommand(this, assetId));
            return;
        }
    }
}

void ProjectDocument::setAssetProxyPath(const QString &assetId,
                                        const QString &proxyPath)
{
    for (Asset &asset : m_project.assets) {
        if (asset.id == assetId) {
            if (asset.proxyPath == proxyPath)
                return;
            asset.proxyPath = proxyPath;
            emit changed();
            emit assetsChanged();
            emit dirtyChanged();
            return;
        }
    }
}

bool ProjectDocument::save(const QString &path, QString *error)
{
    if (!ProjectSerializer::save(m_project, path, error))
        return false;
    m_filePath = path;
    m_undoStack.setClean();
    emit dirtyChanged();
    return true;
}

bool ProjectDocument::load(const QString &path, QString *error)
{
    auto project = ProjectSerializer::load(path, error);
    if (!project)
        return false;
    setProject(std::move(*project), path);
    return true;
}

void ProjectDocument::insertAssetInternal(const Asset &asset)
{
    m_project.assets.append(asset);
}

void ProjectDocument::insertAssetInternalAt(int index, const Asset &asset)
{
    const int bounded = qBound(0, index, int(m_project.assets.size()));
    m_project.assets.insert(bounded, asset);
}

void ProjectDocument::removeAssetInternal(const QString &assetId)
{
    for (int i = 0; i < m_project.assets.size(); ++i) {
        if (m_project.assets.at(i).id == assetId) {
            m_project.assets.removeAt(i);
            return;
        }
    }
}

} // namespace VideoEditor
```

`src/core/commands/AddAssetCommand.h`：

```cpp
#pragma once

#include <QUndoCommand>

#include "ProjectDocument.h"

namespace VideoEditor {

class AddAssetCommand : public QUndoCommand {
public:
    AddAssetCommand(ProjectDocument *document, Asset asset)
        : m_document(document)
        , m_asset(std::move(asset))
    {
        setText(QStringLiteral("添加素材"));
    }

    void redo() override { m_document->insertAssetInternal(m_asset); }
    void undo() override { m_document->removeAssetInternal(m_asset.id); }

private:
    ProjectDocument *m_document;
    Asset m_asset;
};

} // namespace VideoEditor
```

`src/core/commands/AddAssetCommand.cpp`：

```cpp
#include "AddAssetCommand.h"
```

`src/core/commands/RemoveAssetCommand.h`：

```cpp
#pragma once

#include <QUndoCommand>

#include "ProjectDocument.h"

namespace VideoEditor {

class RemoveAssetCommand : public QUndoCommand {
public:
    RemoveAssetCommand(ProjectDocument *document, QString assetId);
    void redo() override;
    void undo() override;

private:
    ProjectDocument *m_document;
    QString m_assetId;
    Asset m_asset;
    int m_index = 0;
};

} // namespace VideoEditor
```

`src/core/commands/RemoveAssetCommand.cpp`：

```cpp
#include "RemoveAssetCommand.h"

namespace VideoEditor {

RemoveAssetCommand::RemoveAssetCommand(ProjectDocument *document, QString assetId)
    : m_document(document)
    , m_assetId(std::move(assetId))
{
    setText(QStringLiteral("删除素材"));
    const auto &assets = m_document->project().assets;
    for (int i = 0; i < assets.size(); ++i) {
        if (assets.at(i).id == m_assetId) {
            m_asset = assets.at(i);
            m_index = i;
            break;
        }
    }
}

void RemoveAssetCommand::redo()
{
    m_document->removeAssetInternal(m_assetId);
}

void RemoveAssetCommand::undo()
{
    m_document->insertAssetInternalAt(m_index, m_asset);
}

} // namespace VideoEditor
```

修改 `src/core/CMakeLists.txt`，加入新源文件：

```cmake
add_library(videoeditor_core STATIC
  AppInfo.h
  AppInfo.cpp
  TimeBase.h
  TimeBase.cpp
  MediaInfo.h
  CodecSupport.h
  CodecSupport.cpp
  Project.h
  ProjectSerializer.h
  ProjectSerializer.cpp
  ProjectStore.h
  ProjectStore.cpp
  ProjectDocument.h
  ProjectDocument.cpp
  commands/AddAssetCommand.h
  commands/AddAssetCommand.cpp
  commands/RemoveAssetCommand.h
  commands/RemoveAssetCommand.cpp
)
target_include_directories(videoeditor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_core PUBLIC Qt6::Core Qt6::Gui)
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`core_tests` 中序列化、撤销、恢复测试全部通过。

- [ ] **Step 6: 提交**

```bash
git add src/core tests
git commit -m "feat(core): add project model, undo stack, save and recovery"
```

---

### Task 5: 媒体库模型与导入流程

**Files:**
- Create: `src/ui/CMakeLists.txt`
- Create: `src/ui/MediaListModel.h`
- Create: `src/ui/MediaListModel.cpp`
- Create: `src/app/AppController.h`
- Create: `src/app/AppController.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/app/main.cpp`
- Modify: `src/app/Main.qml`
- Modify: `src/CMakeLists.txt`
- Create: `tests/app/AppControllerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MediaProbe`、`ProjectDocument`、`ProjectStore`。
- Produces:
  - `MediaListModel` 角色：`idRole`、`nameRole`、`pathRole`、`codecRole`、`resolutionRole`、`durationRole`、`compatibleRole`、`reasonRole`、`proxyPathRole`。
  - `AppController::importUrls(QList<QUrl>)`、`lastError`、`clearError()`、`restoreRecovery()`、`discardRecovery()`。

- [ ] **Step 1: 写失败测试**

`tests/app/AppControllerTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QUrl>

#include "AppController.h"
#include "MediaListModel.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(AppController, ImportsSupportedFile)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_TRUE(controller.lastError().isEmpty());
}

TEST(AppController, ReportsUnsupportedOrMissingFile)
{
    AppController controller;
    controller.importUrls({QUrl::fromLocalFile(fixture(QStringLiteral("corrupt.mp4")))});
    EXPECT_EQ(controller.mediaModel()->rowCount(), 0);
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("无法打开")));

    controller.clearError();
    controller.importUrls({QUrl::fromLocalFile(fixture(QStringLiteral("missing.mp4")))});
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("不存在")));
}

TEST(AppController, UndoRemovesImportedAsset)
{
    AppController controller;
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    controller.undo();
    EXPECT_EQ(controller.mediaModel()->rowCount(), 0);
    controller.redo();
    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
}
```

修改 `tests/CMakeLists.txt`，追加：

```cmake
add_executable(app_tests app/AppControllerTest.cpp TestMain.cpp)
target_link_libraries(app_tests PRIVATE videoeditor_app GTest::gtest Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(app_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(app_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
```

预期：编译失败，缺少 `AppController.h`。

- [ ] **Step 3: 实现媒体列表模型**

`src/ui/MediaListModel.h`：

```cpp
#pragma once

#include <QAbstractListModel>
#include <QList>

#include "Project.h"

namespace VideoEditor {

class MediaListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PathRole,
        CodecRole,
        ResolutionRole,
        DurationRole,
        CompatibleRole,
        ReasonRole,
        ProxyPathRole
    };

    explicit MediaListModel(QObject *parent = nullptr);

    void setAssets(const QList<Asset> &assets);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    QList<Asset> m_assets;
};

} // namespace VideoEditor
```

`src/ui/MediaListModel.cpp`：

```cpp
#include "MediaListModel.h"

#include <QFileInfo>

#include "CodecSupport.h"
#include "TimeBase.h"

namespace VideoEditor {

MediaListModel::MediaListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void MediaListModel::setAssets(const QList<Asset> &assets)
{
    beginResetModel();
    m_assets = assets;
    endResetModel();
}

int MediaListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_assets.size());
}

QVariant MediaListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_assets.size())
        return {};

    const Asset &asset = m_assets.at(index.row());
    switch (role) {
    case IdRole:
        return asset.id;
    case NameRole:
        return QFileInfo(asset.path).fileName();
    case PathRole:
        return asset.path;
    case CodecRole:
        return friendlyCodecName(classifyVideoCodec(asset.info.videoCodec).kind);
    case ResolutionRole:
        return QStringLiteral("%1×%2").arg(asset.info.width).arg(asset.info.height);
    case DurationRole:
        return formatDuration(asset.info.durationUs);
    case CompatibleRole:
        return asset.info.supported;
    case ReasonRole:
        return asset.info.unsupportedReason;
    case ProxyPathRole:
        return asset.proxyPath;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaListModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {NameRole, "name"},
        {PathRole, "path"},
        {CodecRole, "codec"},
        {ResolutionRole, "resolution"},
        {DurationRole, "duration"},
        {CompatibleRole, "compatible"},
        {ReasonRole, "reason"},
        {ProxyPathRole, "proxyPath"},
    };
}

} // namespace VideoEditor
```

- [ ] **Step 4: 实现 AppController**

`src/app/AppController.h`：

```cpp
#pragma once

#include <memory>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include "ProjectDocument.h"
#include "ProjectStore.h"

namespace VideoEditor {

class MediaListModel;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(MediaListModel *mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryAvailableChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    MediaListModel *mediaModel() const { return m_mediaModel; }
    QString lastError() const { return m_lastError; }
    bool canUndo() const;
    bool canRedo() const;
    bool recoveryAvailable() const { return m_recoveryAvailable; }

    Q_INVOKABLE void importUrls(const QList<QUrl> &urls);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void restoreRecovery();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void saveProject(const QUrl &url);
    Q_INVOKABLE void openProject(const QUrl &url);

    ProjectDocument *document() const { return m_document; }
    ProjectStore *store() const { return m_store.get(); }

signals:
    void lastErrorChanged();
    void undoStateChanged();
    void recoveryAvailableChanged();
    void projectChanged();

private:
    void setLastError(const QString &message);
    void refreshModel();
    void refreshRecoveryState();
    void autosave();

    ProjectDocument *m_document = nullptr;
    MediaListModel *m_mediaModel = nullptr;
    std::unique_ptr<ProjectStore> m_store;
    QTimer m_autosaveTimer;
    QString m_lastError;
    bool m_recoveryAvailable = false;
};

} // namespace VideoEditor
```

`src/app/AppController.cpp`：

```cpp
#include "AppController.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include "CodecSupport.h"
#include "MediaListModel.h"
#include "MediaProbe.h"

namespace VideoEditor {

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_document(new ProjectDocument(this))
    , m_mediaModel(new MediaListModel(this))
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    const QString recoveryPath = QDir(appData).filePath(
        QStringLiteral("recovery/project.autosave.json"));
    m_store = std::make_unique<ProjectStore>(recoveryPath);

    connect(m_document, &ProjectDocument::assetsChanged,
            this, &AppController::refreshModel);
    connect(m_document, &ProjectDocument::undoStateChanged,
            this, &AppController::undoStateChanged);
    connect(m_document, &ProjectDocument::dirtyChanged,
            this, &AppController::projectChanged);

    m_autosaveTimer.setInterval(30'000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &AppController::autosave);
    m_autosaveTimer.start();

    refreshModel();
    refreshRecoveryState();
}

AppController::~AppController() = default;

bool AppController::canUndo() const
{
    return m_document->canUndo();
}

bool AppController::canRedo() const
{
    return m_document->canRedo();
}

void AppController::importUrls(const QList<QUrl> &urls)
{
    QStringList failures;

    for (const QUrl &url : urls) {
        const QString path = url.toLocalFile();
        QString error;
        const auto info = MediaProbe::probe(path, &error);
        if (!info) {
            failures.append(error);
            continue;
        }
        if (!info->supported) {
            failures.append(info->unsupportedReason);
            continue;
        }

        Asset asset;
        asset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        asset.path = path;
        asset.info = *info;
        m_document->addAsset(asset);
    }

    if (failures.isEmpty())
        clearError();
    else
        setLastError(failures.join(QLatin1Char('\n')));
}

void AppController::undo()
{
    m_document->undo();
}

void AppController::redo()
{
    m_document->redo();
}

void AppController::clearError()
{
    if (m_lastError.isEmpty())
        return;
    m_lastError.clear();
    emit lastErrorChanged();
}

void AppController::restoreRecovery()
{
    QString error;
    auto project = m_store->loadRecovery(&error);
    if (!project) {
        setLastError(error);
        return;
    }
    m_document->setProject(std::move(*project));
    m_store->discardRecovery();
    refreshRecoveryState();
    clearError();
}

void AppController::discardRecovery()
{
    m_store->discardRecovery();
    refreshRecoveryState();
}

void AppController::saveProject(const QUrl &url)
{
    QString error;
    if (!m_document->save(url.toLocalFile(), &error))
        setLastError(error);
    else
        clearError();
}

void AppController::openProject(const QUrl &url)
{
    QString error;
    if (!m_document->load(url.toLocalFile(), &error))
        setLastError(error);
    else
        clearError();
}

void AppController::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

void AppController::refreshModel()
{
    m_mediaModel->setAssets(m_document->project().assets);
}

void AppController::refreshRecoveryState()
{
    const bool available = QFileInfo::exists(m_store->recoveryFilePath());
    if (available == m_recoveryAvailable)
        return;
    m_recoveryAvailable = available;
    emit recoveryAvailableChanged();
}

void AppController::autosave()
{
    if (!m_document->isDirty())
        return;
    QString error;
    if (!m_store->saveRecovery(m_document->project(), &error)) {
        setLastError(error);
        return;
    }
    refreshRecoveryState();
}

} // namespace VideoEditor
```

`src/ui/CMakeLists.txt`：

```cmake
add_library(videoeditor_ui STATIC
  MediaListModel.h
  MediaListModel.cpp
)
target_include_directories(videoeditor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_ui PUBLIC videoeditor_core Qt6::Core)
```

`src/app/CMakeLists.txt`：

```cmake
add_library(videoeditor_app STATIC
  AppController.h
  AppController.cpp
)
target_include_directories(videoeditor_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_app PUBLIC videoeditor_core videoeditor_media videoeditor_ui Qt6::Core)

qt_add_executable(VideoEditor main.cpp)
qt_add_qml_module(VideoEditor
  URI VideoEditor
  VERSION 1.0
  QML_FILES Main.qml
)
target_link_libraries(VideoEditor PRIVATE Qt6::Quick Qt6::QuickControls2 videoeditor_app)

if(WIN32 AND FFMPEG_ROOT)
  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  add_custom_command(TARGET VideoEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:VideoEditor>")
endif()
```

修改 `src/CMakeLists.txt`：

```cmake
add_subdirectory(core)
add_subdirectory(media)
add_subdirectory(ui)
add_subdirectory(app)
```

- [ ] **Step 5: 接入界面**

`src/app/main.cpp` 修改为：

```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include "AppController.h"
#include "AppInfo.h"

int main(int argc, char *argv[])
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("VideoEditor"));
    QGuiApplication::setApplicationName(QStringLiteral("VideoEditor"));

    VideoEditor::AppController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             VideoEditor::appVersion());
    engine.loadFromModule(QStringLiteral("VideoEditor"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
```

`src/app/Main.qml` 修改为：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "VideoEditor " + appVersion
    color: "#111111"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Button { text: "导入视频"; onClicked: fileDialog.open() }
            Button { text: "撤销"; enabled: App.canUndo; onClicked: App.undo() }
            Button { text: "重做"; enabled: App.canRedo; onClicked: App.redo() }
            Item { Layout.fillWidth: true }
            Label { text: "M1 媒体基础"; color: "#cccccc" }
        }
    }

    ListView {
        id: mediaList
        anchors.fill: parent
        model: App.mediaModel
        delegate: ItemDelegate {
            width: ListView.view.width
            text: model.name + "\n" + model.codec + " · " + model.resolution
                  + " · " + model.duration
        }

        Label {
            anchors.centerIn: parent
            visible: mediaList.count === 0
            text: "媒体库为空，点击“导入视频”开始"
            color: "#888888"
        }
    }

    Rectangle {
        visible: App.lastError.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 72
        color: "#7a1f1f"
        z: 10

        Label {
            anchors.fill: parent
            anchors.margins: 8
            text: App.lastError
            color: "white"
            wrapMode: Text.WordWrap
        }

        MouseArea {
            anchors.fill: parent
            onClicked: App.clearError()
        }
    }

    FileDialog {
        id: fileDialog
        title: "导入视频"
        nameFilters: ["视频文件 (*.mp4 *.wmv)", "所有文件 (*)"]
        fileMode: FileDialog.OpenFiles
        onAccepted: App.importUrls(selectedFiles)
    }

    Dialog {
        id: recoveryDialog
        visible: App.recoveryAvailable
        title: "发现未保存的工程"
        modal: true
        standardButtons: Dialog.NoButton

        RowLayout {
            Button {
                text: "恢复"
                onClicked: { App.restoreRecovery(); recoveryDialog.close() }
            }
            Button {
                text: "丢弃"
                onClicked: { App.discardRecovery(); recoveryDialog.close() }
            }
        }
    }
}
```

- [ ] **Step 6: 构建并运行测试**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`app_tests` 通过；导入成功、损坏文件提示、撤销/重做均符合断言。

- [ ] **Step 7: 手动检查界面**

```powershell
windeployqt --qmldir src/app build/windows-x64-debug/src/app/VideoEditor.exe
./build/windows-x64-debug/src/app/VideoEditor.exe
```

预期：点击“导入视频”选择 `tests/fixtures/h264.mp4`，列表出现文件名、`H.264`、`640×360`、时长；选择 `corrupt.mp4` 时底部出现红色错误提示，点击提示可关闭。

- [ ] **Step 8: 提交**

```bash
git add src tests
git commit -m "feat(app): import media and show library with actionable errors"
```

---

### Task 6: D3D11 设备、共享纹理与 NV12 转换

**Files:**
- Create: `src/media/decode/D3D11Context.h`
- Create: `src/media/decode/D3D11Context.cpp`
- Create: `src/media/decode/SharedTextureRing.h`
- Create: `src/media/decode/SharedTextureRing.cpp`
- Create: `src/media/decode/Shaders.h`
- Create: `src/media/decode/Nv12Converter.h`
- Create: `src/media/decode/Nv12Converter.cpp`
- Create: `src/media/decode/RgbaConverter.h`
- Create: `src/media/decode/RgbaConverter.cpp`
- Create: `src/media/decode/FrameReadback.h`
- Create: `src/media/decode/FrameReadback.cpp`
- Modify: `src/media/CMakeLists.txt`
- Create: `tests/media/D3D11PipelineTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: 无（GPU 基础层）。
- Produces:
  - `D3D11Context::create(bool enableVideo, QString*)`
  - `SharedTextureRing::create(int w, int h, int count)`
  - `SharedTextureRing::sharedHandle(int)`
  - `Nv12Converter::convert(ID3D11Texture2D*, UINT subresource, int w, int h, ID3D11RenderTargetView*, float rotationRadians, QString*)`
  - `RgbaConverter::convert(ID3D11Texture2D* rgba, int w, int h, ID3D11RenderTargetView*, float rotationRadians, QString*)`
  - `readbackSharedTexture(D3D11Context&, ID3D11Texture2D*, QString*) -> std::vector<uint8_t>`

- [ ] **Step 1: 写失败测试**

`tests/media/D3D11PipelineTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <QString>

#include "D3D11Context.h"
#include "FrameReadback.h"
#include "Nv12Converter.h"
#include "RgbaConverter.h"
#include "SharedTextureRing.h"

using namespace VideoEditor;
using Microsoft::WRL::ComPtr;

namespace {

std::unique_ptr<D3D11Context> requireContext()
{
    QString error;
    auto context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();
    return context;
}

// 创建两张 R8 / R8G8 纹理，模拟 NV12 的两个平面。
bool makeNv12Planes(ID3D11Device *device, int width, int height,
                    uint8_t y, uint8_t u, uint8_t v,
                    ID3D11Texture2D **luma, ID3D11Texture2D **chroma)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Format = DXGI_FORMAT_R8_UNORM;

    std::vector<uint8_t> yPlane(size_t(width) * height, y);
    D3D11_SUBRESOURCE_DATA yData{};
    yData.pSysMem = yPlane.data();
    yData.SysMemPitch = UINT(width);
    if (FAILED(device->CreateTexture2D(&desc, &yData, luma)))
        return false;

    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    std::vector<uint8_t> uvPlane(size_t(width) * height * 2, 0);
    for (int i = 0; i < width * height; ++i) {
        uvPlane[size_t(i) * 2] = u;
        uvPlane[size_t(i) * 2 + 1] = v;
    }
    D3D11_SUBRESOURCE_DATA uvData{};
    uvData.pSysMem = uvPlane.data();
    uvData.SysMemPitch = UINT(width * 2);
    return SUCCEEDED(device->CreateTexture2D(&desc, &uvData, chroma));
}

} // namespace

TEST(D3D11Pipeline, CreatesSharedTextureRing)
{
    auto context = requireContext();
    SharedTextureRing ring(*context);
    QString error;
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    EXPECT_NE(ring.sharedHandle(0), nullptr);
    EXPECT_NE(ring.sharedHandle(1), nullptr);
    EXPECT_NE(ring.sharedHandle(2), nullptr);
}

TEST(D3D11Pipeline, ConvertsNv12RedToRgba)
{
    auto context = requireContext();
    ID3D11Device *device = context->device();
    ID3D11DeviceContext *deviceContext = context->context();

    ID3D11Texture2D *luma = nullptr;
    ID3D11Texture2D *chroma = nullptr;
    ASSERT_TRUE(makeNv12Planes(device, 64, 64, 63, 102, 240, &luma, &chroma));

    SharedTextureRing ring(*context);
    QString error;
    ASSERT_TRUE(ring.create(64, 64, 1, &error)) << error.toStdString();

    Nv12Converter converter;
    ASSERT_TRUE(converter.initialize(*context, &error)) << error.toStdString();

    ComPtr<IDXGIKeyedMutex> mutex;
    ring.texture(0)->QueryInterface(IID_PPV_ARGS(&mutex));
    ASSERT_NE(mutex, nullptr);
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);
    ASSERT_TRUE(converter.convert(luma, chroma, 64, 64, ring.rtv(0), 0.0f, &error))
        << error.toStdString();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(64 * 64 * 4)) << error.toStdString();
    EXPECT_GT(rgba[0], 200);
    EXPECT_LT(rgba[1], 60);
    EXPECT_LT(rgba[2], 60);
    EXPECT_EQ(rgba[3], 255);

    luma->Release();
    chroma->Release();
    deviceContext->Flush();
}

TEST(D3D11Pipeline, ConvertsRgbaBuffer)
{
    auto context = requireContext();
    ID3D11Device *device = context->device();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 32;
    desc.Height = 32;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<uint8_t> pixels(32 * 32 * 4, 255);
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = 32 * 4;

    ComPtr<ID3D11Texture2D> source;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &source)));

    SharedTextureRing ring(*context);
    QString error;
    ASSERT_TRUE(ring.create(32, 32, 1, &error)) << error.toStdString();

    RgbaConverter converter;
    ASSERT_TRUE(converter.initialize(*context, &error)) << error.toStdString();

    ComPtr<IDXGIKeyedMutex> mutex;
    ring.texture(0)->QueryInterface(IID_PPV_ARGS(&mutex));
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);
    ASSERT_TRUE(converter.convert(source.Get(), 32, 32, ring.rtv(0), 0.0f, &error))
        << error.toStdString();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(32 * 32 * 4));
    EXPECT_GT(rgba[0], 200);
    EXPECT_GT(rgba[1], 200);
    EXPECT_GT(rgba[2], 200);
}
```

修改 `tests/CMakeLists.txt`，追加：

```cmake
add_executable(media_tests media/MediaProbeTest.cpp media/D3D11PipelineTest.cpp TestMain.cpp)
```

并把 `media_tests` 的链接改为：

```cmake
target_link_libraries(media_tests PRIVATE
  videoeditor_media GTest::gtest Qt6::Core Qt6::Gui d3d11 dxgi d3dcompiler)
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-debug
```

预期：编译失败，缺少 `D3D11Context.h` 等。

- [ ] **Step 3: 实现 D3D11 设备与共享纹理**

`src/media/decode/D3D11Context.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context {
public:
    static std::unique_ptr<D3D11Context> create(bool enableVideo,
                                                QString *error = nullptr);

    ID3D11Device *device() const { return m_device.Get(); }
    ID3D11DeviceContext *context() const { return m_context.Get(); }
    ID3D11VideoDevice *videoDevice() const { return m_videoDevice.Get(); }
    bool supportsVideoDecoding() const { return m_videoDevice != nullptr; }
    QString adapterName() const { return m_adapterName; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    QString m_adapterName;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/D3D11Context.cpp`：

```cpp
#include "D3D11Context.h"

#ifdef _WIN32

#include <d3d10.h>
#include <wrl/client.h>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

std::unique_ptr<D3D11Context> D3D11Context::create(bool enableVideo, QString *error)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (enableVideo)
        flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actualLevel{};

    auto context = std::unique_ptr<D3D11Context>(new D3D11Context);
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, 2, D3D11_SDK_VERSION,
                                   &context->m_device, &actualLevel,
                                   &context->m_context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, &levels[1], 1, D3D11_SDK_VERSION,
                               &context->m_device, &actualLevel,
                               &context->m_context);
    }
    if (FAILED(hr) && !enableVideo) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               flags, levels, 2, D3D11_SDK_VERSION,
                               &context->m_device, &actualLevel,
                               &context->m_context);
    }
    if (FAILED(hr)) {
        setError(error, QStringLiteral("无法创建 D3D11 设备（0x%1）。")
                            .arg(quint32(hr), 8, 16, QLatin1Char('0')));
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context->m_context.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    if (enableVideo)
        context->m_device.As(&context->m_videoDevice);

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(context->m_device.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
                context->m_adapterName = QString::fromWCharArray(desc.Description);
        }
    }

    return context;
}

} // namespace VideoEditor

#endif
```

`src/media/decode/SharedTextureRing.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <array>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context;

class SharedTextureRing {
public:
    static constexpr int kSlotCount = 3;

    explicit SharedTextureRing(D3D11Context &context);

    bool create(int width, int height, int count = kSlotCount,
                QString *error = nullptr);
    bool isValid() const { return m_valid; }
    int count() const { return m_count; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    ID3D11Texture2D *texture(int index) const;
    ID3D11RenderTargetView *rtv(int index) const;
    IDXGIKeyedMutex *mutex(int index) const;
    HANDLE sharedHandle(int index) const;

    int nextWriteIndex() const { return m_nextWrite; }
    void advanceWriteIndex() { m_nextWrite = (m_nextWrite + 1) % m_count; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
        HANDLE handle = nullptr;
    };

    D3D11Context &m_context;
    std::array<Slot, kSlotCount> m_slots;
    int m_count = 0;
    int m_width = 0;
    int m_height = 0;
    int m_nextWrite = 0;
    bool m_valid = false;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/SharedTextureRing.cpp`：

```cpp
#include "SharedTextureRing.h"

#ifdef _WIN32

#include <d3d11.h>

#include "D3D11Context.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

SharedTextureRing::SharedTextureRing(D3D11Context &context)
    : m_context(context)
{
}

bool SharedTextureRing::create(int width, int height, int count, QString *error)
{
    if (count <= 0 || count > kSlotCount) {
        setError(error, QStringLiteral("共享纹理数量无效：%1").arg(count));
        return false;
    }

    for (int i = 0; i < count; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(width);
        desc.Height = UINT(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED
                         | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = m_context.device()->CreateTexture2D(
            &desc, nullptr, &m_slots[i].texture);
        if (FAILED(hr)) {
            setError(error, QStringLiteral("无法创建共享纹理（0x%1）。")
                                .arg(quint32(hr), 8, 16, QLatin1Char('0')));
            return false;
        }

        if (FAILED(m_context.device()->CreateRenderTargetView(
                m_slots[i].texture.Get(), nullptr, &m_slots[i].rtv))) {
            setError(error, QStringLiteral("无法创建渲染目标视图。"));
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        if (FAILED(m_slots[i].texture.As(&resource))
            || FAILED(resource->GetSharedHandle(&m_slots[i].handle))) {
            setError(error, QStringLiteral("无法获取共享纹理句柄。"));
            return false;
        }

        if (FAILED(m_slots[i].texture.As(&m_slots[i].mutex))) {
            setError(error, QStringLiteral("无法获取键控互斥量。"));
            return false;
        }
    }

    m_count = count;
    m_width = width;
    m_height = height;
    m_nextWrite = 0;
    m_valid = true;
    return true;
}

ID3D11Texture2D *SharedTextureRing::texture(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].texture.Get() : nullptr;
}

ID3D11RenderTargetView *SharedTextureRing::rtv(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].rtv.Get() : nullptr;
}

IDXGIKeyedMutex *SharedTextureRing::mutex(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].mutex.Get() : nullptr;
}

HANDLE SharedTextureRing::sharedHandle(int index) const
{
    return (index >= 0 && index < m_count) ? m_slots[index].handle : nullptr;
}

} // namespace VideoEditor

#endif
```

- [ ] **Step 4: 实现 shader 与转换器**

`src/media/decode/Shaders.h`：

```cpp
#pragma once

namespace VideoEditor::shaders {

inline constexpr const char *kFullscreenVs = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
    return o;
}
)";

inline constexpr const char *kRotateAndSample = R"(
cbuffer Params : register(b0)
{
    float rotation;
    float3 padding;
};

float2 rotateUv(float2 uv)
{
    float2 centered = uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(c * centered.x - s * centered.y,
                  s * centered.x + c * centered.y) + 0.5;
}
)";

inline constexpr const char *kNv12Ps = R"(
Texture2D<float>  luma   : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState      samp   : register(s0);

cbuffer Params : register(b0)
{
    float rotation;
    float3 padding;
};

float2 rotateUv(float2 uv)
{
    float2 centered = uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(c * centered.x - s * centered.y,
                  s * centered.x + c * centered.y) + 0.5;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float2 rotated = rotateUv(uv);
    float y = (luma.Sample(samp, rotated).r - 16.0 / 255.0) * (255.0 / 219.0);
    float2 c = chroma.Sample(samp, rotated);
    float u = (c.x - 128.0 / 255.0) * (255.0 / 224.0);
    float v = (c.y - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb = float3(y + 1.5748 * v,
                        y - 0.1873 * u - 0.4681 * v,
                        y + 1.8556 * u);
    return float4(saturate(rgb), 1.0);
}
)";

inline constexpr const char *kRgbaPs = R"(
Texture2D<float4> src  : register(t0);
SamplerState      samp : register(s0);

cbuffer Params : register(b0)
{
    float rotation;
    float3 padding;
};

float2 rotateUv(float2 uv)
{
    float2 centered = uv - 0.5;
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(c * centered.x - s * centered.y,
                  s * centered.x + c * centered.y) + 0.5;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return src.Sample(samp, rotateUv(uv));
}
)";

} // namespace VideoEditor::shaders
```

`src/media/decode/Nv12Converter.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context;

class Nv12Converter {
public:
    bool initialize(D3D11Context &context, QString *error = nullptr);
    bool convert(ID3D11Texture2D *nv12, UINT subresource, int width, int height,
                 ID3D11RenderTargetView *target, float rotationRadians,
                 QString *error = nullptr);
    bool convert(ID3D11Texture2D *luma, ID3D11Texture2D *chroma,
                 int width, int height, ID3D11RenderTargetView *target,
                 float rotationRadians, QString *error = nullptr);

private:
    bool draw(ID3D11ShaderResourceView *lumaSrv,
              ID3D11ShaderResourceView *chromaSrv,
              int width, int height, ID3D11RenderTargetView *target,
              float rotationRadians);

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_params;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/Nv12Converter.cpp`：

```cpp
#include "Nv12Converter.h"

#ifdef _WIN32

#include <cstring>
#include <d3dcompiler.h>

#include "D3D11Context.h"
#include "Shaders.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

Microsoft::WRL::ComPtr<ID3DBlob> compile(const char *source,
                                         const char *entry,
                                         const char *target,
                                         QString *error)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                                  entry, target, flags, 0, &blob, &diagnostics);
    if (FAILED(hr)) {
        const QString detail = diagnostics
            ? QString::fromUtf8(static_cast<const char *>(diagnostics->GetBufferPointer()),
                                int(diagnostics->GetBufferSize()))
            : QStringLiteral("unknown shader error");
        setError(error, QStringLiteral("着色器编译失败：%1").arg(detail));
    }
    return blob;
}

} // namespace

bool Nv12Converter::initialize(D3D11Context &context, QString *error)
{
    m_device = context.device();
    m_context = context.context();

    auto vsBlob = compile(shaders::kFullscreenVs, "main", "vs_5_0", error);
    auto psBlob = compile(shaders::kNv12Ps, "main", "ps_5_0", error);
    if (!vsBlob || !psBlob)
        return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(), nullptr, &m_vs))
        || FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                              psBlob->GetBufferSize(), nullptr, &m_ps))) {
        setError(error, QStringLiteral("无法创建着色器对象。"));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&samplerDesc, &m_sampler))) {
        setError(error, QStringLiteral("无法创建采样器。"));
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 16;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_params))) {
        setError(error, QStringLiteral("无法创建参数缓冲区。"));
        return false;
    }

    return true;
}

bool Nv12Converter::convert(ID3D11Texture2D *nv12, UINT subresource,
                            int width, int height, ID3D11RenderTargetView *target,
                            float rotationRadians, QString *error)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MostDetailedMip = 0;
    desc.Texture2DArray.MipLevels = 1;
    desc.Texture2DArray.FirstArraySlice = subresource;
    desc.Texture2DArray.ArraySize = 1;

    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Texture2DArray.PlaneSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaSrv;
    if (FAILED(m_device->CreateShaderResourceView(nv12, &desc, &lumaSrv))) {
        setError(error, QStringLiteral("无法创建亮度平面视图。"));
        return false;
    }

    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    desc.Texture2DArray.PlaneSlice = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaSrv;
    if (FAILED(m_device->CreateShaderResourceView(nv12, &desc, &chromaSrv))) {
        setError(error, QStringLiteral("无法创建色度平面视图。"));
        return false;
    }

    return draw(lumaSrv.Get(), chromaSrv.Get(), width, height, target,
                rotationRadians);
}

bool Nv12Converter::convert(ID3D11Texture2D *luma, ID3D11Texture2D *chroma,
                            int width, int height, ID3D11RenderTargetView *target,
                            float rotationRadians, QString *error)
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaSrv;
    if (FAILED(m_device->CreateShaderResourceView(luma, nullptr, &lumaSrv))
        || FAILED(m_device->CreateShaderResourceView(chroma, nullptr, &chromaSrv))) {
        setError(error, QStringLiteral("无法创建测试平面视图。"));
        return false;
    }
    return draw(lumaSrv.Get(), chromaSrv.Get(), width, height, target,
                rotationRadians);
}

bool Nv12Converter::draw(ID3D11ShaderResourceView *lumaSrv,
                         ID3D11ShaderResourceView *chromaSrv,
                         int width, int height, ID3D11RenderTargetView *target,
                         float rotationRadians)
{
    const float params[4] = {rotationRadians, 0.0f, 0.0f, 0.0f};
    m_context->UpdateSubresource(m_params.Get(), 0, nullptr, params, 0, 0);

    ID3D11RenderTargetView *targets[] = {target};
    m_context->OMSetRenderTargets(1, targets, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    ID3D11ShaderResourceView *srvs[] = {lumaSrv, chromaSrv};
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);
    return true;
}

} // namespace VideoEditor

#endif
```

`src/media/decode/RgbaConverter.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>

#include <QString>

namespace VideoEditor {

class D3D11Context;

class RgbaConverter {
public:
    bool initialize(D3D11Context &context, QString *error = nullptr);
    bool convert(ID3D11Texture2D *source, int width, int height,
                 ID3D11RenderTargetView *target, float rotationRadians,
                 QString *error = nullptr);

private:
    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_params;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/RgbaConverter.cpp`：

```cpp
#include "RgbaConverter.h"

#ifdef _WIN32

#include <cstring>
#include <d3dcompiler.h>

#include "D3D11Context.h"
#include "Shaders.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

Microsoft::WRL::ComPtr<ID3DBlob> compileShader(const char *source,
                                               const char *entry,
                                               const char *target,
                                               QString *error)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                                  entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                  &blob, &diagnostics);
    if (FAILED(hr)) {
        const QString detail = diagnostics
            ? QString::fromUtf8(static_cast<const char *>(diagnostics->GetBufferPointer()),
                                int(diagnostics->GetBufferSize()))
            : QStringLiteral("unknown shader error");
        setError(error, QStringLiteral("着色器编译失败：%1").arg(detail));
    }
    return blob;
}

} // namespace

bool RgbaConverter::initialize(D3D11Context &context, QString *error)
{
    m_device = context.device();
    m_context = context.context();

    auto vsBlob = compileShader(shaders::kFullscreenVs, "main", "vs_5_0", error);
    auto psBlob = compileShader(shaders::kRgbaPs, "main", "ps_5_0", error);
    if (!vsBlob || !psBlob)
        return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(), nullptr, &m_vs))
        || FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                              psBlob->GetBufferSize(), nullptr, &m_ps))) {
        setError(error, QStringLiteral("无法创建着色器对象。"));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&samplerDesc, &m_sampler))) {
        setError(error, QStringLiteral("无法创建采样器。"));
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 16;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_params))) {
        setError(error, QStringLiteral("无法创建参数缓冲区。"));
        return false;
    }

    return true;
}

bool RgbaConverter::convert(ID3D11Texture2D *source, int width, int height,
                            ID3D11RenderTargetView *target, float rotationRadians,
                            QString *error)
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(m_device->CreateShaderResourceView(source, nullptr, &srv))) {
        setError(error, QStringLiteral("无法创建 RGBA 纹理视图。"));
        return false;
    }

    const float params[4] = {rotationRadians, 0.0f, 0.0f, 0.0f};
    m_context->UpdateSubresource(m_params.Get(), 0, nullptr, params, 0, 0);

    ID3D11RenderTargetView *targets[] = {target};
    m_context->OMSetRenderTargets(1, targets, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    m_context->PSSetShaderResources(0, 1, srv.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_params.GetAddressOf());
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);
    return true;
}

} // namespace VideoEditor

#endif
```

**全屏三角形状态（必做）：** 两个转换器都必须创建并绑定 `CullMode = D3D11_CULL_NONE` 的光栅化状态，否则全屏三角形可能被背面剔除，表现为黑屏。在 `Nv12Converter` 与 `RgbaConverter` 中各增加成员 `Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;`，在 `initialize()` 中创建：

```cpp
D3D11_RASTERIZER_DESC rasterizer{};
rasterizer.FillMode = D3D11_FILL_SOLID;
rasterizer.CullMode = D3D11_CULL_NONE;
rasterizer.DepthClipEnable = TRUE;
if (FAILED(m_device->CreateRasterizerState(&rasterizer, &m_rasterizer))) {
    setError(error, QStringLiteral("无法创建光栅化状态。"));
    return false;
}
```

并在 `draw()` / `convert()` 的绘制前调用 `m_context->RSSetState(m_rasterizer.Get());`。

`src/media/decode/FrameReadback.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <QString>

namespace VideoEditor {

class D3D11Context;

std::vector<uint8_t> readbackSharedTexture(D3D11Context &context,
                                           ID3D11Texture2D *texture,
                                           QString *error = nullptr);

} // namespace VideoEditor

#endif
```

`src/media/decode/FrameReadback.cpp`：

```cpp
#include "FrameReadback.h"

#ifdef _WIN32

#include <cstring>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "D3D11Context.h"

namespace VideoEditor {

std::vector<uint8_t> readbackSharedTexture(D3D11Context &context,
                                           ID3D11Texture2D *texture,
                                           QString *error)
{
    if (!texture) {
        if (error)
            *error = QStringLiteral("纹理为空。");
        return {};
    }

    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&mutex)))
        || !mutex || mutex->AcquireSync(1, 5000) != S_OK) {
        if (error)
            *error = QStringLiteral("无法获取共享纹理互斥量。");
        return {};
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = context.device()->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) {
        mutex->ReleaseSync(0);
        if (error)
            *error = QStringLiteral("无法创建回读纹理。");
        return {};
    }

    context.context()->CopyResource(staging.Get(), texture);
    mutex->ReleaseSync(0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        if (error)
            *error = QStringLiteral("无法映射回读纹理。");
        return {};
    }

    std::vector<uint8_t> result(size_t(desc.Width) * desc.Height * 4);
    for (UINT row = 0; row < desc.Height; ++row) {
        std::memcpy(result.data() + size_t(row) * desc.Width * 4,
                    static_cast<const uint8_t *>(mapped.pData)
                        + size_t(row) * mapped.RowPitch,
                    size_t(desc.Width) * 4);
    }
    context.context()->Unmap(staging.Get(), 0);
    return result;
}

} // namespace VideoEditor

#endif
```

修改 `src/media/CMakeLists.txt`，加入解码源文件：

```cmake
find_package(FFmpeg REQUIRED)

add_library(videoeditor_media STATIC
  probe/MediaProbe.h
  probe/MediaProbe.cpp
  probe/FfmpegError.h
  probe/FfmpegError.cpp
  decode/D3D11Context.h
  decode/D3D11Context.cpp
  decode/SharedTextureRing.h
  decode/SharedTextureRing.cpp
  decode/Shaders.h
  decode/Nv12Converter.h
  decode/Nv12Converter.cpp
  decode/RgbaConverter.h
  decode/RgbaConverter.cpp
  decode/FrameReadback.h
  decode/FrameReadback.cpp
)
target_include_directories(videoeditor_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_media
  PUBLIC videoeditor_core Qt6::Core Qt6::Gui
  PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
          d3d11 dxgi d3dcompiler
)
target_compile_definitions(videoeditor_media PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
```

- [ ] **Step 5: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`D3D11Pipeline` 测试通过；如果机器没有 D3D11（极少见），测试显示 skipped 并给出原因。

- [ ] **Step 6: 提交**

```bash
git add src/media tests
git commit -m "feat(media): add D3D11 context, shared textures, NV12 and RGBA converters"
```

---

### Task 7: FFmpeg 硬件/软件解码器

**Files:**
- Create: `src/media/decode/IVideoDecoder.h`
- Create: `src/media/decode/FfmpegD3D11Decoder.h`
- Create: `src/media/decode/FfmpegD3D11Decoder.cpp`
- Create: `src/media/decode/FfmpegSoftwareDecoder.h`
- Create: `src/media/decode/FfmpegSoftwareDecoder.cpp`
- Create: `src/media/decode/VideoDecoderFactory.h`
- Create: `src/media/decode/VideoDecoderFactory.cpp`
- Create: `src/media/decode/FramePublisher.h`
- Create: `src/media/decode/FramePublisher.cpp`
- Modify: `src/media/CMakeLists.txt`
- Create: `tests/media/DecoderTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MediaProbe`、`D3D11Context`、`SharedTextureRing`、`Nv12Converter`、`RgbaConverter`。
- Produces:
  - `DecodedFrame`（硬件纹理或 CPU RGBA）
  - `createVideoDecoder(path, D3D11Context&, info) -> DecoderCreationResult`
  - `FramePublisher::publish(DecodedFrame&, int rotationDegrees, QString*) -> bool`
  - `FramePublisher::lastWrittenSlot() -> int`

- [ ] **Step 1: 写失败测试**

`tests/media/DecoderTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "D3D11Context.h"
#include "FramePublisher.h"
#include "FrameReadback.h"
#include "MediaProbe.h"
#include "SharedTextureRing.h"
#include "VideoDecoderFactory.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

std::unique_ptr<D3D11Context> requireContext(bool video)
{
    QString error;
    auto context = D3D11Context::create(video, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();
    return context;
}

} // namespace

TEST(Decoder, DecodesH264FirstFrame)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(true);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());

    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();

    DecodedFrame frame;
    QString error;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    EXPECT_EQ(frame.width, 640);
    EXPECT_EQ(frame.height, 360);
    EXPECT_GE(frame.ptsUs, 0);
    EXPECT_TRUE(frame.kind == DecodedFrame::Kind::HardwareTexture
                || frame.kind == DecodedFrame::Kind::CpuRgba);
}

TEST(Decoder, Decodes10BitH265WithFallback)
{
    const QString path = fixture(QStringLiteral("h265_10bit.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(true);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();

    DecodedFrame frame;
    QString error;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    EXPECT_EQ(frame.width, 640);
    EXPECT_EQ(frame.height, 360);
}

TEST(Decoder, SeeksToHalfSecond)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(true);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();

    QString error;
    ASSERT_TRUE(result.decoder->seek(1'000'000, &error)) << error.toStdString();
    DecodedFrame frame;
    ASSERT_TRUE(result.decoder->decodeNext(frame, &error)) << error.toStdString();
    EXPECT_GE(frame.ptsUs, 900'000);
    EXPECT_LT(frame.ptsUs, 1'300'000);
}

TEST(Decoder, FallsBackToSoftwareWhenVideoContextDisabled)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(false);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    EXPECT_FALSE(result.decoder->isHardware());
}

TEST(Decoder, HandlesUnicodePath)
{
    const QString path = fixture(QStringLiteral("中文 文件名.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(false);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();
    DecodedFrame frame;
    ASSERT_TRUE(result.decoder->decodeNext(frame, nullptr));
}

TEST(Decoder, VfrTimestampsAreMonotonic)
{
    const QString path = fixture(QStringLiteral("vfr.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(false);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();

    int64_t previous = -1;
    for (int i = 0; i < 10; ++i) {
        DecodedFrame frame;
        if (!result.decoder->decodeNext(frame, nullptr))
            break;
        EXPECT_GT(frame.ptsUs, previous);
        previous = frame.ptsUs;
    }
}

TEST(Decoder, PublishesRedFrameToSharedTexture)
{
    const QString path = fixture(QStringLiteral("red.mp4"));
    if (!QFile::exists(path)) GTEST_SKIP() << "missing fixture";

    auto context = requireContext(false);
    const auto info = MediaProbe::probe(path, nullptr);
    ASSERT_TRUE(info.has_value());
    auto result = createVideoDecoder(path, *context, *info);
    ASSERT_NE(result.decoder, nullptr) << result.error.toStdString();

    DecodedFrame frame;
    ASSERT_TRUE(result.decoder->decodeNext(frame, nullptr));

    SharedTextureRing ring(*context);
    QString error;
    ASSERT_TRUE(ring.create(frame.width, frame.height, 1, &error)) << error.toStdString();

    FramePublisher publisher(*context, ring);
    ASSERT_TRUE(publisher.publish(frame, 0, &error)) << error.toStdString();

    const auto rgba = readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(rgba.size(), size_t(frame.width) * frame.height * 4)
        << error.toStdString();
    EXPECT_GT(rgba[0], 200);
    EXPECT_LT(rgba[1], 60);
    EXPECT_LT(rgba[2], 60);
}
```

修改 `tests/CMakeLists.txt`：

```cmake
add_executable(media_tests
  media/MediaProbeTest.cpp
  media/D3D11PipelineTest.cpp
  media/DecoderTest.cpp
  TestMain.cpp
)
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-debug
```

预期：编译失败，缺少 `VideoDecoderFactory.h`。

- [ ] **Step 3: 实现解码器接口与工厂**

`src/media/decode/IVideoDecoder.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>

#include <d3d11.h>

#include <QString>

#include "MediaInfo.h"

struct AVFrame;

namespace VideoEditor {

struct DecodedFrame {
    enum class Kind { HardwareTexture, CpuRgba };

    Kind kind = Kind::CpuRgba;
    ID3D11Texture2D *texture = nullptr;
    UINT subresource = 0;
    int width = 0;
    int height = 0;
    int64_t ptsUs = 0;
    std::vector<uint8_t> rgba;
    AVFrame *avFrame = nullptr;

    DecodedFrame() = default;
    DecodedFrame(const DecodedFrame &) = delete;
    DecodedFrame &operator=(const DecodedFrame &) = delete;
    DecodedFrame(DecodedFrame &&other) noexcept;
    DecodedFrame &operator=(DecodedFrame &&other) noexcept;
    ~DecodedFrame();
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool seek(int64_t ptsUs, QString *error = nullptr) = 0;
    virtual bool decodeNext(DecodedFrame &frame, QString *error = nullptr) = 0;
    virtual const MediaInfo &info() const = 0;
    virtual bool isHardware() const = 0;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/IVideoDecoder.cpp`（新增；不要忘记加入 CMake）：

```cpp
#include "IVideoDecoder.h"

#ifdef _WIN32

extern "C" {
#include <libavutil/frame.h>
}

namespace VideoEditor {

DecodedFrame::DecodedFrame(DecodedFrame &&other) noexcept
    : kind(other.kind)
    , texture(other.texture)
    , subresource(other.subresource)
    , width(other.width)
    , height(other.height)
    , ptsUs(other.ptsUs)
    , rgba(std::move(other.rgba))
    , avFrame(other.avFrame)
{
    other.texture = nullptr;
    other.avFrame = nullptr;
}

DecodedFrame &DecodedFrame::operator=(DecodedFrame &&other) noexcept
{
    if (this == &other)
        return *this;
    if (avFrame)
        av_frame_free(&avFrame);
    kind = other.kind;
    texture = other.texture;
    subresource = other.subresource;
    width = other.width;
    height = other.height;
    ptsUs = other.ptsUs;
    rgba = std::move(other.rgba);
    avFrame = other.avFrame;
    other.texture = nullptr;
    other.avFrame = nullptr;
    return *this;
}

DecodedFrame::~DecodedFrame()
{
    if (avFrame)
        av_frame_free(&avFrame);
}

} // namespace VideoEditor

#endif
```

`src/media/decode/VideoDecoderFactory.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;

struct DecoderCreationResult {
    std::unique_ptr<IVideoDecoder> decoder;
    QString error;
    QString hardwareFallbackReason;
};

DecoderCreationResult createVideoDecoder(const QString &path,
                                         D3D11Context &context,
                                         const MediaInfo &info);

} // namespace VideoEditor

#endif
```

`src/media/decode/VideoDecoderFactory.cpp`：

```cpp
#include "VideoDecoderFactory.h"

#ifdef _WIN32

#include "D3D11Context.h"
#include "FfmpegD3D11Decoder.h"
#include "FfmpegSoftwareDecoder.h"

namespace VideoEditor {

DecoderCreationResult createVideoDecoder(const QString &path,
                                         D3D11Context &context,
                                         const MediaInfo &info)
{
    DecoderCreationResult result;

    if (context.supportsVideoDecoding()) {
        QString hardwareError;
        if (auto decoder = FfmpegD3D11Decoder::open(path, context, info,
                                                    &hardwareError)) {
            result.decoder = std::move(decoder);
            return result;
        }
        result.hardwareFallbackReason = hardwareError;
    } else {
        result.hardwareFallbackReason = QStringLiteral("D3D11 视频设备不可用。");
    }

    QString softwareError;
    if (auto decoder = FfmpegSoftwareDecoder::open(path, context, info,
                                                   &softwareError)) {
        result.decoder = std::move(decoder);
        return result;
    }

    result.error = QStringLiteral("无法解码视频。\n硬件解码：%1\n软件解码：%2")
                       .arg(result.hardwareFallbackReason, softwareError);
    return result;
}

} // namespace VideoEditor

#endif
```

- [ ] **Step 4: 实现硬件解码器**

`src/media/decode/FfmpegD3D11Decoder.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;

class FfmpegD3D11Decoder final : public IVideoDecoder {
public:
    static std::unique_ptr<FfmpegD3D11Decoder> open(const QString &path,
                                                    D3D11Context &context,
                                                    const MediaInfo &info,
                                                    QString *error = nullptr);
    ~FfmpegD3D11Decoder() override;

    bool seek(int64_t ptsUs, QString *error = nullptr) override;
    bool decodeNext(DecodedFrame &frame, QString *error = nullptr) override;
    const MediaInfo &info() const override;
    bool isHardware() const override { return true; }

private:
    FfmpegD3D11Decoder();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/FfmpegD3D11Decoder.cpp`：

```cpp
#include "FfmpegD3D11Decoder.h"

#ifdef _WIN32

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
}

#include "D3D11Context.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString ffmpegError(int value)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

AVPixelFormat selectD3D11Format(AVCodecContext *, const AVPixelFormat *formats)
{
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_D3D11)
            return *format;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

struct FfmpegD3D11Decoder::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVBufferRef *hardwareDevice = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int videoStream = -1;
    MediaInfo info;

    ~Impl()
    {
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (codec)
            avcodec_free_context(&codec);
        if (hardwareDevice)
            av_buffer_unref(&hardwareDevice);
        if (format)
            avformat_close_input(&format);
    }
};

FfmpegD3D11Decoder::FfmpegD3D11Decoder()
    : m_impl(std::make_unique<Impl>())
{
}

FfmpegD3D11Decoder::~FfmpegD3D11Decoder() = default;

std::unique_ptr<FfmpegD3D11Decoder> FfmpegD3D11Decoder::open(
    const QString &path, D3D11Context &context, const MediaInfo &info,
    QString *error)
{
    auto decoder = std::unique_ptr<FfmpegD3D11Decoder>(new FfmpegD3D11Decoder);
    Impl &impl = *decoder->m_impl;
    impl.info = info;

    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&impl.format, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("硬件解码打开文件失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }
    ret = avformat_find_stream_info(impl.format, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("硬件解码读取流信息失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.videoStream = av_find_best_stream(impl.format, AVMEDIA_TYPE_VIDEO,
                                           -1, -1, nullptr, 0);
    if (impl.videoStream < 0) {
        setError(error, QStringLiteral("文件不包含视频流。"));
        return nullptr;
    }

    const AVCodec *codec = avcodec_find_decoder(
        impl.format->streams[impl.videoStream]->codecpar->codec_id);
    if (!codec) {
        setError(error, QStringLiteral("FFmpeg 不支持该视频编码。"));
        return nullptr;
    }

    impl.hardwareDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!impl.hardwareDevice) {
        setError(error, QStringLiteral("无法分配 D3D11VA 设备。"));
        return nullptr;
    }

    auto *deviceContext = reinterpret_cast<AVD3D11VADeviceContext *>(
        impl.hardwareDevice->data);
    deviceContext->device = context.device();
    deviceContext->device->AddRef();
    deviceContext->device_context = context.context();
    deviceContext->device_context->AddRef();

    ret = av_hwdevice_ctx_init(impl.hardwareDevice);
    if (ret < 0) {
        setError(error, QStringLiteral("初始化 D3D11VA 设备失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.codec = avcodec_alloc_context3(codec);
    if (!impl.codec) {
        setError(error, QStringLiteral("无法分配解码上下文。"));
        return nullptr;
    }

    ret = avcodec_parameters_to_context(
        impl.codec, impl.format->streams[impl.videoStream]->codecpar);
    if (ret < 0) {
        setError(error, QStringLiteral("复制解码参数失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.codec->pkt_timebase = impl.format->streams[impl.videoStream]->time_base;
    impl.codec->hw_device_ctx = av_buffer_ref(impl.hardwareDevice);
    impl.codec->get_format = selectD3D11Format;
    impl.codec->thread_count = 1;

    ret = avcodec_open2(impl.codec, codec, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("打开硬件解码器失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    if (impl.codec->hw_frames_ctx) {
        const auto *frames = reinterpret_cast<AVHWFramesContext *>(
            impl.codec->hw_frames_ctx->data);
        if (frames->sw_format != AV_PIX_FMT_NV12) {
            setError(error, QStringLiteral("硬件路径暂只支持 NV12 8-bit，已回退软件解码。"));
            return nullptr;
        }
    }

    impl.packet = av_packet_alloc();
    impl.frame = av_frame_alloc();
    if (!impl.packet || !impl.frame) {
        setError(error, QStringLiteral("无法分配解码帧。"));
        return nullptr;
    }

    return decoder;
}

bool FfmpegD3D11Decoder::seek(int64_t ptsUs, QString *error)
{
    Impl &impl = *m_impl;
    const int64_t timestamp = av_rescale_q(ptsUs, AV_TIME_BASE_Q,
                                           impl.format->streams[impl.videoStream]->time_base);
    const int ret = av_seek_frame(impl.format, impl.videoStream, timestamp,
                                  AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        setError(error, QStringLiteral("定位失败：%1").arg(ffmpegError(ret)));
        return false;
    }
    avcodec_flush_buffers(impl.codec);
    return true;
}

bool FfmpegD3D11Decoder::decodeNext(DecodedFrame &out, QString *error)
{
    Impl &impl = *m_impl;

    while (true) {
        int ret = avcodec_receive_frame(impl.codec, impl.frame);
        if (ret == 0) {
            if (impl.frame->format != AV_PIX_FMT_D3D11) {
                setError(error, QStringLiteral("硬件解码器未返回 D3D11 帧。"));
                return false;
            }
            if (impl.frame->hw_frames_ctx) {
                const auto *frames = reinterpret_cast<AVHWFramesContext *>(
                    impl.frame->hw_frames_ctx->data);
                if (frames->sw_format != AV_PIX_FMT_NV12) {
                    setError(error, QStringLiteral("硬件路径暂只支持 NV12 8-bit。"));
                    return false;
                }
            }

            out.kind = DecodedFrame::Kind::HardwareTexture;
            out.texture = reinterpret_cast<ID3D11Texture2D *>(impl.frame->data[0]);
            out.subresource = static_cast<UINT>(
                reinterpret_cast<uintptr_t>(impl.frame->data[1]));
            out.width = impl.frame->width;
            out.height = impl.frame->height;
            out.rgba.clear();

            int64_t timestamp = impl.frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE)
                timestamp = impl.frame->pts;
            out.ptsUs = timestamp == AV_NOPTS_VALUE
                ? 0
                : av_rescale_q(timestamp, impl.format->streams[impl.videoStream]->time_base,
                               AV_TIME_BASE_Q);

            if (out.avFrame)
                av_frame_free(&out.avFrame);
            out.avFrame = impl.frame;
            impl.frame = av_frame_alloc();
            if (!impl.frame) {
                setError(error, QStringLiteral("无法分配下一帧。"));
                return false;
            }
            return true;
        }

        if (ret == AVERROR_EOF)
            return false;

        if (ret != AVERROR(EAGAIN)) {
            setError(error, QStringLiteral("硬件解码失败：%1").arg(ffmpegError(ret)));
            return false;
        }

        int readRet = av_read_frame(impl.format, impl.packet);
        if (readRet == AVERROR_EOF) {
            avcodec_send_packet(impl.codec, nullptr);
            continue;
        }
        if (readRet < 0) {
            setError(error, QStringLiteral("读取视频包失败：%1").arg(ffmpegError(readRet)));
            return false;
        }

        if (impl.packet->stream_index == impl.videoStream) {
            const int sendRet = avcodec_send_packet(impl.codec, impl.packet);
            av_packet_unref(impl.packet);
            if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                setError(error, QStringLiteral("发送视频包失败：%1").arg(ffmpegError(sendRet)));
                return false;
            }
        } else {
            av_packet_unref(impl.packet);
        }
    }
}

const MediaInfo &FfmpegD3D11Decoder::info() const
{
    return m_impl->info;
}

} // namespace VideoEditor

#endif
```

- [ ] **Step 5: 实现软件解码器**

`src/media/decode/FfmpegSoftwareDecoder.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;

class FfmpegSoftwareDecoder final : public IVideoDecoder {
public:
    static std::unique_ptr<FfmpegSoftwareDecoder> open(const QString &path,
                                                       D3D11Context &context,
                                                       const MediaInfo &info,
                                                       QString *error = nullptr);
    ~FfmpegSoftwareDecoder() override;

    bool seek(int64_t ptsUs, QString *error = nullptr) override;
    bool decodeNext(DecodedFrame &frame, QString *error = nullptr) override;
    const MediaInfo &info() const override;
    bool isHardware() const override { return false; }

private:
    FfmpegSoftwareDecoder();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/FfmpegSoftwareDecoder.cpp`：

```cpp
#include "FfmpegSoftwareDecoder.h"

#ifdef _WIN32

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "D3D11Context.h"

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString ffmpegError(int value)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

} // namespace

struct FfmpegSoftwareDecoder::Impl {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *scaler = nullptr;
    int scalerWidth = 0;
    int scalerHeight = 0;
    int scalerFormat = AV_PIX_FMT_NONE;
    int videoStream = -1;
    MediaInfo info;

    ~Impl()
    {
        if (scaler)
            sws_freeContext(scaler);
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (codec)
            avcodec_free_context(&codec);
        if (format)
            avformat_close_input(&format);
    }
};

FfmpegSoftwareDecoder::FfmpegSoftwareDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

FfmpegSoftwareDecoder::~FfmpegSoftwareDecoder() = default;

std::unique_ptr<FfmpegSoftwareDecoder> FfmpegSoftwareDecoder::open(
    const QString &path, D3D11Context &, const MediaInfo &info, QString *error)
{
    auto decoder = std::unique_ptr<FfmpegSoftwareDecoder>(new FfmpegSoftwareDecoder);
    Impl &impl = *decoder->m_impl;
    impl.info = info;

    const QByteArray utf8Path = path.toUtf8();
    int ret = avformat_open_input(&impl.format, utf8Path.constData(), nullptr, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("软件解码打开文件失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }
    ret = avformat_find_stream_info(impl.format, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("软件解码读取流信息失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.videoStream = av_find_best_stream(impl.format, AVMEDIA_TYPE_VIDEO,
                                           -1, -1, nullptr, 0);
    if (impl.videoStream < 0) {
        setError(error, QStringLiteral("文件不包含视频流。"));
        return nullptr;
    }

    const AVCodec *codec = avcodec_find_decoder(
        impl.format->streams[impl.videoStream]->codecpar->codec_id);
    if (!codec) {
        setError(error, QStringLiteral("FFmpeg 不支持该视频编码。"));
        return nullptr;
    }

    impl.codec = avcodec_alloc_context3(codec);
    if (!impl.codec) {
        setError(error, QStringLiteral("无法分配解码上下文。"));
        return nullptr;
    }

    ret = avcodec_parameters_to_context(
        impl.codec, impl.format->streams[impl.videoStream]->codecpar);
    if (ret < 0) {
        setError(error, QStringLiteral("复制解码参数失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }
    impl.codec->pkt_timebase = impl.format->streams[impl.videoStream]->time_base;
    impl.codec->thread_count = 0;

    ret = avcodec_open2(impl.codec, codec, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("打开软件解码器失败：%1").arg(ffmpegError(ret)));
        return nullptr;
    }

    impl.packet = av_packet_alloc();
    impl.frame = av_frame_alloc();
    if (!impl.packet || !impl.frame) {
        setError(error, QStringLiteral("无法分配解码帧。"));
        return nullptr;
    }

    return decoder;
}

bool FfmpegSoftwareDecoder::seek(int64_t ptsUs, QString *error)
{
    Impl &impl = *m_impl;
    const int64_t timestamp = av_rescale_q(ptsUs, AV_TIME_BASE_Q,
                                           impl.format->streams[impl.videoStream]->time_base);
    const int ret = av_seek_frame(impl.format, impl.videoStream, timestamp,
                                  AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        setError(error, QStringLiteral("定位失败：%1").arg(ffmpegError(ret)));
        return false;
    }
    avcodec_flush_buffers(impl.codec);
    return true;
}

bool FfmpegSoftwareDecoder::decodeNext(DecodedFrame &out, QString *error)
{
    Impl &impl = *m_impl;

    while (true) {
        int ret = avcodec_receive_frame(impl.codec, impl.frame);
        if (ret == 0) {
            if (!impl.scaler || impl.scalerWidth != impl.frame->width
                || impl.scalerHeight != impl.frame->height
                || impl.scalerFormat != impl.frame->format) {
                if (impl.scaler)
                    sws_freeContext(impl.scaler);
                impl.scaler = sws_getContext(
                    impl.frame->width, impl.frame->height,
                    static_cast<AVPixelFormat>(impl.frame->format),
                    impl.frame->width, impl.frame->height, AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                impl.scalerWidth = impl.frame->width;
                impl.scalerHeight = impl.frame->height;
                impl.scalerFormat = impl.frame->format;
            }
            if (!impl.scaler) {
                setError(error, QStringLiteral("无法创建像素转换器。"));
                return false;
            }

            out.kind = DecodedFrame::Kind::CpuRgba;
            out.texture = nullptr;
            out.width = impl.frame->width;
            out.height = impl.frame->height;
            out.rgba.resize(size_t(out.width) * out.height * 4);

            uint8_t *destination[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
            int destinationStride[4] = {out.width * 4, 0, 0, 0};
            sws_scale(impl.scaler, impl.frame->data, impl.frame->linesize, 0,
                      impl.frame->height, destination, destinationStride);

            int64_t timestamp = impl.frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE)
                timestamp = impl.frame->pts;
            out.ptsUs = timestamp == AV_NOPTS_VALUE
                ? 0
                : av_rescale_q(timestamp, impl.format->streams[impl.videoStream]->time_base,
                               AV_TIME_BASE_Q);
            out.avFrame = nullptr;
            return true;
        }

        if (ret == AVERROR_EOF)
            return false;

        if (ret != AVERROR(EAGAIN)) {
            setError(error, QStringLiteral("软件解码失败：%1").arg(ffmpegError(ret)));
            return false;
        }

        int readRet = av_read_frame(impl.format, impl.packet);
        if (readRet == AVERROR_EOF) {
            avcodec_send_packet(impl.codec, nullptr);
            continue;
        }
        if (readRet < 0) {
            setError(error, QStringLiteral("读取视频包失败：%1").arg(ffmpegError(readRet)));
            return false;
        }

        if (impl.packet->stream_index == impl.videoStream) {
            const int sendRet = avcodec_send_packet(impl.codec, impl.packet);
            av_packet_unref(impl.packet);
            if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                setError(error, QStringLiteral("发送视频包失败：%1").arg(ffmpegError(sendRet)));
                return false;
            }
        } else {
            av_packet_unref(impl.packet);
        }
    }
}

const MediaInfo &FfmpegSoftwareDecoder::info() const
{
    return m_impl->info;
}

} // namespace VideoEditor

#endif
```

- [ ] **Step 6: 实现帧发布器**

`src/media/decode/FramePublisher.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <memory>
#include <QString>

#include "IVideoDecoder.h"

namespace VideoEditor {

class D3D11Context;
class SharedTextureRing;
class Nv12Converter;
class RgbaConverter;

class FramePublisher {
public:
    FramePublisher(D3D11Context &context, SharedTextureRing &ring);
    ~FramePublisher();

    bool publish(DecodedFrame &frame, int rotationDegrees,
                 QString *error = nullptr);
    int lastWrittenSlot() const { return m_lastSlot; }

private:
    D3D11Context &m_context;
    SharedTextureRing &m_ring;
    std::unique_ptr<Nv12Converter> m_nv12;
    std::unique_ptr<RgbaConverter> m_rgba;
    int m_lastSlot = -1;
};

} // namespace VideoEditor

#endif
```

`src/media/decode/FramePublisher.cpp`：

```cpp
#include "FramePublisher.h"

#ifdef _WIN32

#include <cmath>
#include <memory>

#include "D3D11Context.h"
#include "Nv12Converter.h"
#include "RgbaConverter.h"
#include "SharedTextureRing.h"

namespace VideoEditor {

FramePublisher::FramePublisher(D3D11Context &context, SharedTextureRing &ring)
    : m_context(context)
    , m_ring(ring)
{
}

FramePublisher::~FramePublisher() = default;

bool FramePublisher::publish(DecodedFrame &frame, int rotationDegrees,
                             QString *error)
{
    if (m_ring.count() <= 0) {
        if (error)
            *error = QStringLiteral("共享纹理环尚未创建。");
        return false;
    }

    if (!m_nv12) {
        m_nv12 = std::make_unique<Nv12Converter>();
        if (!m_nv12->initialize(m_context, error))
            return false;
    }
    if (!m_rgba) {
        m_rgba = std::make_unique<RgbaConverter>();
        if (!m_rgba->initialize(m_context, error))
            return false;
    }

    const int slot = m_ring.nextWriteIndex();
    IDXGIKeyedMutex *mutex = m_ring.mutex(slot);
    if (!mutex || mutex->AcquireSync(0, 5000) != S_OK) {
        if (error)
            *error = QStringLiteral("等待共享纹理写入超时。");
        return false;
    }

    const float radians = float(rotationDegrees) * 3.14159265358979323846f / 180.0f;
    bool ok = false;
    if (frame.kind == DecodedFrame::Kind::HardwareTexture) {
        ok = m_nv12->convert(frame.texture, frame.subresource, frame.width,
                             frame.height, m_ring.rtv(slot), radians, error);
    } else {
        ok = m_rgba->convertRgba(frame.rgba, frame.width, frame.height,
                                 m_ring.rtv(slot), radians, error);
    }

    mutex->ReleaseSync(ok ? 1 : 0);
    if (!ok)
        return false;

    m_lastSlot = slot;
    m_ring.advanceWriteIndex();
    return true;
}

} // namespace VideoEditor

#endif
```

并且 `RgbaConverter` 需要提供从 CPU RGBA 上传的方法。在 `RgbaConverter.h` 增加：

```cpp
bool convertRgba(const std::vector<uint8_t> &rgba, int width, int height,
                 ID3D11RenderTargetView *target, float rotationRadians,
                 QString *error = nullptr);
```

实现：

```cpp
bool RgbaConverter::convertRgba(const std::vector<uint8_t> &rgba, int width,
                                int height, ID3D11RenderTargetView *target,
                                float rotationRadians, QString *error)
{
    if (rgba.size() < size_t(width) * height * 4) {
        setError(error, QStringLiteral("RGBA 缓冲区尺寸不正确。"));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!m_uploadTexture || m_uploadWidth != width || m_uploadHeight != height) {
        m_uploadTexture.Reset();
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_uploadTexture))) {
            setError(error, QStringLiteral("无法创建 CPU 上传纹理。"));
            return false;
        }
        m_uploadWidth = width;
        m_uploadHeight = height;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_uploadTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                              0, &mapped))) {
        setError(error, QStringLiteral("无法映射 CPU 上传纹理。"));
        return false;
    }
    for (int row = 0; row < height; ++row) {
        std::memcpy(static_cast<uint8_t *>(mapped.pData) + size_t(row) * mapped.RowPitch,
                    rgba.data() + size_t(row) * width * 4,
                    size_t(width) * 4);
    }
    m_context->Unmap(m_uploadTexture.Get(), 0);

    return convert(m_uploadTexture.Get(), width, height, target, rotationRadians,
                   error);
}
```

在 `RgbaConverter.h` 私有成员增加：
```cpp
Microsoft::WRL::ComPtr<ID3D11Texture2D> m_uploadTexture;
int m_uploadWidth = 0;
int m_uploadHeight = 0;
```

修改 `src/media/CMakeLists.txt`，加入：

```cmake
decode/IVideoDecoder.h
decode/IVideoDecoder.cpp
decode/FfmpegD3D11Decoder.h
decode/FfmpegD3D11Decoder.cpp
decode/FfmpegSoftwareDecoder.h
decode/FfmpegSoftwareDecoder.cpp
decode/VideoDecoderFactory.h
decode/VideoDecoderFactory.cpp
decode/FramePublisher.h
decode/FramePublisher.cpp
```

- [ ] **Step 7: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`Decoder` 测试通过；硬件解码不可用时，工厂自动使用软件解码，测试仍通过。软件解码测试中的红帧回读应得到 `R>200, G<60, B<60`。

- [ ] **Step 8: 提交**

```bash
git add src/media tests
git commit -m "feat(media): decode H.264/H.265/MPEG-4/WMV with hardware and software paths"
```

---

### Task 8: 预览桥接、播放控制与 QML 预览

**Files:**
- Create: `src/ui/FrameBridge.h`
- Create: `src/ui/FrameBridge.cpp`
- Create: `src/ui/VideoPreviewItem.h`
- Create: `src/ui/VideoPreviewItem.cpp`
- Create: `src/ui/DecoderWorker.h`
- Create: `src/ui/DecoderWorker.cpp`
- Create: `src/ui/PreviewController.h`
- Create: `src/ui/PreviewController.cpp`
- Modify: `src/ui/CMakeLists.txt`
- Modify: `src/app/AppController.h`
- Modify: `src/app/AppController.cpp`
- Modify: `src/app/main.cpp`
- Modify: `src/app/Main.qml`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `FramePublisher`、`SharedTextureRing`、`IVideoDecoder`、`MediaProbe`。
- Produces:
  - `VideoPreviewItem::submitFrame(int slot, QSize size, int rotation, qint64 ptsUs)`
  - `PreviewController::openMedia(QString path, QString proxyPath)`、`play()`、`pause()`、`seekMs(qint64)`、`stepFrames(int)`
  - QML 类型 `VideoPreview`；上下文属性 `Preview`。

- [ ] **Step 1: 实现 FrameBridge**

`src/ui/FrameBridge.h`：

```cpp
#pragma once

#ifdef _WIN32

#include <array>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <QQuickWindow>
#include <QSGTexture>
#include <QString>

namespace VideoEditor {

class FrameBridge {
public:
    bool initialize(QQuickWindow *window,
                    const std::array<quintptr, 3> &handles,
                    int width, int height, QString *error = nullptr);
    QSGTexture *present(int slot, QQuickWindow *window);
    bool isValid() const { return m_valid; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> shared;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> owned;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    };

    std::array<Slot, 3> m_slots;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    ID3D11DeviceContext *m_context = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_valid = false;
};

} // namespace VideoEditor

#endif
```

`src/ui/FrameBridge.cpp`：

```cpp
#include "FrameBridge.h"

#ifdef _WIN32

#include <QNativeInterface>
#include <QSGRendererInterface>

namespace VideoEditor {

bool FrameBridge::initialize(QQuickWindow *window,
                             const std::array<quintptr, 3> &handles,
                             int width, int height, QString *error)
{
    if (!window) {
        if (error)
            *error = QStringLiteral("预览窗口为空。");
        return false;
    }

    auto *device = static_cast<ID3D11Device *>(
        QSGRendererInterface::getResource(window,
                                          QSGRendererInterface::DeviceResource));
    auto *context = static_cast<ID3D11DeviceContext *>(
        QSGRendererInterface::getResource(window,
                                          QSGRendererInterface::DeviceContextResource));
    if (!device || !context) {
        if (error)
            *error = QStringLiteral("Qt 场景图尚未使用 D3D11 后端。");
        return false;
    }

    m_device = device;
    m_context = context;
    m_width = width;
    m_height = height;

    for (int i = 0; i < 3; ++i) {
        if (!handles[i]) {
            if (error)
                *error = QStringLiteral("共享纹理句柄为空。");
            return false;
        }

        if (FAILED(m_device->OpenSharedResource(
                reinterpret_cast<HANDLE>(handles[i]),
                IID_PPV_ARGS(&m_slots[i].shared)))) {
            if (error)
                *error = QStringLiteral("无法打开共享纹理。");
            return false;
        }

        if (FAILED(m_slots[i].shared.As(&m_slots[i].mutex))) {
            if (error)
                *error = QStringLiteral("无法获取共享纹理互斥量。");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        m_slots[i].shared->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_slots[i].owned))) {
            if (error)
                *error = QStringLiteral("无法创建 Qt 侧纹理。");
            return false;
        }
    }

    m_valid = true;
    return true;
}

QSGTexture *FrameBridge::present(int slot, QQuickWindow *window)
{
    if (!m_valid || slot < 0 || slot >= 3 || !window)
        return nullptr;

    Slot &target = m_slots[slot];
    if (!target.mutex || target.mutex->AcquireSync(1, 2000) != S_OK)
        return nullptr;

    m_context->CopyResource(target.owned.Get(), target.shared.Get());
    target.mutex->ReleaseSync(0);

    QSGTexture *texture = QNativeInterface::QSGD3D11Texture::fromNative(
        target.owned.Get(), window, QSize(m_width, m_height), {});
    return texture;
}

} // namespace VideoEditor

#endif
```

`QNativeInterface::QSGD3D11Texture::fromNative` 必须在场景图渲染线程调用；`present()` 只由 `updatePaintNode()` 调用。CMake 中链接 `Qt6::Quick`。

- [ ] **Step 2: 实现预览项**

`src/ui/VideoPreviewItem.h`：

```cpp
#pragma once

#include <array>
#include <atomic>
#include <QQuickItem>
#include <QSize>

#include "FrameBridge.h"

namespace VideoEditor {

class VideoPreviewItem : public QQuickItem {
    Q_OBJECT

public:
    explicit VideoPreviewItem(QQuickItem *parent = nullptr);

    void setSharedHandles(const std::array<quintptr, 3> &handles);
    void submitFrame(int slot, const QSize &size, int rotationDegrees,
                     qint64 ptsUs);
    void resetBridge();

signals:
    void framePresented(qint64 ptsUs);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;

private:
    QRectF aspectFitRect(const QSizeF &frameSize) const;

    FrameBridge m_bridge;
    std::array<quintptr, 3> m_handles{};
    QSize m_pendingSize;
    qint64 m_pendingPts = 0;
    int m_pendingSlot = -1;
    int m_rotation = 0;
    bool m_hasPending = false;
    std::atomic_bool m_resetRequested{false};
};

} // namespace VideoEditor
```

`src/ui/VideoPreviewItem.cpp`：

```cpp
#include "VideoPreviewItem.h"

#include <QSGSimpleTextureNode>

namespace VideoEditor {

VideoPreviewItem::VideoPreviewItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void VideoPreviewItem::setSharedHandles(const std::array<quintptr, 3> &handles)
{
    if (m_handles == handles)
        return;
    m_handles = handles;
    m_resetRequested.store(true);
    update();
}

void VideoPreviewItem::resetBridge()
{
    m_hasPending = false;
    m_resetRequested.store(true);
    update();
}

void VideoPreviewItem::submitFrame(int slot, const QSize &size,
                                   int rotationDegrees, qint64 ptsUs)
{
    m_pendingSlot = slot;
    m_pendingSize = size;
    m_rotation = rotationDegrees;
    m_pendingPts = ptsUs;
    m_hasPending = true;
    update();
}

QRectF VideoPreviewItem::aspectFitRect(const QSizeF &frameSize) const
{
    if (frameSize.isEmpty())
        return boundingRect();
    QSizeF scaled = frameSize;
    scaled.scale(boundingRect().size(), Qt::KeepAspectRatio);
    QRectF result(QPointF(0, 0), scaled);
    result.moveCenter(boundingRect().center());
    return result;
}

QSGNode *VideoPreviewItem::updatePaintNode(QSGNode *oldNode,
                                           UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }

    if (m_resetRequested.exchange(false)) {
        m_bridge = FrameBridge();
        m_hasPending = false;
        node->setTexture(nullptr);
    }

    if (!m_bridge.isValid() && window() && m_pendingSize.isValid()) {
        m_bridge.initialize(window(), m_handles, m_pendingSize.width(),
                            m_pendingSize.height());
    }

    if (m_hasPending && m_bridge.isValid()) {
        if (QSGTexture *texture = m_bridge.present(m_pendingSlot, window())) {
            node->setTexture(texture);
            node->setRect(aspectFitRect(m_pendingSize));
            node->markDirty(QSGNode::DirtyMaterial);
            emit framePresented(m_pendingPts);
            m_hasPending = false;
        }
    }

    return node;
}

} // namespace VideoEditor
```

- [ ] **Step 3: 实现解码工作线程**

`src/ui/DecoderWorker.h`：

```cpp
#pragma once

#include <array>
#include <memory>
#include <QObject>
#include <QSize>

#include "IVideoDecoder.h"
#include "MediaInfo.h"
#include "SharedTextureRing.h"

namespace VideoEditor {

class D3D11Context;
class FramePublisher;

class DecoderWorker : public QObject {
    Q_OBJECT

public:
    explicit DecoderWorker(QObject *parent = nullptr);
    ~DecoderWorker() override;

public slots:
    void open(const QString &path, const QString &proxyPath);
    void requestFrame();
    void seek(qint64 ptsUs);

signals:
    void opened(VideoEditor::MediaInfo info, bool hardware,
                QString hardwareFallbackReason);
    void frameReady(int slot, QSize size, int rotationDegrees, qint64 ptsUs,
                    quintptr handle0, quintptr handle1, quintptr handle2);
    void error(QString message);

private:
    void clearDecoder();

    std::unique_ptr<D3D11Context> m_context;
    std::unique_ptr<IVideoDecoder> m_decoder;
    std::unique_ptr<SharedTextureRing> m_ring;
    std::unique_ptr<FramePublisher> m_publisher;
    MediaInfo m_info;
    bool m_busy = false;
    bool m_eof = false;
};

} // namespace VideoEditor
```

`src/ui/DecoderWorker.cpp`：

```cpp
#include "DecoderWorker.h"

#include <QFileInfo>
#include <QMetaType>

#include "D3D11Context.h"
#include "FramePublisher.h"
#include "MediaProbe.h"
#include "VideoDecoderFactory.h"

Q_DECLARE_METATYPE(VideoEditor::MediaInfo)

namespace VideoEditor {

DecoderWorker::DecoderWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<VideoEditor::MediaInfo>("VideoEditor::MediaInfo");
}

DecoderWorker::~DecoderWorker() = default;

void DecoderWorker::clearDecoder()
{
    m_publisher.reset();
    m_ring.reset();
    m_decoder.reset();
    m_context.reset();
    m_eof = false;
    m_busy = false;
}

void DecoderWorker::open(const QString &path, const QString &proxyPath)
{
    clearDecoder();

    const bool usingProxy = QFileInfo::exists(proxyPath);
    const QString actualPath = usingProxy ? proxyPath : path;
    QString error;
    auto info = MediaProbe::probe(path, &error);
    if (!info) {
        emit this->error(error);
        return;
    }
    m_info = *info;
    m_info.path = path;
    // ffmpeg 生成代理时会应用显示矩阵，代理画面已经摆正，不能重复旋转。
    if (usingProxy)
        m_info.rotationDegrees = 0;

    m_context = D3D11Context::create(true, &error);
    if (!m_context) {
        // 视频设备不可用时，仍然创建普通 D3D11 设备用于软件帧上传。
        QString softwareError;
        m_context = D3D11Context::create(false, &softwareError);
        if (!m_context) {
            emit this->error(QStringLiteral("%1\n%2").arg(error, softwareError));
            return;
        }
    }

    auto result = createVideoDecoder(actualPath, *m_context, m_info);
    if (!result.decoder) {
        emit this->error(result.error);
        return;
    }
    m_decoder = std::move(result.decoder);

    DecodedFrame firstFrame;
    if (!m_decoder->decodeNext(firstFrame, &error)) {
        emit this->error(error.isEmpty()
                             ? QStringLiteral("视频没有可解码的帧。")
                             : error);
        return;
    }

    m_ring = std::make_unique<SharedTextureRing>(*m_context);
    if (!m_ring->create(firstFrame.width, firstFrame.height,
                        SharedTextureRing::kSlotCount, &error)) {
        emit this->error(error);
        return;
    }

    m_publisher = std::make_unique<FramePublisher>(*m_context, *m_ring);
    if (!m_publisher->publish(firstFrame, m_info.rotationDegrees, &error)) {
        emit this->error(error);
        return;
    }
    m_eof = false;

    emit opened(m_info, m_decoder->isHardware(), result.hardwareFallbackReason);
    emit frameReady(m_publisher->lastWrittenSlot(),
                    QSize(firstFrame.width, firstFrame.height),
                    m_info.rotationDegrees, firstFrame.ptsUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)));
}

void DecoderWorker::requestFrame()
{
    if (!m_decoder || m_busy || m_eof)
        return;

    m_busy = true;
    DecodedFrame frame;
    QString error;
    if (!m_decoder->decodeNext(frame, &error)) {
        m_busy = false;
        if (!error.isEmpty())
            emit this->error(error);
        else
            m_eof = true;
        return;
    }

    if (!m_publisher->publish(frame, m_info.rotationDegrees, &error)) {
        m_busy = false;
        emit this->error(error);
        return;
    }

    m_busy = false;
    emit frameReady(m_publisher->lastWrittenSlot(),
                    QSize(frame.width, frame.height),
                    m_info.rotationDegrees, frame.ptsUs,
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(0)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(1)),
                    reinterpret_cast<quintptr>(m_ring->sharedHandle(2)));
}

void DecoderWorker::seek(qint64 ptsUs)
{
    if (!m_decoder)
        return;
    QString error;
    if (!m_decoder->seek(ptsUs, &error)) {
        emit this->error(error);
        return;
    }
    m_eof = false;
}

} // namespace VideoEditor
```

- [ ] **Step 4: 实现播放控制器**

`src/ui/PreviewController.h`：

```cpp
#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include "MediaInfo.h"

namespace VideoEditor {

class DecoderWorker;
class VideoPreviewItem;

class PreviewController : public QObject {
    Q_OBJECT
    Q_PROPERTY(VideoEditor::VideoPreviewItem *item READ item WRITE setItem
                   NOTIFY itemChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)
    Q_PROPERTY(QString timecode READ timecode NOTIFY positionChanged)
    Q_PROPERTY(QString decodeMode READ decodeMode NOTIFY decodeModeChanged)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY mediaChanged)

public:
    explicit PreviewController(QObject *parent = nullptr);
    ~PreviewController() override;

    VideoPreviewItem *item() const { return m_item; }
    void setItem(VideoPreviewItem *item);

    bool playing() const { return m_playing; }
    qint64 positionMs() const { return m_positionUs / 1000; }
    qint64 durationMs() const { return m_durationUs / 1000; }
    QString timecode() const;
    QString decodeMode() const { return m_decodeMode; }
    QString mediaName() const { return m_mediaName; }

    Q_INVOKABLE void openMedia(const QString &path,
                               const QString &proxyPath = {});
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekMs(qint64 ms);
    Q_INVOKABLE void stepFrames(int delta);

signals:
    void itemChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void decodeModeChanged();
    void mediaChanged();
    void framePresented(qint64 ptsUs);
    void error(QString message);

private:
    void onOpened(const MediaInfo &info, bool hardware,
                  const QString &fallbackReason);
    void onFrameReady(int slot, const QSize &size, int rotation, qint64 ptsUs,
                      quintptr h0, quintptr h1, quintptr h2);
    void requestNextFrame();

    QThread m_thread;
    DecoderWorker *m_worker = nullptr;
    QPointer<VideoPreviewItem> m_item;
    QTimer m_frameTimer;
    MediaInfo m_info;
    QString m_mediaName;
    QString m_decodeMode;
    qint64 m_positionUs = 0;
    qint64 m_durationUs = 0;
    bool m_playing = false;
    bool m_framePending = false;
};

} // namespace VideoEditor
```

`src/ui/PreviewController.cpp`：

```cpp
#include "PreviewController.h"

#include <QFileInfo>
#include <QMetaObject>

#include "DecoderWorker.h"
#include "TimeBase.h"
#include "VideoPreviewItem.h"

namespace VideoEditor {

PreviewController::PreviewController(QObject *parent)
    : QObject(parent)
    , m_worker(new DecoderWorker)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &DecoderWorker::opened, this, &PreviewController::onOpened);
    connect(m_worker, &DecoderWorker::frameReady, this, &PreviewController::onFrameReady);
    connect(m_worker, &DecoderWorker::error, this, &PreviewController::error);
    m_thread.start();

    m_frameTimer.setSingleShot(false);
    connect(&m_frameTimer, &QTimer::timeout, this, &PreviewController::requestNextFrame);
}

PreviewController::~PreviewController()
{
    m_frameTimer.stop();
    m_thread.quit();
    m_thread.wait();
}

void PreviewController::setItem(VideoPreviewItem *item)
{
    if (m_item == item)
        return;
    if (m_item)
        disconnect(m_item, nullptr, this, nullptr);
    m_item = item;
    if (m_item) {
        connect(m_item, &VideoPreviewItem::framePresented,
                this, &PreviewController::framePresented, Qt::QueuedConnection);
    }
    emit itemChanged();
}

void PreviewController::openMedia(const QString &path, const QString &proxyPath)
{
    pause();
    if (m_item)
        m_item->resetBridge();
    m_info = MediaInfo{};
    m_positionUs = 0;
    m_durationUs = 0;
    m_mediaName = QFileInfo(path).fileName();
    m_decodeMode.clear();
    m_framePending = false;
    emit positionChanged();
    emit durationChanged();
    emit mediaChanged();
    emit decodeModeChanged();

    QMetaObject::invokeMethod(m_worker, "open", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(QString, proxyPath));
}

void PreviewController::play()
{
    if (!m_info.supported || m_playing)
        return;
    m_playing = true;
    if (m_frameTimer.interval() <= 0)
        m_frameTimer.setInterval(33);
    m_frameTimer.start();
    emit playingChanged();
    requestNextFrame();
}

void PreviewController::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_frameTimer.stop();
    emit playingChanged();
}

void PreviewController::togglePlay()
{
    m_playing ? pause() : play();
}

void PreviewController::seekMs(qint64 ms)
{
    m_positionUs = qMax<qint64>(0, ms) * 1000;
    emit positionChanged();
    QMetaObject::invokeMethod(m_worker, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, m_positionUs));
    m_framePending = false;
    requestNextFrame();
}

void PreviewController::stepFrames(int delta)
{
    pause();
    const Rational fps = m_info.frameRate.isValid() ? m_info.frameRate
                                                     : Rational{30, 1};
    const int64_t frameUs = framesToMicros(1, fps);
    const qint64 target = qMax<qint64>(0, m_positionUs + delta * frameUs);
    seekMs(target / 1000);
}

void PreviewController::onOpened(const MediaInfo &info, bool hardware,
                                 const QString &fallbackReason)
{
    m_info = info;
    m_durationUs = info.durationUs;

    QString mode = hardware ? QStringLiteral("硬件 D3D11VA")
                            : QStringLiteral("软件解码");
    if (!hardware && !fallbackReason.isEmpty())
        mode += QStringLiteral("（%1）").arg(fallbackReason);
    m_decodeMode = mode;

    const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational{30, 1};
    m_frameTimer.setInterval(qMax(1, int(1000.0 * fps.den / fps.num)));

    emit durationChanged();
    emit decodeModeChanged();
    requestNextFrame();
}

void PreviewController::onFrameReady(int slot, const QSize &size, int rotation,
                                     qint64 ptsUs, quintptr h0, quintptr h1,
                                     quintptr h2)
{
    m_framePending = false;
    m_positionUs = ptsUs;
    emit positionChanged();

    if (m_item) {
        m_item->setSharedHandles({h0, h1, h2});
        m_item->submitFrame(slot, size, rotation, ptsUs);
    }
}

void PreviewController::requestNextFrame()
{
    if (!m_info.supported || m_framePending)
        return;
    m_framePending = true;
    QMetaObject::invokeMethod(m_worker, "requestFrame", Qt::QueuedConnection);
}

QString PreviewController::timecode() const
{
    return formatTimecode(m_positionUs,
                          m_info.frameRate.isValid() ? m_info.frameRate
                                                     : Rational{30, 1});
}

} // namespace VideoEditor
```

需要包含 `<QFileInfo>`。另外 `VideoPreviewItem` 的 `framePresented` 信号应转发给 `PreviewController::framePresented`；在 `setItem()` 中连接：

```cpp
connect(item, &VideoPreviewItem::framePresented,
        this, &PreviewController::framePresented);
```

- [ ] **Step 5: 接入 QML 与 selftest**

修改 `src/ui/CMakeLists.txt`：

```cmake
add_library(videoeditor_ui STATIC
  MediaListModel.h
  MediaListModel.cpp
  FrameBridge.h
  FrameBridge.cpp
  VideoPreviewItem.h
  VideoPreviewItem.cpp
  DecoderWorker.h
  DecoderWorker.cpp
  PreviewController.h
  PreviewController.cpp
)
target_include_directories(videoeditor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_ui
  PUBLIC videoeditor_core videoeditor_media Qt6::Core Qt6::Gui Qt6::Quick
  PRIVATE d3d11 dxgi
)
```

修改 `src/app/AppController.h`，增加：

```cpp
class PreviewController;
PreviewController *preview() const { return m_preview; }
void selftest(const QString &path);
signals:
    void selftestFramePresented();
private:
    PreviewController *m_preview = nullptr;
```

修改 `src/app/AppController.cpp`：

```cpp
#include "PreviewController.h"

// 构造函数中：
m_preview = new PreviewController(this);
connect(m_preview, &PreviewController::framePresented, this,
        [this](qint64) { emit selftestFramePresented(); });

// 新方法：
void AppController::selftest(const QString &path)
{
    importUrls({QUrl::fromLocalFile(path)});
    if (m_mediaModel->rowCount() <= 0)
        return;
    const Asset &asset = m_document->project().assets.first();
    m_preview->openMedia(asset.path, asset.proxyPath);
}
```

修改 `src/app/main.cpp`：

```cpp
#include <QCommandLineParser>
#include <QTimer>

#include "PreviewController.h"

// 在 QGuiApplication 创建后解析参数
QCommandLineParser parser;
parser.addOption({QStringLiteral("selftest"), QStringLiteral("Open a file and exit after first frame"),
                  QStringLiteral("path")});
parser.process(app);

VideoEditor::AppController controller;
QQmlApplicationEngine engine;
engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
engine.rootContext()->setContextProperty(QStringLiteral("Preview"),
                                         controller.preview());
...
engine.loadFromModule(...);

if (parser.isSet(QStringLiteral("selftest"))) {
    QObject::connect(&controller, &VideoEditor::AppController::selftestFramePresented,
                     &app, [] {
                         qInfo("SELFTEST_FRAME_OK");
                         QCoreApplication::exit(0);
                     });
    QTimer::singleShot(0, &controller, [&controller, &parser] {
        controller.selftest(parser.value(QStringLiteral("selftest")));
    });
    QTimer::singleShot(15000, &app, [] {
        qCritical("SELFTEST_TIMEOUT");
        QCoreApplication::exit(2);
    });
}
```

修改 `src/app/Main.qml`，将媒体列表右侧改为预览区：

```qml
import VideoEditor 1.0

RowLayout {
    anchors.fill: parent
    spacing: 1

    Rectangle {
        Layout.preferredWidth: 320
        Layout.fillHeight: true
        color: "#1b1b1b"

        ListView {
            id: mediaList
            anchors.fill: parent
            model: App.mediaModel
            delegate: ItemDelegate {
                width: ListView.view.width
                text: model.name + "\n" + model.codec + " · " + model.resolution
                      + " · " + model.duration
                onClicked: Preview.openMedia(model.path, model.proxyPath)
            }
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        Rectangle {
            anchors.fill: parent
            color: "black"
        }

        VideoPreview {
            id: previewItem
            anchors.fill: parent
        }

        Component.onCompleted: Preview.item = previewItem

        Label {
            anchors.centerIn: parent
            visible: !Preview.mediaName
            text: "选择左侧素材开始预览"
            color: "#888888"
        }
    }
}

footer: RowLayout {
    ToolButton { text: Preview.playing ? "暂停" : "播放"; onClicked: Preview.togglePlay() }
    ToolButton { text: "◀"; onClicked: Preview.stepFrames(-1) }
    ToolButton { text: "▶"; onClicked: Preview.stepFrames(1) }
    Label { text: Preview.timecode; color: "white" }
    Slider {
        Layout.fillWidth: true
        from: 0
        to: Math.max(1, Preview.durationMs)
        value: Preview.positionMs
        onMoved: Preview.seekMs(value)
    }
    Label { text: Preview.decodeMode; color: "#cccccc" }
}
```

在 `main.cpp` 中注册类型：

```cpp
#include <QQmlEngine>
#include "VideoPreviewItem.h"
qmlRegisterType<VideoEditor::VideoPreviewItem>("VideoEditor", 1, 0, "VideoPreview");
```

在 `qt_add_qml_module` 之后、加载 QML 之前调用。

- [ ] **Step 6: 增加 selftest 到 CTest**

修改 `tests/CMakeLists.txt`，追加：

```cmake
add_test(NAME preview_selftest
         COMMAND VideoEditor --selftest "${CMAKE_SOURCE_DIR}/tests/fixtures/h264.mp4")
set_tests_properties(preview_selftest PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 30
  LABELS "gpu"
)
```

需要确保 `VideoEditor` 目标在测试目录可见；在 `tests/CMakeLists.txt` 中直接使用目标名即可。

- [ ] **Step 7: 构建并运行测试**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`preview_selftest` 在 30 秒内退出 0，输出包含 `SELFTEST_FRAME_OK`。如果测试环境无桌面会话，测试会超时；本地交互式运行应当通过。

- [ ] **Step 8: 手动验证**

```powershell
windeployqt --qmldir src/app build/windows-x64-debug/src/app/VideoEditor.exe
./build/windows-x64-debug/src/app/VideoEditor.exe
```

依次验证：

1. 导入 `h264.mp4`，单击列表项，预览出现画面。
2. 点击播放，画面连续；点击暂停，停在当前帧。
3. 点击 `◀`/`▶`，画面逐帧变化，时间码同步。
4. 拖动进度条，画面跳转。
5. 导入 `rotated.mp4`，画面方向竖直向上，不横躺。
6. 底部显示“硬件 D3D11VA”或“软件解码（原因）”。

- [ ] **Step 9: 提交**

```bash
git add src tests
git commit -m "feat(ui): present decoded frames in Qt Quick and add playback controls"
```

---

### Task 9: 代理媒体与缓存

**Files:**
- Create: `src/media/cache/CacheStore.h`
- Create: `src/media/cache/CacheStore.cpp`
- Create: `src/media/proxy/AssetFingerprint.h`
- Create: `src/media/proxy/AssetFingerprint.cpp`
- Create: `src/media/proxy/ProxyPlanner.h`
- Create: `src/media/proxy/ProxyPlanner.cpp`
- Create: `src/media/proxy/ProxyGenerator.h`
- Create: `src/media/proxy/ProxyGenerator.cpp`
- Create: `src/media/proxy/ProxyService.h`
- Create: `src/media/proxy/ProxyService.cpp`
- Modify: `src/media/CMakeLists.txt`
- Modify: `src/app/AppController.h`
- Modify: `src/app/AppController.cpp`
- Modify: `src/app/Main.qml`
- Create: `tests/media/ProxyTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MediaInfo`、`MediaProbe`.
- Produces:
  - `QString fingerprintFile(const QString&)`
  - `ProxyPlan planProxy(const MediaInfo&)`
  - `CacheStore::proxyPath(key)`
  - `ProxyService::request(assetId, sourcePath, height)`
  - `ProxyService::ready(assetId, proxyPath)` / `failed(assetId, message)`

- [ ] **Step 1: 写失败测试**

`tests/media/ProxyTest.cpp`：

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "AssetFingerprint.h"
#include "CacheStore.h"
#include "MediaProbe.h"
#include "ProxyGenerator.h"
#include "ProxyPlanner.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

} // namespace

TEST(ProxyPlanner, TriggersOnlyAbove1080p)
{
    MediaInfo info;
    info.width = 1920;
    info.height = 1080;
    EXPECT_FALSE(planProxy(info).needed);

    info.width = 3840;
    info.height = 2160;
    const ProxyPlan plan = planProxy(info);
    EXPECT_TRUE(plan.needed);
    EXPECT_EQ(plan.height, 540);
}

TEST(AssetFingerprint, ChangesWithContentAndMtime)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.bin"));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("hello");
    file.close();

    const QString first = fingerprintFile(path);
    EXPECT_FALSE(first.isEmpty());

    ASSERT_TRUE(file.open(QIODevice::Append));
    file.write("world");
    file.close();
    EXPECT_NE(fingerprintFile(path), first);
}

TEST(CacheStore, BuildsProxyPathsUnderRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CacheStore cache(dir.path());
    const QString path = cache.proxyPath(QStringLiteral("abc-h540"));
    EXPECT_TRUE(path.startsWith(QDir(dir.path()).absolutePath()));
    EXPECT_TRUE(path.endsWith(QStringLiteral(".mp4")));
    EXPECT_TRUE(cache.clear());
}

TEST(ProxyGenerator, Generates540pProxy)
{
    const QString source = fixture(QStringLiteral("highres.mp4"));
    if (!QFile::exists(source))
        GTEST_SKIP() << "missing highres fixture";

    const QString ffmpeg = qEnvironmentVariable("VIDEOEDITOR_FFMPEG",
                                                QStringLiteral("ffmpeg"));
    if (QStandardPaths::findExecutable(ffmpeg).isEmpty())
        GTEST_SKIP() << "ffmpeg not found";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("proxy.mp4"));

    ProxyGenerator generator(ffmpeg);
    QSignalSpy finished(&generator, &ProxyGenerator::finished);
    QSignalSpy failed(&generator, &ProxyGenerator::failed);
    generator.start(source, output, 540);

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() + failed.count() > 0, 120000);
    ASSERT_EQ(failed.count(), 0);
    ASSERT_EQ(finished.count(), 1);
    EXPECT_TRUE(QFileInfo::exists(output));

    const auto info = MediaProbe::probe(output, nullptr);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->height, 540);
}
```

修改 `tests/CMakeLists.txt`：

```cmake
add_executable(media_tests
  media/MediaProbeTest.cpp
  media/D3D11PipelineTest.cpp
  media/DecoderTest.cpp
  media/ProxyTest.cpp
  TestMain.cpp
)
target_link_libraries(media_tests PRIVATE Qt6::Test)
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build --preset windows-x64-debug
```

预期：编译失败，缺少 `ProxyPlanner.h`。

- [ ] **Step 3: 实现指纹、计划与缓存**

`src/media/proxy/AssetFingerprint.h`：

```cpp
#pragma once

#include <QString>

namespace VideoEditor {

QString fingerprintFile(const QString &path);

} // namespace VideoEditor
```

`src/media/proxy/AssetFingerprint.cpp`：

```cpp
#include "AssetFingerprint.h"

#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>

namespace VideoEditor {

QString fingerprintFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QFileInfo info(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));

    constexpr qint64 kChunk = 1024 * 1024;
    const QByteArray head = file.read(kChunk);
    hash.addData(head);
    if (info.size() > kChunk * 2) {
        file.seek(info.size() - kChunk);
        hash.addData(file.read(kChunk));
    }

    return QString::fromLatin1(hash.result().toHex());
}

} // namespace VideoEditor
```

`src/media/proxy/ProxyPlanner.h`：

```cpp
#pragma once

#include "MediaInfo.h"

namespace VideoEditor {

struct ProxyPlan {
    bool needed = false;
    int height = 540;
    QString reason;
};

ProxyPlan planProxy(const MediaInfo &info);

} // namespace VideoEditor
```

`src/media/proxy/ProxyPlanner.cpp`：

```cpp
#include "ProxyPlanner.h"

namespace VideoEditor {

ProxyPlan planProxy(const MediaInfo &info)
{
    ProxyPlan plan;
    if (info.width > 1920 || info.height > 1080) {
        plan.needed = true;
        plan.height = 540;
        plan.reason = QStringLiteral("素材分辨率高于 1080p，已生成代理。");
    }
    return plan;
}

} // namespace VideoEditor
```

`src/media/cache/CacheStore.h`：

```cpp
#pragma once

#include <QString>

namespace VideoEditor {

class CacheStore {
public:
    explicit CacheStore(QString root);

    QString root() const { return m_root; }
    QString proxyPath(const QString &key) const;
    bool ensureProxyDir(QString *error = nullptr) const;
    qint64 sizeOnDisk(QString *error = nullptr) const;
    bool clear(QString *error = nullptr) const;

private:
    QString m_root;
};

} // namespace VideoEditor
```

`src/media/cache/CacheStore.cpp`：

```cpp
#include "CacheStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

CacheStore::CacheStore(QString root)
    : m_root(std::move(root))
{
}

QString CacheStore::proxyPath(const QString &key) const
{
    return QDir(m_root).filePath(QStringLiteral("proxies/%1/proxy.mp4").arg(key));
}

bool CacheStore::ensureProxyDir(QString *error) const
{
    if (!QDir().mkpath(QDir(m_root).filePath(QStringLiteral("proxies")))) {
        setError(error, QStringLiteral("无法创建代理缓存目录。"));
        return false;
    }
    return true;
}

qint64 CacheStore::sizeOnDisk(QString *error) const
{
    QDirIterator it(m_root, QDir::Files, QDirIterator::Subdirectories);
    qint64 total = 0;
    while (it.hasNext())
        total += QFileInfo(it.next()).size();
    return total;
}

bool CacheStore::clear(QString *error) const
{
    QDir dir(m_root);
    if (!dir.exists())
        return true;
    if (!dir.removeRecursively()) {
        setError(error, QStringLiteral("无法清理缓存目录。"));
        return false;
    }
    return true;
}

} // namespace VideoEditor
```

`proxyPath` 的目录由生成器创建；`ProxyGenerator` 在写临时文件前 `mkpath(QFileInfo(output).absolutePath())`。

- [ ] **Step 4: 实现代理生成与服务**

`src/media/proxy/ProxyGenerator.h`：

```cpp
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace VideoEditor {

class ProxyGenerator : public QObject {
    Q_OBJECT

public:
    explicit ProxyGenerator(QString ffmpegPath, QObject *parent = nullptr);

    void start(const QString &source, const QString &output, int height);
    void cancel();

signals:
    void progress(int percent);
    void finished(QString outputPath);
    void failed(QString message);

private:
    QString m_ffmpegPath;
    QProcess *m_process = nullptr;
    QString m_output;
    QString m_temp;
};

} // namespace VideoEditor
```

`src/media/proxy/ProxyGenerator.cpp`：

```cpp
#include "ProxyGenerator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace VideoEditor {

ProxyGenerator::ProxyGenerator(QString ffmpegPath, QObject *parent)
    : QObject(parent)
    , m_ffmpegPath(std::move(ffmpegPath))
{
}

void ProxyGenerator::start(const QString &source, const QString &output, int height)
{
    if (m_process) {
        emit failed(QStringLiteral("代理任务正在进行。"));
        return;
    }

    m_output = output;
    m_temp = output + QStringLiteral(".tmp.mp4");
    QDir().mkpath(QFileInfo(output).absolutePath());
    QFile::remove(m_temp);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                emit failed(QStringLiteral("无法启动 ffmpeg：%1")
                                .arg(m_process->errorString()));
                m_process->deleteLater();
                m_process = nullptr;
            });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
                if (!m_process)
                    return;
                const QString temp = m_temp;
                m_process->deleteLater();
                m_process = nullptr;

                if (status != QProcess::NormalExit || exitCode != 0) {
                    QFile::remove(temp);
                    emit failed(QStringLiteral("代理生成失败（ffmpeg 退出码 %1）。")
                                    .arg(exitCode));
                    return;
                }
                if (!QFile::rename(temp, m_output)) {
                    QFile::remove(temp);
                    emit failed(QStringLiteral("无法移动代理文件到缓存目录。"));
                    return;
                }
                emit finished(m_output);
            });

    const QStringList arguments = {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-i"), source,
        QStringLiteral("-vf"), QStringLiteral("scale=-2:%1").arg(height),
        QStringLiteral("-c:v"), QStringLiteral("mjpeg"),
        QStringLiteral("-q:v"), QStringLiteral("3"),
        QStringLiteral("-an"),
        QStringLiteral("-g"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("mp4"),
        m_temp,
    };

    m_process->start(m_ffmpegPath, arguments);
}

void ProxyGenerator::cancel()
{
    if (!m_process)
        return;
    m_process->kill();
}

} // namespace VideoEditor
```

`src/media/proxy/ProxyService.h`：

```cpp
#pragma once

#include <QObject>
#include <QQueue>
#include <QString>

#include "CacheStore.h"

namespace VideoEditor {

class ProxyGenerator;

class ProxyService : public QObject {
    Q_OBJECT

public:
    explicit ProxyService(QString cacheRoot, QString ffmpegPath,
                          QObject *parent = nullptr);

    void request(const QString &assetId, const QString &sourcePath,
                 int height = 540);
    bool isBusy() const { return m_generator != nullptr; }

signals:
    void ready(QString assetId, QString proxyPath);
    void failed(QString assetId, QString message);

private:
    struct Job {
        QString assetId;
        QString sourcePath;
        QString outputPath;
        int height = 540;
    };

    void startNext();

    CacheStore m_cache;
    QString m_ffmpegPath;
    QQueue<Job> m_queue;
    ProxyGenerator *m_generator = nullptr;
};

} // namespace VideoEditor
```

`src/media/proxy/ProxyService.cpp`：

```cpp
#include "ProxyService.h"

#include <QFileInfo>

#include "AssetFingerprint.h"
#include "MediaProbe.h"
#include "ProxyGenerator.h"

namespace VideoEditor {

ProxyService::ProxyService(QString cacheRoot, QString ffmpegPath, QObject *parent)
    : QObject(parent)
    , m_cache(std::move(cacheRoot))
    , m_ffmpegPath(std::move(ffmpegPath))
{
}

void ProxyService::request(const QString &assetId, const QString &sourcePath,
                           int height)
{
    const QString fingerprint = fingerprintFile(sourcePath);
    if (fingerprint.isEmpty()) {
        emit failed(assetId, QStringLiteral("无法读取素材指纹。"));
        return;
    }

    const QString key = QStringLiteral("%1-h%2").arg(fingerprint).arg(height);
    const QString output = m_cache.proxyPath(key);
    if (QFileInfo::exists(output)) {
        emit ready(assetId, output);
        return;
    }

    m_queue.enqueue(Job{assetId, sourcePath, output, height});
    startNext();
}

void ProxyService::startNext()
{
    if (m_generator || m_queue.isEmpty())
        return;

    const Job job = m_queue.dequeue();
    m_generator = new ProxyGenerator(m_ffmpegPath, this);

    connect(m_generator, &ProxyGenerator::finished, this,
            [this, job](const QString &path) {
                m_generator->deleteLater();
                m_generator = nullptr;
                emit ready(job.assetId, path);
                startNext();
            });

    connect(m_generator, &ProxyGenerator::failed, this,
            [this, job](const QString &message) {
                m_generator->deleteLater();
                m_generator = nullptr;
                emit failed(job.assetId, message);
                startNext();
            });

    m_generator->start(job.sourcePath, job.outputPath, job.height);
}

} // namespace VideoEditor
```

修改 `src/media/CMakeLists.txt`，加入：

```cmake
proxy/AssetFingerprint.h
proxy/AssetFingerprint.cpp
proxy/ProxyPlanner.h
proxy/ProxyPlanner.cpp
proxy/ProxyGenerator.h
proxy/ProxyGenerator.cpp
proxy/ProxyService.h
proxy/ProxyService.cpp
cache/CacheStore.h
cache/CacheStore.cpp
```

- [ ] **Step 5: 接入 AppController 与界面**

修改 `src/app/AppController.h`，增加：

```cpp
class ProxyService;
Q_PROPERTY(bool useProxy READ useProxy WRITE setUseProxy NOTIFY useProxyChanged)
bool useProxy() const { return m_useProxy; }
void setUseProxy(bool enabled);
ProxyService *proxyService() const { return m_proxyService; }
Q_INVOKABLE void openAsset(const QString &id);
signals:
    void useProxyChanged();
private:
    ProxyService *m_proxyService = nullptr;
    bool m_useProxy = true;
```

修改 `src/app/AppController.cpp`：

```cpp
#include "ProxyService.h"
#include "ProxyPlanner.h"

// 构造函数中：
const QString cacheRoot = QDir(appData).filePath(QStringLiteral("cache"));
const QString ffmpegPath = qEnvironmentVariable(
    "VIDEOEDITOR_FFMPEG", QStringLiteral("ffmpeg"));
m_proxyService = new ProxyService(cacheRoot, ffmpegPath, this);

connect(m_proxyService, &ProxyService::ready, this,
        [this](const QString &assetId, const QString &proxyPath) {
            m_document->setAssetProxyPath(assetId, proxyPath);
        });
connect(m_proxyService, &ProxyService::failed, this,
        [this](const QString &, const QString &message) {
            setLastError(message);
        });

// importUrls 成功添加后：
const Asset &added = m_document->project().assets.last();
const ProxyPlan plan = planProxy(added.info);
if (plan.needed)
    m_proxyService->request(added.id, added.path, plan.height);

// setUseProxy：
void AppController::setUseProxy(bool enabled)
{
    if (m_useProxy == enabled)
        return;
    m_useProxy = enabled;
    emit useProxyChanged();
}
```

`openMedia` 的调用方改为：

```cpp
// AppController 增加：
Q_INVOKABLE void openAsset(const QString &id);
void AppController::openAsset(const QString &id)
{
    for (const Asset &asset : m_document->project().assets) {
        if (asset.id == id) {
            m_preview->openMedia(asset.path,
                                 m_useProxy ? asset.proxyPath : QString());
            return;
        }
    }
}
```

QML 列表改为：

```qml
onClicked: App.openAsset(model.id)
```

底部增加代理开关：

```qml
CheckBox {
    text: "使用代理"
    checked: App.useProxy
    onToggled: App.useProxy = checked
}
```

- [ ] **Step 6: 运行测试确认通过**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：`ProxyPlanner`、`AssetFingerprint`、`CacheStore` 通过；`ProxyGenerator` 在系统有 ffmpeg 时通过，否则 skipped。

- [ ] **Step 7: 手动验证代理**

```powershell
$env:VIDEOEDITOR_FFMPEG = "ffmpeg"
./build/windows-x64-debug/src/app/VideoEditor.exe
```

导入 `highres.mp4`。预期：列表项出现后，后台开始生成代理；等待完成，打开素材时底部“使用代理”勾选状态下预览正常。缓存目录 `%LOCALAPPDATA%/VideoEditor/cache/proxies/` 下出现代理文件。取消勾选后重新打开素材，使用源文件预览。

- [ ] **Step 8: 提交**

```bash
git add src tests
git commit -m "feat(media): generate cached proxies for high-resolution sources"
```

---

### Task 10: 端到端验收与文档

**Files:**
- Create: `README.md`
- Modify: `tests/CMakeLists.txt`
- Modify: `docs/superpowers/specs/2026-11-14-video-editor-design.md`（仅在实现与规格不一致时补充已知限制）

**Interfaces:**
- Consumes: 全部 M1 功能。
- Produces: 可重复的验收命令与已知限制说明。

- [ ] **Step 1: 为每种格式增加 selftest**

在 `tests/CMakeLists.txt` 中追加：

```cmake
foreach(_fixture IN ITEMS
    mpeg4.mp4 h264.mp4 h265.mp4 h265_10bit.mp4 wmv2.wmv rotated.mp4
    "中文 文件名.mp4" noaudio.mp4)
  get_filename_component(_name "${_fixture}" NAME_WE)
  add_test(NAME "preview_${_name}"
           COMMAND VideoEditor --selftest
                   "${CMAKE_SOURCE_DIR}/tests/fixtures/${_fixture}")
  set_tests_properties("preview_${_name}" PROPERTIES
    ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
    ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
    TIMEOUT 30
    LABELS "gpu"
  )
endforeach()
```

- [ ] **Step 2: 运行完整测试**

```bash
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

预期：

- 所有非 GPU 测试通过。
- 所有 `preview_*` 测试通过；H.265 可能在无硬件解码时走软件回退，仍然通过。
- 若 `wmv2.wmv` 的 selftest 报错，检查软件解码器与 WMV2 支持；这是必须修复的缺陷，不能跳过。

- [ ] **Step 3: 手动验收清单**

依次执行并勾选：

- [ ] 导入 `mpeg4.mp4`、`h264.mp4`、`h265.mp4`、`h265_10bit.mp4`、`wmv2.wmv`，列表显示正确容器、编码、分辨率、时长。
- [ ] 如果拿到真实 VC-1 样本，放入 `tests/fixtures/manual/vc1.wmv` 并重新运行 `media_tests` 与导入预览。
- [ ] 播放、暂停、逐帧、拖动进度条均正常。
- [ ] `rotated.mp4` 预览方向正确。
- [ ] `中文 文件名.mp4` 可导入和预览。
- [ ] `corrupt.mp4` 显示错误提示，应用不崩溃。
- [ ] `noaudio.mp4` 可导入和预览，无音频不报错。
- [ ] 导入 `highres.mp4` 后生成代理，缓存目录出现文件；切换“使用代理”能切换预览源。
- [ ] 导入素材后撤销、重做，列表同步变化。
- [ ] 保存工程到 `.veproj`，重新打开后素材列表恢复。
- [ ] 编辑后等待 30 秒，关闭应用再启动，出现恢复对话框；恢复后素材列表正确。
- [ ] 任务完成前运行 `git status`，确认没有未提交的构建产物或缓存文件。

- [ ] **Step 4: 写 README**

`README.md`：

```markdown
# VideoEditor

Windows 桌面视频编辑器（M1 媒体基础）。

## 环境

- Windows 10 21H2+ / Windows 11 x64
- Visual Studio 2022（MSVC 19.4x）
- CMake 3.27+、Ninja
- Qt 6.8.3 MSVC 2022 64-bit
- vcpkg（FFmpeg 7.1、GoogleTest）
- 可选：带 libx264/libx265 的 FFmpeg CLI，仅用于生成测试素材和代理

## 构建

```powershell
$env:QTDIR = "C:/Qt/6.8.3/msvc2022_64"
$env:VCPKG_ROOT = "C:/vcpkg"
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

## 运行

```powershell
windeployqt --qmldir src/app build/windows-x64-debug/src/app/VideoEditor.exe
./build/windows-x64-debug/src/app/VideoEditor.exe
```

## 测试素材

```powershell
$env:VIDEOEDITOR_FFMPEG = "ffmpeg"
./tools/generate_fixtures.ps1
```

## M1 已知限制

- 不播放音频；音画同步与音频编辑属于 M2。
- 硬件解码路径只接受 NV12 8-bit；P010/10-bit 自动回退软件解码。
- 时间码为非丢帧时间码；丢帧时间码在 M2 与音频时钟一起加入。
- WMV/VC-1 解码由 FFmpeg 提供；自动测试使用 WMV2 容器样本，VC-1 需用真实样本（`tests/fixtures/manual/vc1.wmv`）验证。
- H.265 10-bit 硬件解码若返回 P010，会自动回退软件解码并转换为 8-bit 预览。
- 代理使用 MJPEG 全帧内编码，存放在本地缓存目录，可删除重建。
```

- [ ] **Step 5: 最终提交**

```bash
git add README.md tests
git commit -m "test: add end-to-end preview selftests and M1 documentation"
git log --oneline -n 12
```

预期：最后一次提交包含 README 与 CTest 扩展；`git status` 干净。

---

## Self-Review

**Spec coverage:**

| 规格 M1 要求 | 对应任务 |
|---|---|
| Qt 6 + C++ 骨架 | Task 1 |
| 时间基、媒体信息、编码判定 | Task 2 |
| MP4 MPEG-4 Part 2 / H.264 / H.265 与 WMV/VC-1 探测 | Task 3 |
| 工程模型、撤销、保存、崩溃恢复 | Task 4 |
| 媒体库、导入、错误提示 | Task 5 |
| D3D11 设备、共享纹理、NV12/RGBA 转换 | Task 6 |
| 硬件解码 + 软件回退 | Task 7 |
| Qt Quick 预览、播放、逐帧、定位、旋转 | Task 8 |
| 代理媒体与缓存 | Task 9 |
| 端到端验收与文档 | Task 10 |

**Review Focus 与测试映射：**

1. 旋转元数据：Task 3 `ReadsRotationMetadata`；Task 7 旋转值进入 `FramePublisher::publish`；Task 8 手动验证。
2. Unicode 路径：Task 3 `HandlesUnicodePath`；Task 7 `HandlesUnicodePath`。
3. VFR：Task 3 `HandlesVfrDuration`；Task 7 `VfrTimestampsAreMonotonic`。
4. 无音频/多视频流/损坏文件：Task 3 `HandlesNoAudioAndMultipleVideoStreams`、`ReportsCorruptAndMissingFiles`。
5. GPU 不可用/4K：Task 7 `FallsBackToSoftwareWhenVideoContextDisabled`；Task 9 `TriggersOnlyAbove1080p` 与 `Generates540pProxy`。

**类型一致性检查：**

- `MediaInfo` 定义在 `src/core/MediaInfo.h`，被 core/media/ui 共同包含。
- `SharedTextureRing::kSlotCount == 3`，`DecoderWorker`、`FrameBridge`、`VideoPreviewItem` 均使用 3 个句柄。
- `FramePublisher::publish(DecodedFrame&, int rotationDegrees, QString*)` 与 `DecoderWorker` 调用一致。
- `PreviewController` 通过 `VideoPreviewItem::setSharedHandles` + `submitFrame` 传递帧，签名一致。
- `ProxyService::request(assetId, sourcePath, height)` 与 `AppController` 调用一致。

**已知实现注意事项（执行时必须遵守）：**

- `FfmpegSoftwareDecoder` 的缩放器缓存必须记录 `width/height/format`，不能访问 `SwsContext` 私有字段。
- `DecoderWorker::open` 必须先解码首帧获取实际尺寸，再创建共享纹理环，避免代理分辨率与源分辨率不一致时画面错位。
- `FfmpegD3D11Decoder::decodeNext` 在把 `impl.frame` 交给 `out.avFrame` 前，先释放 `out.avFrame` 已有的帧；随后检查 `av_frame_alloc()` 是否成功，失败时设置错误并返回 `false`。
- `ProjectDocument::removeAsset` 在 push 命令前先确认 id 存在；不存在时直接返回，避免 `RemoveAssetCommand` 插入空素材。
- `QSGD3D11Texture::fromNative` 只在 Qt 场景图渲染线程调用，且只支持 2D RGBA 纹理；`FrameBridge::present` 只能在 `updatePaintNode` 内调用。
- `VideoPreviewItem` 的 `FrameBridge` 只能在渲染线程重建；GUI 线程通过 `m_resetRequested` 原子标志请求重置，不要直接赋值 `m_bridge`。
- 两个转换器必须绑定 `CullMode = D3D11_CULL_NONE` 的光栅化状态，否则全屏三角形可能被剔除为黑屏。
- 旋转方向以手动验证为准：探测值表示顺时针角度；如果 90° 素材显示方向相反，在 shader 的 `rotateUv()` 中把 rotation 取负，不要修改探测值或代理逻辑。
- 如果 `QSGD3D11Texture` 在当前 Qt 版本的头文件中不可见，包含 `<QNativeInterface>` 与 `<QtQuick/QSGTexture>`；仍不可用时记录为阻塞问题，不要用 CPU 回读静默替代。
