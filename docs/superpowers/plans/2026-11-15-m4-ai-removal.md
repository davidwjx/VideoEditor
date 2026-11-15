# M4 本地 AI 消除（遮罩、跟踪、修复、缓存、返修）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付本地 AI 消除：用户涂抹/框选目标，系统跟踪并传播遮罩，逐帧修复背景，把结果写成内容寻址的生成层缓存；预览可对比原片/修复，支持关键帧修正、局部重做、撤销/重做；导出只读取完整缓存，绝不在导出过程中启动推理。

**Architecture:** 新增 `RemovalLayer`（工程 schema v3）关联 `Clip`，遮罩与跟踪置信度作为工程旁车文件保存，修复结果作为 `removal/<cacheKey>` 缓存。AI 推理运行在独立子进程 `VisionAiEngine.exe`，通过 QLocalSocket + NDJSON 与 UI 进程通信，ONNX Runtime 使用 DirectML/CPU（CUDA 仅作 QA）。`TimelineCompositor` 在绘制原片后叠加带羽化 alpha 的修复补丁，预览与 M3 导出因此共用同一条合成路径；导出前由 `RemovalPreflight` 校验缓存完整性，缺失/过期时拒绝导出。

**Tech Stack:** C++20、Qt 6.8.x（Core/Gui/Quick/QuickControls2/Qml/Network/Test）、FFmpeg 7.1.x LGPL、Direct3D 11、ONNX Runtime 1.22.x（DirectML Execution Provider + CPU EP，CUDA 可选）、QLocalSocket/QLocalServer、GoogleTest、CMake 3.27+、MSVC 2022、Windows x64。

**Spec:** [docs/superpowers/specs/2026-11-14-video-editor-design.md](../specs/2026-11-14-video-editor-design.md)（M4 对应 §3.1、§4.6、§4.8、§5.5、§6.1–§6.3、§7.3、§8、§9.3、§10、§11.1–§11.3、§12）

**Research:** [docs/superpowers/research/2026-11-15-m4-ai-removal-research.md](../research/2026-11-15-m4-ai-removal-research.md)

## Global Constraints

- 仅 Windows 10 21H2+ / Windows 11 x64；MSVC 2022 19.4x；CMake ≥ 3.27；C++20；源码 UTF-8（`/utf-8`）。
- Qt 固定 6.8.x；FFmpeg 固定 7.1.x **LGPL**。应用不得链接 GPL 组件；`ffmpeg.exe` 只用于生成测试素材/代理，不得在 AI 推理或导出时启动。
- 图形后端只能是 D3D11。预览与导出必须共用 `TimelineCompositor`；AI 补丁必须在同一合成函数中绘制，不得新增“仅预览”或“仅导出”的视觉分支。
- AI 推理只在独立子进程 `VisionAiEngine.exe` 中运行；UI 进程、合成线程、导出线程都不得加载模型或分配模型张量。子进程无网络访问、无遥测、不上传帧。
- ONNX Runtime 执行提供者顺序固定为 DirectML → CPU；CUDA EP 仅在 NVIDIA QA 构建启用。DirectML 使用 `ORT_SEQUENTIAL`、`enable_mem_pattern=false`、`enable_cpu_mem_arena=false`、静态形状、`batch=1`、opset ≤ 20。
- 所有推理在本地完成；模型文件必须 SHA-256 校验；模型缺失或哈希不符时功能关闭并给出中文原因，绝不运行未知模型。
- 1080p SDR / BT.709 为性能基线；支持 30/60 fps 工程；单个消除层覆盖的源片段 ≤ 30 秒；一个消除层只处理一个目标。4K/HDR、快速复杂遮挡、扩散生成、多目标联合优化、自动检测、云端、训练/微调均为非目标。
- 时间统一 `int64_t` 微秒；帧率用 `Rational{num, den}`；帧号与源时间换算必须用整数运算，禁止浮点累加。
- 原始素材只读；修复结果是生成层，不修改源文件。删除/禁用生成层立即恢复原片。
- 遮罩与跟踪置信度是**工程数据**，保存在 `<project>.veproj.assets/masks|tracking`，不得放在可清理的缓存根下。修复结果是**缓存**，可重建、可清理；清理缓存不得影响工程正确性。
- 缓存键必须包含：资产指纹、源区间、所有遮罩关键帧（时间/哈希/羽化/revision）、参数（模式/稳定化/处理分辨率）、跟踪与修复模型 id + 模型文件哈希、pipeline 版本、色彩空间、源帧率、输出宽高、cacheKeyVersion。**不得包含** `timelineStartUs`。
- 导出前 `RemovalPreflight` 只允许完整且哈希匹配的缓存；导出过程中缓存丢失必须报错并中止，绝不静默输出原片。导出路径内不得启动任何推理。
- 撤销栈保持 `QUndoStack` 为唯一真相：一次画笔拖动 = 一个命令；跟踪、生成、局部重做、删除/禁用生成层都是可撤销命令；撤销栈至少 100 步。
- 面向用户的字符串用中文；标识符、日志、注释用英文。错误信息必须说明“发生了什么、影响是什么、可以怎么做”。
- 单元测试不得强制依赖 GPU、模型下载或交互式桌面：无 GPU/模型时 `GTEST_SKIP()` 并写明原因。模型相关测试用固定小样本，生成类测试用合成图像。
- 每个任务结束必须 `cmake --build --preset windows-x64-release` 且 `ctest --preset windows-x64-release --output-on-failure` 通过后提交；M1/M2/M3 的 `core_tests`、`media_tests`、`ui_tests`、`render_tests`、`export_tests`、`app_tests`、`TimelineQml`、`ExportQml`、`timeline_selftest`、`export_selftest` 必须保持绿色。
- 所有提交信息末尾附加：`Co-Authored-By: Claude Code <noreply@anthropic.com>`。

## Review Focus

以下五类输入/条件最容易让用户感知到错误，且规格没有逐条展开。每一条都在拥有该代码的任务中有对应测试：

1. **创建消除层后继续裁切/移动/分割片段，或替换素材、改帧率/分辨率**：层只能在其源区间内生效，移出区间应显示原片；缓存键不得因时间线位置变化而失效，但素材/帧率/参数变化必须失效。测试在 Task 1（区间校验）、Task 2（缓存键排除 timelineStart）、Task 7（区间外显示原片）。
2. **缓存被清理、写了一半、文件损坏、工程换机打开**：预览必须回退原片并只提示一次；导出必须在写任何输出前失败；工程文件与遮罩旁车不得被缓存清理破坏。测试在 Task 2（manifest/state/clear）、Task 7（preview fallback + preflight）、Task 9（损坏/磁盘满/中断）。
3. **未保存工程、另存为、工程/素材路径含中文或空格**：遮罩与跟踪旁车必须随工程迁移，缓存键中的指纹与路径无关；保存/另存为后撤销与预览仍正确。测试在 Task 2（sidecar relocate/中文路径）、Task 3（未保存工程编辑）、Task 9（中文路径工程端到端）。
4. **低显存、4–6 GB 显卡、驱动重置、无 GPU/CPU-only**：必须按回退阶梯降规格或暂停，保留已完成分段，给出中文原因；不得崩溃、不得把 CPU 回退说成 GPU 加速。测试在 Task 4（gate/fallback 事件）、Task 5（DML/CPU parity + skip）、Task 6（VRAM 预算）、Task 8（错误文案与恢复）。
5. **退化遮罩：贴边、零面积、羽化大于目标、关键帧乱序/重复、播放头在层区间外、框选反向拖拽**：不得产生无效缓存或崩溃；空遮罩应提示重画；羽化只改参数不改遮罩哈希。测试在 Task 2（RLE/哈希/参数）、Task 3（画笔/框选/关键帧）、Task 6（补丁边界与羽化）。

---

## Design Decisions

1. **许可证门禁 fail-closed。** 只有 Memo §3.4 的安全短名单可以进入实现：跟踪 SAM 2.1 Hiera-Tiny/Small、EfficientTAM-Ti 512（MobileSAM 仅最后兜底）；修复 MI-GAN 512、LaMa 512。代码/权重是 Apache-2.0/MIT，但训练数据 SA-1B、Places2、YouTube-VOS 存在商业使用/再分发风险。Task 0 必须记录法务结论；`models.manifest.json` 的 `commercialApproved` 不为 `true` 时，`AiFeatureGate` 关闭功能，入口显示中文原因，安装包不附带模型。若法务 NO_GO，Task 5–9 不启动。
2. **Schema v3 + 旁车。** `Project::removalLayers` 保存生成层元数据；`Clip::removalLayerId` 建立关联。遮罩 RLE 与跟踪置信度 JSON 放在工程旁车目录，工程 JSON 只存 64 位十六进制哈希。`Project::kSchemaVersion = 3`，v1/v2 迁移为空列表，v4+ 拒绝。
3. **一个片段一个消除层。** 规格 §7.2 的 `Clip.removalLayerId` 是单值，因此 M4 一个片段只能挂一个目标；多目标通过分割/复制片段并分别建层实现，不做联合优化。层可以禁用、删除、局部重做；删除即恢复原片。
4. **遮罩编码用二进制 RLE。** `BinaryMask` 每像素 1 字节（0/255），`MaskCodec` 输出带 magic/宽高/交替 run-length 的确定性字节串；哈希就是字节串的 SHA-256。羽化是 `MaskKeyframe::featherPx` 参数，不烘焙进遮罩，改羽化不触发重画。
5. **缓存格式：逐帧 RGBA PNG 补丁 + chunk index + manifest。** 选择 PNG 而不是 FFV1/MKV，因为：(a) 需要 alpha 做羽化叠加；(b) 随机访问预览简单；(c) `QImage`/`LayerCompositor::drawImage` 已存在，测试无需 GPU 解码器；(d) 补丁只覆盖遮罩外扩区域，体积可控。chunk 默认 30 帧，index 记录每帧 crop 与 SHA-256；manifest 记录 chunk 树哈希、模型 id、fps、状态。写入用临时目录 + 原子 rename；manifest 最后写。
6. **AI 子进程协议。** 父进程把冻结的 `AiJobSpec` 写成 JSON 文件并启动 `VisionAiEngine.exe --job <file> --socket <name>`；子进程连接 `QLocalServer`，双方交换 NDJSON。`AiTransport` 抽象让测试用 `FakeTransport` 完全不需要 GPU/模型；`LocalAiTransport` 负责进程生命周期、5 秒取消宽限、崩溃检测。
7. **跟踪选型。** 默认 SAM 2.1 Hiera-Tiny 拆分 ONNX（image_encoder / prompt_encoder / mask_decoder / memory_attention / memory_encoder）；若 Task 0 测得 DirectML 不可用或 J&F 不达标，改用 EfficientTAM-Ti 512 的 5-session ONNX bundle。跟踪接口模型无关；置信度用 predicted-IoU、遮挡分（若模型输出）和面积稳定性分级；低置信连续 3 帧即停止传播并标记 `LowConfidence`，不静默漂移。
8. **修复组合。** MI-GAN 512 为快速路径（小/中遮罩、水印、台标），LaMa 512 为质量路径（大洞、纹理背景）。逐帧修复后加项目自有时域稳定化：按目标 bbox 平移对齐相邻补丁，做 5 帧中值 + 强度混合；再做边缘颜色匹配与 alpha 羽化。诚实标注“移动较慢目标”的限制，不承诺 ProPainter 级质量。
9. **合成方式：原片 + alpha 补丁。** `TimelineCompositor` 先按现有逻辑画原片，再用 `RepairFrameSource` 取补丁，按 clip transform 的内嵌矩形以 alpha 叠加。补丁的 alpha 在遮罩内为 1、羽化带渐变、遮罩外为 0。对比模式 `Repair`/`Original`/`SplitWipe` 只在预览生效；导出固定 `Repair`，且缺失缓存时 `failOnMissingRemoval` 直接报错。
10. **UX 渐进披露。** 默认流程只有“AI 消除 → 涂抹/框选 → 羽化 → 跟踪 → 生成修复 → 对比”；模型、处理分辨率、稳定化强度、chunk 大小放在“高级”。置信度条、缓存状态、低置信跳转始终可见；错误用内联中文提示，只有导出前缺缓存这种必须决策的场景用按钮选择。

## Starting Point

M1/M2 在 `main`（`3177c11`），M3 导出在 `feature/m3-export`（只读上下文；M4 分支从 M3 合并后的 `main` 切出）。本计划假设以下既有接口，**不得重命名**：

- `Project { QString name; TimelineSettings timeline; QList<Asset> assets; QList<Track> tracks; QList<SubtitleStyle> subtitleStyles; }`、`Project::assetById`、`Project::kSchemaVersion`（当前 2）、`ProjectSerializer::toJson/fromJson/save/load`。
- `Timeline.h`：`Clip { id, assetId, linkId, sourceInUs, sourceOutUs, timelineStartUs, speedMilli, preservePitch, enabled, transform, audio }`、`Track`、`Transform`、`Rational`。
- `TimelineEvaluator(project).frameAt(us)` → `FrameComposition { timelineUs, QList<VideoLayer> video, QList<SubtitleLayer> subtitles }`；`VideoLayer { trackId, clipId, assetId, trackIndex, localTimeUs, sourceTimeUs, transform }`；`sourceTimeAt(clip, timelineUs)`。
- `TimelineCompositor::open(project, width, height, useProxy)`、`drawFrame(timelineUs, target, clearBlack, error)`、`setWarningCallback`；`ClipVideoSource::open(asset, context, proxyPath, converters)`、`frameAt(sourceUs)`、`width/height/texture`；`activeProxyPath(asset, useProxy)`；`LayerCompositor::{begin, drawFrame, drawImage, drawImageCached, end, forgetTexture}`；`LayerRect`/`layerRectFor`。
- `TimelineRenderWorker::setProject(project, projectPath, generation, useProxy)`、`renderFrame`、`frameReady`、`error`；`PreviewController::{openTimeline, updateTimelineProject, setUseProxy, timelineProject}`。
- `OfflineFrameRenderer::open(project, width, height)`、`renderFrame(timelineUs, rgba, error)`、`setWarningCallback`；M3 `ExportPipeline::run(project, settings, catalog, resume, hooks)`、`ExportController::{validate, startExport, resumeExport, running, canResume}`。
- `CacheStore(QString root)`、`CacheStore::proxyPath/sizeOnDisk/clear`；`fingerprintFile(path)`（size+mtime+head+tail SHA-256）；`ProjectDocument` 的 `QUndoStack`、`applyTrackEdit(s)`、`setProject`、`save/load`。
- `ProjectStore`（工程保存/恢复）；`QSaveFile` 原子写；`TimeBase.h` 的 `framesToMicros/microsToFrames`。
- 构建：`cmake --preset windows-x64-release`、`cmake --build --preset windows-x64-release`、`ctest --preset windows-x64-release --output-on-failure`；测试目标 `core_tests`、`media_tests`、`render_tests`、`export_tests`、`ui_tests`、`app_tests`、`app_qml_tests`。

## File Structure

| 责任 | 新文件 / 修改 |
|---|---|
| 数据模型、序列化、撤销 | `src/core/Removal.h/.cpp`、`src/core/Project.h/.cpp`、`src/core/Timeline.h`、`src/core/ProjectSerializer.cpp`、`src/core/commands/RemovalLayerCommand.h/.cpp`、`src/core/ProjectDocument.h/.cpp` |
| 遮罩、旁车、缓存键 | `src/core/MaskCodec.h/.cpp`、`src/core/MaskEditor.h/.cpp`、`src/core/MaskSidecarStore.h/.cpp`、`src/core/RemovalCacheKey.h/.cpp` |
| 修复缓存 | `src/media/cache/RemovalCache.h/.cpp`、`src/media/cache/CacheStore.h/.cpp` |
| AI 子进程与协议 | `src/ai/AiProtocol.h/.cpp`、`src/ai/AiTransport.h`、`src/ai/LocalAiTransport.h/.cpp`、`src/ai/AiJobManager.h/.cpp`、`src/ai/ModelRegistry.h/.cpp`、`src/ai/AiFeatureGate.h/.cpp`、`src/ai/AiEngineMain.cpp`、`src/ai/CMakeLists.txt` |
| ONNX 推理 | `src/ai/engine/OnnxExecutor.h/.cpp`、`Confidence.h/.cpp`、`Sam2Tracker.h/.cpp`、`TrackingPipeline.h/.cpp`、`MiGanInpainter.h/.cpp`、`LamaInpainter.h/.cpp`、`InpaintPipeline.h/.cpp`、`TemporalStabilizer.h/.cpp`、`FrameReader.h/.cpp` |
| 合成与导出 | `src/media/render/RepairFrameSource.h/.cpp`、`src/media/render/TimelineCompositor.h/.cpp`、`LayerCompositor.h/.cpp`、`TimelineEvaluator.h/.cpp`、`TimelineRenderWorker.h/.cpp`、`src/ai/RemovalPreflight.h/.cpp`、`src/ui/ExportController.h/.cpp`、`src/media/export/ExportPipeline.h/.cpp` |
| UI | `src/ui/RemovalController.h/.cpp`、`src/ui/MaskImageProvider.h/.cpp`、`src/ui/PreviewController.h/.cpp`、`src/ui/VideoPreviewItem.h/.cpp`、`src/app/RemovalPanel.qml`、`MaskEditorOverlay.qml`、`ConfidenceBand.qml`、`JobProgressPanel.qml`、`InspectorPanel.qml`、`Main.qml`、`TimelineClipDelegate.qml`、`ExportDialog.qml`、`src/app/AppController.h/.cpp`、`main.cpp` |
| 模型与门禁 | `assets/models/models.manifest.json`、`docs/superpowers/research/2026-11-15-m4-license-decision.md`、`docs/superpowers/research/2026-11-15-m4-dml-spike.md`、`tools/m4_spike/dml_probe.py` |
| 测试 | `tests/RemovalTestProject.h`、`RemovalSerializerTest.cpp`、`RemovalCommandTest.cpp`、`MaskCodecTest.cpp`、`MaskSidecarStoreTest.cpp`、`RemovalCacheKeyTest.cpp`；`tests/media/RemovalCacheTest.cpp`、`RepairSourceTest.cpp`、`RemovalRenderTest.cpp`、`RemovalExportTest.cpp`；`tests/ai/RemovalPreflightTest.cpp`、`tests/ai/ConfidenceTest.cpp`、`TrackingPipelineTest.cpp`、`InpaintPipelineTest.cpp`、`TemporalStabilizerTest.cpp`、`RemovalEvalTest.cpp`、`RemovalEvalMetrics.h`、`RemovalRobustnessTest.cpp`；`tests/ui/RemovalControllerTest.cpp`、`RemovalControllerProgressTest.cpp`；`tests/app/AiJobManagerTest.cpp`、`RemovalQmlTest.cpp`；`tests/fixtures/removal/generate_fixtures.ps1` |

## Dependency Graph

```
Task 0 (license + DirectML gate)
  └─ Task 1 (schema v3 + undo)
       └─ Task 2 (mask/cache key/manifest)
            ├─ Task 3 (mask editor UI)
            └─ Task 4 (AI subprocess/protocol/gate)
                 ├─ Task 5 (tracking)   [model-dependent]
                 └─ Task 6 (inpainting) [model-dependent]
                      └─ Task 7 (repair source + compositor + export preflight)
                           └─ Task 8 (UX polish: confidence/progress/partial redo)
                                └─ Task 9 (evaluation/robustness/acceptance)
```

Task 0 是硬门禁：技术选型与法务结论必须记录在案。Task 1–4 可在 Task 0 的技术选型记录完成后并行推进，但功能默认关闭、不得附带模型；Task 5–9 必须等 Task 0 的 GO + 法务 `commercialApproved=true` 才能开始。若 Task 0 判定 NO_GO，本计划在 Task 0 终止。

---

### Task 0: 许可证与 DirectML 技术门禁（硬门禁，不写产品代码）

**Files:**
- Create: `docs/superpowers/research/2026-11-15-m4-license-decision.md`
- Create: `docs/superpowers/research/2026-11-15-m4-dml-spike.md`
- Create: `assets/models/models.manifest.json`
- Create: `tools/m4_spike/dml_probe.py`
- No `src/` changes.

**Interfaces:**
- Consumes: Memo §2–§4、§8、附录 C；规格 §12 选型门禁。
- Produces: `models.manifest.json`（Task 4 的 `ModelRegistry` 读取）；决策记录（Task 5–9 的前置证据）；性能/质量阈值（Task 5/6/9 使用）。

- [ ] **Step 1: 建立三许可证核对表**

在 `2026-11-15-m4-license-decision.md` 写以下表格，每一行必须填“代码许可证 / 权重许可证 / 训练数据 / 商业再分发结论 / 证据链接”。短名单固定为：

| 角色 | 模型 | 代码/权重 | 训练数据（必须列明） | 结论 |
|---|---|---|---|---|
| 跟踪主选 | SAM 2.1 Hiera-Tiny（可升级 Small） | Apache-2.0 / Apache-2.0 | SA-1B、YouTube-VOS、DAVIS、MOSE、内部数据 | 待法务 |
| 跟踪兜底 | EfficientTAM-Ti 512 | Apache-2.0 / Apache-2.0 | SA-V / SA-1B 相关 | 待法务 |
| 最后兜底 | MobileSAM | Apache-2.0 / Apache-2.0 | 由 SAM 蒸馏（SA-1B） | 待法务 |
| 修复快速 | MI-GAN 512 | MIT / MIT（LICENSE-WEIGHTS） | Places2、FFHQ | 待法务 |
| 修复质量 | LaMa 512 | Apache-2.0 / Apache-2.0 | Places2 | 待法务 |
| 运行时 | ONNX Runtime + DirectML | MIT / Microsoft 可再分发条款 | 不适用 | 需附 ThirdPartyNotices |

明确排除并写入“不得采用”：ProPainter（NTU S-Lab 非商用）、E2FGVI（CC BY-NC）、MAT（研究）、DiffuEraser（依赖 ProPainter + SD1.5）、MiniMax-Remover（CC-BY-NC）、Netflix VOID（5B/40GB，超出 M4）、XMem（GPL-3.0）、SAM 3（自定义 Meta 许可）、COCOCO/ZITS/Cutie 研究项。

- [ ] **Step 2: 向法务提交 8 个必须回答的问题**

把 Memo §3.5 的 8 个问题逐条写入决策文档，并要求法务对每条给出“是/否/有条件”：

1. 在目标司法辖区，Apache-2.0/MIT 权重许可是否覆盖 SA-1B/Places2/YouTube-VOS 的训练数据限制？
2. Places2 是否存在商业许可路径；若无，重训/微调的成本与周期？
3. SAM 2 权重是否被视为 SA-1B/YouTube-VOS 的衍生作品；YouTube-VOS 的 ShareAlike 是否传播到 ONNX 转换权重？
4. Apache-2.0 NOTICE、修改文件标记（ONNX 导出属于修改/衍生）、MIT 版权声明、About/Licenses 对话框的具体义务？
5. ONNX Runtime 的 ThirdPartyNotices 与 DirectML 的再分发条款如何随安装包分发？
6. 是否可以在 UI 中 factual 地使用模型名而不暗示 Meta/Picsart 背书？
7. 若质量不可妥协，ProPainter 商业许可的报价/周期；另立 12–16 GB VRAM 的“AI Pro”轨道是否可行？
8. 选定模型/ORT/DirectML 是否有已知专利负担？

- [ ] **Step 3: 下载候选模型并按 SHA-256 记录**

在独立空目录执行（不要把下载物放进仓库；只把哈希与路径写进 manifest）：

```powershell
# 示例：实际 URL 以 Memo §12 为准；每个模型单独目录，禁止在下载目录内执行脚本。
$root = "C:\m4-spike\models"
New-Item -ItemType Directory -Force $root | Out-Null
# SAM 2.1 Tiny 拆分 ONNX（社区 bundle / AMD PAVS）
Invoke-WebRequest -Uri "<sam2-tiny-split-onnx-url>" -OutFile "$root\sam2_tiny_split.zip"
# EfficientTAM-Ti 512 5-session ONNX
Invoke-WebRequest -Uri "https://huggingface.co/egordm/efficienttam-ti-512/resolve/main/<bundle>.zip" -OutFile "$root\efficienttam_ti.zip"
# MI-GAN 512 ONNX
Invoke-WebRequest -Uri "https://github.com/Picsart-AI-Research/MI-GAN/releases/download/<tag>/migan_pipeline_v2.onnx" -OutFile "$root\migan_pipeline_v2.onnx"
# LaMa 512 ONNX fp16
Invoke-WebRequest -Uri "https://huggingface.co/g-ronimo/lama/resolve/main/<lama>.onnx" -OutFile "$root\lama.onnx"
Get-ChildItem $root -Recurse -File | ForEach-Object {
  $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
  "$h  $($_.FullName)"
}
```

把每个 `id/family/version/files{path,sha256}/codeLicense/weightsLicense/trainingData/io` 写入 `assets/models/models.manifest.json`。初始值必须是 **fail-closed**：

```json
{
  "schemaVersion": 1,
  "pipelineVersion": "m4.0",
  "commercialApproved": false,
  "legalSignOff": { "approvedBy": "", "date": "", "scope": "" },
  "models": [
    {
      "id": "sam2.1-hiera-tiny",
      "family": "tracking",
      "version": "1.0.0",
      "codeLicense": "Apache-2.0",
      "weightsLicense": "Apache-2.0",
      "trainingData": ["SA-1B", "YouTube-VOS", "DAVIS", "MOSE", "internal"],
      "commercialApproved": false,
      "files": {
        "image_encoder": { "path": "sam2_tiny/image_encoder.onnx", "sha256": "<64-hex>" },
        "prompt_encoder": { "path": "sam2_tiny/prompt_encoder.onnx", "sha256": "<64-hex>" },
        "mask_decoder": { "path": "sam2_tiny/mask_decoder.onnx", "sha256": "<64-hex>" },
        "memory_attention": { "path": "sam2_tiny/memory_attention.onnx", "sha256": "<64-hex>" },
        "memory_encoder": { "path": "sam2_tiny/memory_encoder.onnx", "sha256": "<64-hex>" }
      },
      "io": {
        "imageInput": "pixel_values",
        "imageEmbedding": "image_embeddings",
        "maskLogits": "low_res_masks",
        "iouScore": "iou_scores",
        "objectScore": ""
      }
    }
  ]
}
```

`commercialApproved` 只有在法务签字后才可改为 `true`，且必须同时填 `legalSignOff`。模型文件本身不进入 git；构建/打包脚本从 `VIDEOEDITOR_M4_MODELS_DIR` 复制。

- [ ] **Step 4: 写 DirectML 探测脚本**

`tools/m4_spike/dml_probe.py`（在独立 venv 中运行；只读模型，不联网）：

```python
import argparse, time
import numpy as np
import onnxruntime as ort

def make_session(path, provider, device=0):
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    so.enable_mem_pattern = False
    so.enable_cpu_mem_arena = False
    providers = ([("DmlExecutionProvider", {"device_id": device})]
                 if provider == "dml" else ["CPUExecutionProvider"])
    return ort.InferenceSession(path, so, providers=providers)

def feeds_for(session):
    feeds = {}
    for inp in session.get_inputs():
        dims = [d if isinstance(d, int) else 1 for d in inp.shape]
        dtype = np.float32 if inp.type == "tensor(float)" else np.int64
        feeds[inp.name] = np.zeros(dims, dtype=dtype)
    return feeds

def run(session, feeds, runs):
    times = []
    out = None
    for _ in range(runs):
        t0 = time.perf_counter()
        out = session.run(None, feeds)
        times.append((time.perf_counter() - t0) * 1000.0)
    return out, times

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", required=True)
    p.add_argument("--runs", type=int, default=10)
    p.add_argument("--device", type=int, default=0)
    args = p.parse_args()

    cpu = make_session(args.model, "cpu")
    cpu_out, cpu_t = run(cpu, feeds_for(cpu), args.runs)
    print(f"CPU provider={cpu.get_providers()} p50={np.median(cpu_t):.2f}ms")

    dml = make_session(args.model, "dml", args.device)
    dml_out, dml_t = run(dml, feeds_for(dml), args.runs)
    print(f"DML provider={dml.get_providers()} p50={np.median(dml_t):.2f}ms")

    worst = 0.0
    for a, b in zip(cpu_out, dml_out):
        a = np.asarray(a, dtype=np.float32)
        b = np.asarray(b, dtype=np.float32)
        denom = max(1e-6, float(np.max(np.abs(a))))
        worst = max(worst, float(np.max(np.abs(a - b))) / denom)
    print(f"max_relative_output_diff={worst:.6f}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 5: 运行技术探测并记录数字**

在 `2026-11-15-m4-dml-spike.md` 记录以下表格的实测值（每格必须有数字或“失败 + HRESULT/错误”）；至少测参考 AMD GPU 与一台 NVIDIA QA 机器：

| 项目 | 通过阈值 | 实测 |
|---|---|---|
| SAM2 5 个 ONNX session 在 DML 创建 | 全部成功 | |
| SAM2 每部件 DML vs CPU 相对输出差（`dml_probe.py`） | ≤ 1e-3 | |
| SAM2 Tiny 1024² 跟踪速度（Task 5 用完整 pipeline 复测） | ≥ 5 fps | |
| SAM2 Tiny 20 帧 DML/CPU mask IoU（Task 5 用固定帧复测） | ≥ 0.98 | |
| EfficientTAM-Ti 5 个 ONNX session DML | 全部成功 | |
| EfficientTAM 512² 跟踪速度 | ≥ 8 fps | |
| MI-GAN 512 单 tile | ≥ 10 fps | |
| LaMa 512 单 tile | ≥ 2 fps | |
| 显存峰值（SAM2 + MI-GAN） | ≤ 4 GB（6 GB 卡） | |
| CPU 回退可用 | MI-GAN 可完成 1 帧 | |

若 SAM2 全部部件不能创建或 parity 失败，但 EfficientTAM 通过，则跟踪默认改为 EfficientTAM；若两者都失败，Task 5/6 不得进入实现，记录 NO_GO。若 MI-GAN/LaMa 的 ONNX 任一不能在 DML 或 CPU 运行，Task 6 不得进入实现。

- [ ] **Step 6: 写 GO/NO_GO 结论与阈值**

在决策文档末尾写以下三选一，并签名/日期：

```text
DECISION: GO_SHORTLIST
COMMERCIAL_APPROVED: true | false
LEGAL_SCOPE: <司法辖区 + 分发方式 + 是否允许 ONNX 转换权重>
TRACKING_DEFAULT: sam2.1-hiera-tiny | efficienttam-ti-512
INPAINT_FAST: migan-512; INPAINT_QUALITY: lama-512
QUALITY_THRESHOLDS: tracking_jf>=0.75; mask_parity_iou>=0.98;
  static_flicker<=2/255; moving_subjective>=3.5/5; psnr_hole>=28dB
PERF_THRESHOLDS: 1080p30 10s <=10min (RTX3060-class / reference AMD);
  tracking>=5fps; migan>=10fps; lama>=2fps; preview>=24fps; vram<=4GB
```

`COMMERCIAL_APPROVED: false` 时只能得到“技术可行但不可发布”的结论；`models.manifest.json` 保持 `commercialApproved=false`，Task 4 的 gate 会关闭功能。若法务结论是 NO_GO，停止计划并回到产品/法务，不得用“权重是 Apache-2.0”替代训练数据结论。

- [ ] **Step 7: 门禁验收**

- [ ] 决策文档包含三许可证表、8 个法务问题、签名/日期、GO/NO_GO。
- [ ] `models.manifest.json` 通过 JSON 解析；每个模型有 ≥1 个文件哈希；`commercialApproved` 与法务结论一致。
- [ ] 探测报告包含上表全部实测值；失败项写清 HRESULT/异常。
- [ ] 若 NO_GO：不修改 `src/`，不下载模型进仓库，Task 1–9 不启动。
- [ ] 提交：`git add docs/superpowers/research assets/models/models.manifest.json tools/m4_spike && git commit -m "docs(m4): record license and DirectML gate"`。

---

### Task 1: Project schema v3、RemovalLayer 与撤销命令

**Files:**
- Create: `src/core/Removal.h`
- Create: `src/core/Removal.cpp`
- Modify: `src/core/Timeline.h`（`Clip` 增加 `removalLayerId`）
- Modify: `src/core/Project.h`（`removalLayers`、`kSchemaVersion=3`、`removalLayerById`）
- Modify: `src/core/Project.cpp`
- Modify: `src/core/ProjectSerializer.cpp`
- Create: `src/core/commands/RemovalLayerCommand.h`
- Create: `src/core/commands/RemovalLayerCommand.cpp`
- Modify: `src/core/ProjectDocument.h`
- Modify: `src/core/ProjectDocument.cpp`
- Create: `tests/RemovalTestProject.h`
- Create: `tests/core/RemovalSerializerTest.cpp`
- Create: `tests/core/RemovalCommandTest.cpp`
- Modify: `src/core/CMakeLists.txt`、`tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Project`、`Clip`、`Track`、`ProjectDocument`、`QUndoStack`、`QJsonObject`。
- Produces:
  - `enum class RemovalTrackingState { NotTracked, Tracking, Partial, Complete, LowConfidence, Failed }` + `removalTrackingStateToString/FromString`。
  - `enum class RepairMode { Fast, Quality }` + `repairModeToString/FromString`。
  - `struct MaskKeyframe { QString id; int64_t sourceTimeUs; QString maskHash; int featherPx; int revision; }`。
  - `struct RemovalParameters { RepairMode mode; bool temporalStabilization; int stabilizationStrength; int processingWidth; int processingHeight; }`。
  - `struct RemovalLayer { QString id; QString clipId; int64_t sourceInUs; int64_t sourceOutUs; QList<MaskKeyframe> maskKeyframes; RemovalTrackingState trackingState; QString trackingConfidenceHash; RemovalParameters parameters; QString cacheKey; QString cachePath; int revision; bool enabled; }`。
  - `Project::removalLayers`、`Project::removalLayerById(const QString&)`（const 与非 const）。
  - `QSize displaySizeForAsset(const Asset &asset)`（应用 90/270 旋转后的显示宽高；遮罩/缓存分辨率与预览/导出一致）。
  - `struct RemovalLayerEdit { QString clipId; std::optional<RemovalLayer> before; std::optional<RemovalLayer> after; }`。
  - `ProjectDocument::applyRemovalLayerEdit(const RemovalLayerEdit&, const QString &text)`、`insertRemovalLayerInternal`、`removeRemovalLayerInternal`、`setClipRemovalLayerInternal`、`signal removalLayersChanged()`。

- [ ] **Step 1: 写失败的 schema/序列化测试**

`tests/RemovalTestProject.h`（Task 1/2/7 共用；在 `tests/CMakeLists.txt` 为 `core_tests`、`media_tests`、`render_tests`、`ui_tests` 增加 `target_include_directories(... PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})`，使 `#include "RemovalTestProject.h"` 可用）：

```cpp
#pragma once
#include "Project.h"
#include "Removal.h"

namespace VideoEditor {
inline Project projectWithClip()
{
    MediaInfo info;
    info.path = QStringLiteral("C:/视频/示例.mp4");
    info.durationUs = 10'000'000;
    info.width = 1920;
    info.height = 1080;
    info.supported = true;
    Project p;
    p.assets.append(Asset{QStringLiteral("asset-1"), info.path, info, {}});
    Track track;
    track.id = QStringLiteral("track-v");
    track.type = TrackType::Video;
    Clip clip;
    clip.id = QStringLiteral("clip-1");
    clip.assetId = QStringLiteral("asset-1");
    clip.sourceInUs = 0;
    clip.sourceOutUs = 10'000'000;
    track.clips.append(clip);
    p.tracks.append(track);
    p.ensureSubtitleStyles();
    return p;
}

inline RemovalLayer sampleLayer()
{
    RemovalLayer layer;
    layer.id = QStringLiteral("rl-1");
    layer.clipId = QStringLiteral("clip-1");
    layer.sourceInUs = 0;
    layer.sourceOutUs = 10'000'000;
    layer.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('a')), 4, 1});
    layer.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-2"), 5'000'000, QString(64, QLatin1Char('b')), 8, 1});
    layer.trackingState = RemovalTrackingState::Complete;
    layer.trackingConfidenceHash = QString(64, QLatin1Char('c'));
    layer.parameters.mode = RepairMode::Fast;
    layer.parameters.temporalStabilization = true;
    layer.parameters.stabilizationStrength = 50;
    layer.parameters.processingWidth = 1920;
    layer.parameters.processingHeight = 1080;
    layer.cacheKey = QString(64, QLatin1Char('d'));
    layer.cachePath = QStringLiteral("removal/") + layer.cacheKey;
    return layer;
}
} // namespace VideoEditor
```

`tests/core/RemovalSerializerTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QJsonObject>
#include "ProjectSerializer.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalSerializer, RoundTripsSchemaV3)
{
    Project original = projectWithClip();
    original.removalLayers.append(sampleLayer());
    original.tracks[0].clips[0].removalLayerId = QStringLiteral("rl-1");

    const QJsonObject json = ProjectSerializer::toJson(original);
    EXPECT_EQ(json.value(QStringLiteral("schemaVersion")).toInt(), 3);
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(*restored, original);
}

TEST(RemovalSerializer, MigratesV2WithEmptyRemovalLayers)
{
    Project v2 = projectWithClip();
    QJsonObject json = ProjectSerializer::toJson(v2);
    json.insert(QStringLiteral("schemaVersion"), 2);
    json.remove(QStringLiteral("removalLayers"));
    QString error;
    const auto restored = ProjectSerializer::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_TRUE(restored->removalLayers.isEmpty());
}

TEST(RemovalSerializer, RejectsDuplicateLayerIds)
{
    Project p = projectWithClip();
    p.removalLayers.append(sampleLayer());
    p.removalLayers.append(sampleLayer());
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("重复")));
}

TEST(RemovalSerializer, RejectsUnsortedMaskKeyframes)
{
    Project p = projectWithClip();
    RemovalLayer layer = sampleLayer();
    layer.maskKeyframes[0].sourceTimeUs = 6'000'000;
    p.removalLayers.append(layer);
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("排序")));
}

TEST(RemovalSerializer, RejectsLayerBeyondAssetDuration)
{
    Project p = projectWithClip();
    RemovalLayer layer = sampleLayer();
    layer.sourceOutUs = 20'000'000; // asset duration is 10s
    p.removalLayers.append(layer);
    QString error;
    EXPECT_FALSE(ProjectSerializer::fromJson(
                     ProjectSerializer::toJson(p), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("超出素材时长")));
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build --preset windows-x64-release --target core_tests && ctest --preset windows-x64-release -R RemovalSerializer --output-on-failure`
Expected: 编译失败（`RemovalLayer`/`removalLayers` 未定义）。

- [ ] **Step 3: 新增 `Removal.h` 与 `Removal.cpp`**

```cpp
// src/core/Removal.h
#pragma once

#include <cstdint>
#include <QList>
#include <QSize>
#include <QString>

namespace VideoEditor {

struct Asset;

enum class RemovalTrackingState {
    NotTracked, Tracking, Partial, Complete, LowConfidence, Failed
};
QString removalTrackingStateToString(RemovalTrackingState state);
RemovalTrackingState removalTrackingStateFromString(const QString &text,
                                                    bool *ok = nullptr);

enum class RepairMode { Fast, Quality };
QString repairModeToString(RepairMode mode);
RepairMode repairModeFromString(const QString &text, bool *ok = nullptr);

struct MaskKeyframe {
    QString id;
    int64_t sourceTimeUs = 0;   // source time, not timeline time
    QString maskHash;           // 64 lowercase hex, RLE sidecar hash
    int featherPx = 4;          // 0..64, not baked into maskHash
    int revision = 1;
    bool operator==(const MaskKeyframe &) const = default;
};

struct RemovalParameters {
    RepairMode mode = RepairMode::Fast;
    bool temporalStabilization = true;
    int stabilizationStrength = 50; // 0..100
    int processingWidth = 1920;
    int processingHeight = 1080;
    bool operator==(const RemovalParameters &) const = default;
};

struct RemovalLayer {
    QString id;
    QString clipId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    QList<MaskKeyframe> maskKeyframes;
    RemovalTrackingState trackingState = RemovalTrackingState::NotTracked;
    QString trackingConfidenceHash; // 64 hex, tracking sidecar hash
    RemovalParameters parameters;
    QString cacheKey;   // 64 hex or empty
    QString cachePath;  // "removal/<cacheKey>" or empty
    int revision = 1;
    bool enabled = true;
    bool operator==(const RemovalLayer &) const = default;
};

} // namespace VideoEditor
```

`Removal.cpp` 用 `QStringLiteral` 精确映射：

```cpp
QString removalTrackingStateToString(RemovalTrackingState s)
{
    switch (s) {
    case RemovalTrackingState::NotTracked: return QStringLiteral("notTracked");
    case RemovalTrackingState::Tracking: return QStringLiteral("tracking");
    case RemovalTrackingState::Partial: return QStringLiteral("partial");
    case RemovalTrackingState::Complete: return QStringLiteral("complete");
    case RemovalTrackingState::LowConfidence: return QStringLiteral("lowConfidence");
    case RemovalTrackingState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}
```

`removalTrackingStateFromString` 对未知值返回 `Failed` 并置 `*ok=false`；`repairModeToString` 返回 `"fast"`/`"quality"`，默认 `Fast`。

- [ ] **Step 4: 修改 `Timeline.h`、`Project.h/.cpp`**

`Clip` 增加：

```cpp
QString removalLayerId; // empty = no AI removal generation layer
```

`Project.h`：

```cpp
#include "Removal.h"
...
static constexpr int kSchemaVersion = 3;
QList<RemovalLayer> removalLayers;
const RemovalLayer *removalLayerById(const QString &id) const;
RemovalLayer *removalLayerById(const QString &id);
```

`Removal.cpp` 包含 `Project.h` 以取得 `Asset`/`MediaInfo`；`displaySizeForAsset` 放在 `Removal.cpp`：`MediaInfo::width/height` 任一 ≤0 时返回 `{1920,1080}`；`rotationDegrees` 为 90/270 时交换宽高；其余原样返回。遮罩、缓存 manifest 与 `RepairFrameSource` 都用这个尺寸，避免旋转素材的补丁错位。

`Project.cpp` 实现线性查找（层数少，不做 map）：

```cpp
const RemovalLayer *Project::removalLayerById(const QString &id) const
{
    for (const RemovalLayer &layer : removalLayers)
        if (layer.id == id)
            return &layer;
    return nullptr;
}

RemovalLayer *Project::removalLayerById(const QString &id)
{
    for (RemovalLayer &layer : removalLayers)
        if (layer.id == id)
            return &layer;
    return nullptr;
}
```

- [ ] **Step 5: 序列化、迁移与校验**

在 `ProjectSerializer.cpp` 增加 `removalLayerToJson/removalLayerFromJson`，字段名固定为：
`id, clipId, sourceInUs, sourceOutUs, maskKeyframes[{id,sourceTimeUs,maskHash,featherPx,revision}], trackingState, trackingConfidenceHash, parameters{mode,temporalStabilization,stabilizationStrength,processingWidth,processingHeight}, cacheKey, cachePath, revision, enabled`。

`clipToJson` 增加 `{"removalLayerId", clip.removalLayerId}`；`clipFromJson` 读取。

`fromJson` 的迁移条件：

```cpp
if (version < 3) {
    // v1/v2 have no removalLayers; the field stays empty.
}
for (const QJsonValue &value : json.value(QStringLiteral("removalLayers")).toArray())
    project.removalLayers.append(removalLayerFromJson(value.toObject()));
```

`validateProject` 增加（`ProjectSerializer.cpp` 需新增 `#include <QRegularExpression>`；错误文案必须逐字一致）：

```cpp
QSet<QString> layerIds;
QSet<QString> attachedClips;
for (const RemovalLayer &layer : project.removalLayers) {
    if (layer.id.isEmpty() || layerIds.contains(layer.id))
        return QStringLiteral("AI 消除层包含无效或重复的 id。");
    layerIds.insert(layer.id);

    const Clip *clip = nullptr;
    for (const Track &track : project.tracks) {
        if (track.type != TrackType::Video)
            continue;
        for (const Clip &candidate : track.clips) {
            if (candidate.id == layer.clipId) { clip = &candidate; break; }
        }
    }
    if (!clip)
        return QStringLiteral("AI 消除层“%1”引用了不存在的视频片段。").arg(layer.id);
    if (layer.sourceOutUs <= layer.sourceInUs)
        return QStringLiteral("AI 消除层“%1”的区间无效。").arg(layer.id);
    const Asset *asset = project.assetById(clip->assetId);
    if (asset && asset->info.durationUs > 0
        && layer.sourceOutUs > asset->info.durationUs)
        return QStringLiteral("AI 消除层“%1”的区间超出素材时长。").arg(layer.id);
    if (layer.maskKeyframes.isEmpty())
        return QStringLiteral("AI 消除层“%1”至少需要一个遮罩关键帧。").arg(layer.id);

    const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
    int64_t previousUs = -1;
    for (const MaskKeyframe &keyframe : layer.maskKeyframes) {
        if (keyframe.id.isEmpty() || keyframe.sourceTimeUs <= previousUs)
            return QStringLiteral("AI 消除层“%1”的遮罩关键帧必须按时间排序且不重复。")
                .arg(layer.id);
        if (!hex64.match(keyframe.maskHash).hasMatch())
            return QStringLiteral("AI 消除层“%1”的遮罩引用无效。").arg(layer.id);
        if (keyframe.featherPx < 0 || keyframe.featherPx > 64)
            return QStringLiteral("AI 消除层“%1”的羽化超出 0–64 像素。").arg(layer.id);
        previousUs = keyframe.sourceTimeUs;
    }
    if (layer.parameters.processingWidth <= 0
        || layer.parameters.processingWidth > 8192
        || layer.parameters.processingHeight <= 0
        || layer.parameters.processingHeight > 8192)
        return QStringLiteral("AI 消除层“%1”的处理分辨率无效。").arg(layer.id);
    if (!layer.cacheKey.isEmpty() && !hex64.match(layer.cacheKey).hasMatch())
        return QStringLiteral("AI 消除层“%1”的缓存键无效。").arg(layer.id);
    if (!layer.cachePath.isEmpty()
        && (!layer.cachePath.startsWith(QStringLiteral("removal/"))
            || layer.cachePath.contains(QStringLiteral(".."))))
        return QStringLiteral("AI 消除层“%1”的缓存路径无效。").arg(layer.id);
    if (!clip->removalLayerId.isEmpty()) {
        if (attachedClips.contains(clip->id) || clip->removalLayerId != layer.id)
            return QStringLiteral("片段“%1”关联了多个 AI 消除层。").arg(clip->id);
        attachedClips.insert(clip->id);
    }
}
// 反向检查：片段不得引用不存在的层（例如手工编辑工程或旧版本残留）。
for (const Track &track : project.tracks) {
    for (const Clip &clip : track.clips) {
        if (!clip.removalLayerId.isEmpty()
            && !layerIds.contains(clip.removalLayerId))
            return QStringLiteral("片段“%1”引用了不存在的 AI 消除层。").arg(clip.id);
    }
}
```

- [ ] **Step 6: 运行测试，确认 schema 通过**

Run: `cmake --build --preset windows-x64-release --target core_tests && ctest --preset windows-x64-release -R "Removal" --output-on-failure`
Expected: `RemovalSerializer.*` 全部 PASS；既有 `ProjectSerializer.*` 全部 PASS。

- [ ] **Step 7: 写失败的撤销命令测试**

`tests/core/RemovalCommandTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "ProjectDocument.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalCommand, UndoRedoRestoresLayerAndClipLink)
{
    Project p = projectWithClip();
    ProjectDocument doc;
    doc.setProject(p);

    RemovalLayer layer = sampleLayer();
    RemovalLayerEdit edit;
    edit.clipId = QStringLiteral("clip-1");
    edit.before = std::nullopt;
    edit.after = layer;
    doc.applyRemovalLayerEdit(edit, QStringLiteral("添加 AI 消除层"));

    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().tracks[0].clips[0].removalLayerId, layer.id);

    doc.undo();
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());
    EXPECT_TRUE(doc.project().tracks[0].clips[0].removalLayerId.isEmpty());

    doc.redo();
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().tracks[0].clips[0].removalLayerId, layer.id);
}

TEST(RemovalCommand, DeletingLayerRestoresOriginalAndUndoRestoresCacheKey)
{
    Project p = projectWithClip();
    RemovalLayer layer = sampleLayer();
    p.removalLayers.append(layer);
    p.tracks[0].clips[0].removalLayerId = layer.id;
    ProjectDocument doc;
    doc.setProject(p);

    RemovalLayerEdit remove;
    remove.clipId = layer.clipId;
    remove.before = layer;
    remove.after = std::nullopt;
    doc.applyRemovalLayerEdit(remove, QStringLiteral("删除 AI 消除层"));
    EXPECT_TRUE(doc.project().removalLayers.isEmpty());

    doc.undo();
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().removalLayers[0].cacheKey, layer.cacheKey);
}
```

- [ ] **Step 8: 运行确认失败**

Run: `cmake --build --preset windows-x64-release --target core_tests`
Expected: 编译失败（`RemovalLayerEdit`/`applyRemovalLayerEdit` 未定义）。

- [ ] **Step 9: 实现命令与文档方法**

`RemovalLayerCommand.h`：

```cpp
#pragma once

#include <optional>
#include <QUndoCommand>
#include "Removal.h"

namespace VideoEditor {
class ProjectDocument;

struct RemovalLayerEdit {
    QString clipId;
    std::optional<RemovalLayer> before;
    std::optional<RemovalLayer> after;
};

class RemovalLayerCommand : public QUndoCommand {
public:
    RemovalLayerCommand(ProjectDocument *document, const QString &text,
                        RemovalLayerEdit edit);
    void redo() override;
    void undo() override;

private:
    void apply(const std::optional<RemovalLayer> &layer);
    ProjectDocument *m_document;
    RemovalLayerEdit m_edit;
};
} // namespace VideoEditor
```

`ProjectDocument.h` 增加：

```cpp
void applyRemovalLayerEdit(const RemovalLayerEdit &edit, const QString &text);
const RemovalLayer *removalLayerById(const QString &id) const;
RemovalLayer *removalLayerById(const QString &id);

signals:
    void removalLayersChanged();
```

private（`friend class RemovalLayerCommand`）：

```cpp
void upsertRemovalLayerInternal(const RemovalLayer &layer);
void removeRemovalLayerInternal(const QString &layerId);
void setClipRemovalLayerInternal(const QString &clipId, const QString &layerId);
```

实现要点：

```cpp
void ProjectDocument::upsertRemovalLayerInternal(const RemovalLayer &layer)
{
    for (RemovalLayer &existing : m_project.removalLayers) {
        if (existing.id == layer.id) { existing = layer; return; }
    }
    m_project.removalLayers.append(layer);
}

void ProjectDocument::removeRemovalLayerInternal(const QString &layerId)
{
    for (int i = 0; i < m_project.removalLayers.size(); ++i) {
        if (m_project.removalLayers[i].id == layerId) {
            m_project.removalLayers.removeAt(i);
            break;
        }
    }
    for (Track &track : m_project.tracks) {
        for (Clip &clip : track.clips)
            if (clip.removalLayerId == layerId)
                clip.removalLayerId.clear();
    }
}

void ProjectDocument::setClipRemovalLayerInternal(const QString &clipId,
                                                  const QString &layerId)
{
    for (Track &track : m_project.tracks) {
        for (Clip &clip : track.clips) {
            if (clip.id == clipId) { clip.removalLayerId = layerId; return; }
        }
    }
}

void ProjectDocument::applyRemovalLayerEdit(const RemovalLayerEdit &edit,
                                            const QString &text)
{
    m_undoStack.push(new RemovalLayerCommand(this, text, edit));
}
```

`RemovalLayerCommand::apply` 的逻辑：
- `layer` 有值：`upsertRemovalLayerInternal(*layer)`，`setClipRemovalLayerInternal(layer->clipId, layer->id)`。
- `layer` 为空：若 `m_edit.before` 有值则 `removeRemovalLayerInternal(before->id)`；否则 `setClipRemovalLayerInternal(m_edit.clipId, {})`。
- 每次 apply 后 emit `changed()`、`timelineChanged()`、`removalLayersChanged()`、`dirtyChanged()`。
- `redo()` 调 `apply(m_edit.after)`；`undo()` 调 `apply(m_edit.before)`。

- [ ] **Step 10: 运行全部相关测试并提交**

Run:
`cmake --build --preset windows-x64-release --target core_tests && ctest --preset windows-x64-release -R "Removal|Project" --output-on-failure`
Expected: PASS；`ProjectDocumentTest`、`ProjectSerializerTest`、`TimelineEvaluatorTest`、`TimelineEditorTest` 无回归。

```bash
git add src/core/Removal.h src/core/Removal.cpp src/core/Timeline.h src/core/Project.h src/core/Project.cpp src/core/ProjectSerializer.cpp src/core/ProjectDocument.h src/core/ProjectDocument.cpp src/core/commands/RemovalLayerCommand.h src/core/commands/RemovalLayerCommand.cpp src/core/CMakeLists.txt tests/RemovalTestProject.h tests/core/RemovalSerializerTest.cpp tests/core/RemovalCommandTest.cpp tests/CMakeLists.txt
git commit -m "feat(core): add removal layer schema v3 and undo commands"
```

---

### Task 2: 遮罩 RLE、工程旁车、缓存键、manifest 与 RemovalCache

**Files:**
- Create: `src/core/MaskCodec.h/.cpp`（含 `BinaryMask`）
- Create: `src/core/MaskEditor.h/.cpp`
- Create: `src/core/MaskSidecarStore.h/.cpp`
- Create: `src/core/RemovalCacheKey.h/.cpp`
- Create: `src/media/cache/RemovalCache.h/.cpp`
- Modify: `src/media/cache/CacheStore.h/.cpp`
- Create: `tests/core/MaskCodecTest.cpp`
- Create: `tests/core/MaskSidecarStoreTest.cpp`
- Create: `tests/core/RemovalCacheKeyTest.cpp`
- Create: `tests/media/RemovalCacheTest.cpp`
- Modify: `src/core/CMakeLists.txt`、`src/media/CMakeLists.txt`、`tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MaskKeyframe`、`RemovalParameters`、`RemovalLayer`、`fingerprintFile`、`Rational`、`QImage`。
- Produces:
  - `struct BinaryMask { int width; int height; std::vector<uint8_t> bits; bool at(int,int) const; void set(int,int,bool); bool isValid() const; }`。
  - `MaskCodec::encode(const BinaryMask&) -> QByteArray`、`decode(QByteArray, QString*) -> std::optional<BinaryMask>`、`hash(const BinaryMask&) -> QString`。
  - `MaskEditor::paintStroke(BinaryMask&, QPoint from, QPoint to, int radiusPx, bool erase)`、`paintBox(BinaryMask&, QRect, bool erase)`、`contentBounds(const BinaryMask&) -> QRect`、`toOverlay(const BinaryMask&, QRgb) -> QImage`。
  - `MaskSidecarStore`：`rootForProject`、`setProjectPath`、`relocateTo`、`writeMask/readMask`、`writeTracking/readTracking`、`removeUnreferenced`。
  - `struct RemovalKeyInput`、`canonicalRemovalKeyJson`、`computeRemovalCacheKey`、`qint64 removalCacheBudgetBytes(qint64 sourceBytes)`（返回 `sourceBytes*2`，UI 缓存状态与验收共用）。
  - `enum class RemovalCacheState { Missing, Partial, Complete, Corrupt }`。
  - `struct RemovalChunkFrame { int frame; QRect crop; QString file; QString sha256; }`、`struct RemovalChunkIndex { int index; int firstFrame; QList<RemovalChunkFrame> frames; QString treeSha256; }`。
  - `struct RemovalCacheChunk { int index; int firstFrame; int frameCount; QString treeSha256; }`。
  - `struct RemovalCacheManifest { QString schema; QString cacheKey; QString clipId; int64_t sourceInUs; int64_t sourceOutUs; int width; int height; Rational fps; QString trackingModelId; QString inpaintModelId; QString pipelineVersion; QString status; QList<RemovalCacheChunk> chunks; int totalFrames; QString createdAt; }`。
  - `RemovalCache`：`cacheDir`、`writeChunk`、`writeManifest`、`readManifest`、`readChunkIndex`、`readFrameImage`、`state`、`verify`、`invalidateFrameRange`、`collectGarbage`、`sizeOnDisk`。
  - `CacheStore::removalRoot/removalPath/ensureRemovalDir/clearRemoval`。

- [ ] **Step 1: 写失败的 RLE/哈希测试**

`tests/core/MaskCodecTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QRegularExpression>
#include "MaskCodec.h"

using namespace VideoEditor;

TEST(MaskCodec, RoundTripsRandomMask)
{
    BinaryMask mask;
    mask.width = 37;
    mask.height = 23;
    mask.bits.assign(mask.width * mask.height, 0);
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            mask.set(x, y, ((x * 7 + y * 13) % 11) < 5);

    const QByteArray encoded = MaskCodec::encode(mask);
    QString error;
    const auto decoded = MaskCodec::decode(encoded, &error);
    ASSERT_TRUE(decoded.has_value()) << error.toStdString();
    EXPECT_EQ(decoded->width, mask.width);
    EXPECT_EQ(decoded->height, mask.height);
    EXPECT_EQ(decoded->bits, mask.bits);
}

TEST(MaskCodec, HashIsStableAndChangesWithPixels)
{
    BinaryMask a;
    a.width = a.height = 16;
    a.bits.assign(256, 0);
    a.set(8, 8, true);
    BinaryMask b = a;
    EXPECT_EQ(MaskCodec::hash(a), MaskCodec::hash(b));

    b.set(9, 8, true);
    EXPECT_NE(MaskCodec::hash(a), MaskCodec::hash(b));
    EXPECT_EQ(MaskCodec::hash(a).size(), 64);
    EXPECT_TRUE(MaskCodec::hash(a).contains(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))));
}

TEST(MaskCodec, RejectsTruncatedPayload)
{
    BinaryMask mask;
    mask.width = mask.height = 8;
    mask.bits.assign(64, 0);
    QByteArray encoded = MaskCodec::encode(mask);
    encoded.truncate(encoded.size() - 4);
    QString error;
    EXPECT_FALSE(MaskCodec::decode(encoded, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));
}

TEST(MaskEditor, StrokeAndBoxAreClampedAndDeterministic)
{
    BinaryMask mask;
    mask.width = 64;
    mask.height = 64;
    mask.bits.assign(4096, 0);
    MaskEditor::paintStroke(mask, QPoint(-10, 32), QPoint(80, 32), 5, false);
    const QRect bounds = MaskEditor::contentBounds(mask);
    EXPECT_FALSE(bounds.isEmpty());
    EXPECT_GE(bounds.left(), 0);
    EXPECT_LE(bounds.right(), 63);

    BinaryMask boxed;
    boxed.width = boxed.height = 64;
    boxed.bits.assign(4096, 0);
    MaskEditor::paintBox(boxed, QRect(50, 50, 40, 40), false);
    EXPECT_TRUE(boxed.at(63, 63));
    EXPECT_FALSE(boxed.at(0, 0));
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build --preset windows-x64-release --target core_tests`
Expected: 编译失败（`MaskCodec`/`BinaryMask` 未定义）。

- [ ] **Step 3: 实现 `MaskCodec` 与 `MaskEditor`**

`MaskCodec.h`：

```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

namespace VideoEditor {

struct BinaryMask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bits; // one byte per pixel, 0 or 255
    bool isValid() const { return width > 0 && height > 0
        && bits.size() == size_t(width) * size_t(height); }
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height
            && bits[size_t(y) * width + x] != 0;
    }
    void set(int x, int y, bool value) {
        if (x >= 0 && y >= 0 && x < width && y < height)
            bits[size_t(y) * width + x] = value ? 255 : 0;
    }
};

class MaskCodec {
public:
    static QByteArray encode(const BinaryMask &mask);
    static std::optional<BinaryMask> decode(const QByteArray &data,
                                             QString *error = nullptr);
    static QString hash(const BinaryMask &mask);
};

class MaskEditor {
public:
    static void paintStroke(BinaryMask &mask, QPoint from, QPoint to,
                            int radiusPx, bool erase);
    static void paintBox(BinaryMask &mask, QRect rect, bool erase);
    static QRect contentBounds(const BinaryMask &mask);
    static QImage toOverlay(const BinaryMask &mask, QRgb inside = 0x80FF3B30);
};

} // namespace VideoEditor
```

RLE 字节格式（确定性）：magic `"VEMASK01"`（8 字节）、`uint32 width`、`uint32 height`、`uint32 runCount`、随后 `runCount` 个 `uint32` run 长度，交替表示 0/255，从 0 开始。全部小端。`decode` 必须校验总 run 长度等于 `width*height`、`runCount` 不超过剩余字节数、宽高 ≤ 8192。

`MaskEditor::paintStroke` 用整数 Bresenham/线性插值，对每个采样点调用：

```cpp
static void stampCircle(BinaryMask &mask, int cx, int cy, int radius, bool erase)
{
    const int r = qMax(1, radius);
    const int r2 = r * r;
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r2)
                mask.set(x, y, !erase);
}
```

`paintBox` 对 `QRect` 的四个边界 clamp 到 `[0,width-1]/[0,height-1]`；`contentBounds` 返回所有非零像素的包围盒；`toOverlay` 返回 `QImage::Format_ARGB32`，遮罩内为 `inside`，外部透明（供 Task 3 的 overlay provider 使用）。

- [ ] **Step 4: 写失败的缓存键测试**

`tests/core/RemovalCacheKeyTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include "RemovalCacheKey.h"

using namespace VideoEditor;

namespace {
RemovalKeyInput baseInput()
{
    RemovalKeyInput in;
    in.assetFingerprint = QString(64, QLatin1Char('a'));
    in.sourceInUs = 0;
    in.sourceOutUs = 10'000'000;
    in.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('b')), 4, 1});
    in.parameters.mode = RepairMode::Fast;
    in.parameters.temporalStabilization = true;
    in.parameters.stabilizationStrength = 50;
    in.parameters.processingWidth = 1920;
    in.parameters.processingHeight = 1080;
    in.trackingModelId = QStringLiteral("sam2.1-hiera-tiny");
    in.trackingModelHash = QString(64, QLatin1Char('c'));
    in.inpaintModelId = QStringLiteral("migan-512");
    in.inpaintModelHash = QString(64, QLatin1Char('d'));
    in.pipelineVersion = QStringLiteral("m4.0");
    in.colorSpace = QStringLiteral("BT.709");
    in.fps = Rational{30, 1};
    in.width = 1920;
    in.height = 1080;
    return in;
}
} // namespace

TEST(RemovalCacheKey, IsStableAndExcludesTimelinePosition)
{
    const RemovalKeyInput in = baseInput();
    const QString first = computeRemovalCacheKey(in);
    EXPECT_EQ(first, computeRemovalCacheKey(in));
    EXPECT_EQ(first.size(), 64);
    const QString json = canonicalRemovalKeyJson(in);
    EXPECT_FALSE(json.contains(QStringLiteral("timeline")));
    EXPECT_FALSE(json.contains(QStringLiteral("timelineStart")));
}

TEST(RemovalCacheKey, ChangesForMaskParametersModelAndRange)
{
    const QString base = computeRemovalCacheKey(baseInput());

    RemovalKeyInput mask = baseInput();
    mask.maskKeyframes[0].maskHash = QString(64, QLatin1Char('e'));
    EXPECT_NE(base, computeRemovalCacheKey(mask));

    RemovalKeyInput feather = baseInput();
    feather.maskKeyframes[0].featherPx = 12;
    EXPECT_NE(base, computeRemovalCacheKey(feather));

    RemovalKeyInput params = baseInput();
    params.parameters.mode = RepairMode::Quality;
    EXPECT_NE(base, computeRemovalCacheKey(params));

    RemovalKeyInput model = baseInput();
    model.inpaintModelHash = QString(64, QLatin1Char('f'));
    EXPECT_NE(base, computeRemovalCacheKey(model));

    RemovalKeyInput range = baseInput();
    range.sourceOutUs = 9'000'000;
    EXPECT_NE(base, computeRemovalCacheKey(range));

    RemovalKeyInput fps = baseInput();
    fps.fps = Rational{60, 1};
    EXPECT_NE(base, computeRemovalCacheKey(fps));
}
```

- [ ] **Step 5: 实现缓存键**

`RemovalCacheKey.h`：

```cpp
#pragma once
#include <cstdint>
#include <QJsonObject>
#include <QList>
#include <QString>
#include "Removal.h"
#include "TimeBase.h"

namespace VideoEditor {

struct RemovalKeyInput {
    QString assetFingerprint;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    QList<MaskKeyframe> maskKeyframes;
    RemovalParameters parameters;
    QString trackingModelId;
    QString trackingModelHash;
    QString inpaintModelId;
    QString inpaintModelHash;
    QString pipelineVersion = QStringLiteral("m4.0");
    QString colorSpace = QStringLiteral("BT.709");
    Rational fps{30, 1};
    int width = 1920;
    int height = 1080;
};

QJsonObject canonicalRemovalKeyJson(const RemovalKeyInput &input);
QString computeRemovalCacheKey(const RemovalKeyInput &input);

} // namespace VideoEditor
```

`removalCacheBudgetBytes` 直接返回 `sourceBytes * 2`（溢出时 `qint64` 饱和到 `std::numeric_limits<qint64>::max()`）。

`canonicalRemovalKeyJson` 的键排序固定为（字母序，Qt `QJsonObject` 会排序；测试按字符串比较）：
`assetFingerprint, cacheKeyVersion(1), colorSpace, fps{den,num}, height, inpaintModel{hash,id}, masks[{featherPx,hash,revision,timeUs}], parameters{mode,processingHeight,processingWidth,stabilizationStrength,temporalStabilization}, pipelineVersion, sourceInUs, sourceOutUs, trackingModel{hash,id}, width`。所有 `int64_t` 写成 `double`（与现有序列化一致）；`mode` 用 `repairModeToString`。`computeRemovalCacheKey` 用 `QJsonDocument(...).toJson(QJsonDocument::Compact)` 后 SHA-256 小写十六进制。

- [ ] **Step 6: 写失败的旁车与缓存测试**

`tests/core/MaskSidecarStoreTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "MaskSidecarStore.h"

using namespace VideoEditor;

TEST(MaskSidecarStore, WritesReadsAndRelocatesMasks)
{
    QTemporaryDir dir;
    const QString projectPath = dir.filePath(QStringLiteral("我的 工程.veproj"));
    MaskSidecarStore store(projectPath);
    ASSERT_TRUE(store.ensureLayout());

    BinaryMask mask;
    mask.width = mask.height = 32;
    mask.bits.assign(1024, 0);
    mask.set(16, 16, true);

    QString hash;
    QString error;
    ASSERT_TRUE(store.writeMask(mask, &hash, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(store.maskPath(hash)));

    const auto read = store.readMask(hash, &error);
    ASSERT_TRUE(read.has_value()) << error.toStdString();
    EXPECT_EQ(read->bits, mask.bits);

    const QString newPath = dir.filePath(QStringLiteral("另存为.veproj"));
    ASSERT_TRUE(store.relocateTo(newPath, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(store.maskPath(hash)));

    QFile::remove(store.maskPath(hash));
    EXPECT_FALSE(store.readMask(hash, &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("遮罩")));
}

TEST(MaskSidecarStore, UnsavedProjectUsesFallbackRootAndMovesOnSaveAs)
{
    QTemporaryDir dir;
    MaskSidecarStore store; // empty project path
    store.setProjectPath(QString(), dir.path());
    EXPECT_TRUE(store.sidecarRoot().contains(QStringLiteral("unsaved")));
    ASSERT_TRUE(store.ensureLayout());

    BinaryMask mask;
    mask.width = mask.height = 8;
    mask.bits.assign(64, 0);
    mask.set(4, 4, true);
    QString hash;
    QString error;
    ASSERT_TRUE(store.writeMask(mask, &hash, &error)) << error.toStdString();

    const QString saved = dir.filePath(QStringLiteral("未命名工程.veproj"));
    ASSERT_TRUE(store.relocateTo(saved, &error)) << error.toStdString();
    EXPECT_TRUE(store.sidecarRoot().startsWith(saved + QStringLiteral(".assets")));
    EXPECT_TRUE(QFileInfo::exists(store.maskPath(hash)));
}
```

`tests/media/RemovalCacheTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "RemovalCache.h"

using namespace VideoEditor;

namespace {
RemovalCacheManifest manifestFor(const QString &key)
{
    RemovalCacheManifest m;
    m.cacheKey = key;
    m.clipId = QStringLiteral("clip-1");
    m.sourceInUs = 0;
    m.sourceOutUs = 1'000'000;
    m.width = 64;
    m.height = 64;
    m.fps = Rational{30, 1};
    m.pipelineVersion = QStringLiteral("m4.0");
    m.status = QStringLiteral("complete");
    m.totalFrames = 3;
    return m;
}

QList<QImage> patches()
{
    QList<QImage> images;
    for (int i = 0; i < 3; ++i) {
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(QColor(10 * i, 20, 30, 255));
        images.append(image);
    }
    return images;
}
} // namespace

TEST(RemovalCache, WritesChunkAndManifestAndReadsBack)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('a'));
    QString error;

    RemovalChunkIndex index;
    const QList<QRect> crops(3, QRect(4, 4, 16, 16));
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(), crops, &index, &error))
        << error.toStdString();
    EXPECT_EQ(index.frames.size(), 3);

    RemovalCacheManifest manifest = manifestFor(key);
    RemovalCacheChunk chunk;
    chunk.index = 0;
    chunk.firstFrame = 0;
    chunk.frameCount = 3;
    chunk.treeSha256 = index.treeSha256;
    manifest.chunks.append(chunk);
    ASSERT_TRUE(cache.writeManifest(manifest, &error)) << error.toStdString();

    EXPECT_EQ(cache.state(key), RemovalCacheState::Complete);
    QRect crop;
    const QImage image = cache.readFrameImage(key, index.frames[1], &error);
    ASSERT_FALSE(image.isNull()) << error.toStdString();
    EXPECT_EQ(image.size(), QSize(16, 16));
}

TEST(RemovalCache, DetectsPartialAndCorruptTree)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('b'));
    QString error;

    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(key, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    RemovalCacheManifest manifest = manifestFor(key);
    manifest.status = QStringLiteral("partial");
    RemovalCacheChunk chunk{0, 0, 3, index.treeSha256};
    manifest.chunks.append(chunk);
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    EXPECT_EQ(cache.state(key), RemovalCacheState::Partial);

    manifest.status = QStringLiteral("complete");
    ASSERT_TRUE(cache.writeManifest(manifest, &error));
    ASSERT_EQ(cache.state(key), RemovalCacheState::Complete);

    QFile file(cache.cacheDir(key) + QStringLiteral("/chunks/0/")
               + index.frames[0].file);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("broken", 6);
    file.close();
    EXPECT_EQ(cache.verify(key, &error), false);
    EXPECT_FALSE(error.isEmpty());
}

TEST(RemovalCache, GarbageCollectionNeverDeletesPinnedKeys)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString keep(64, QLatin1Char('c'));
    const QString drop(64, QLatin1Char('d'));
    QString error;
    RemovalChunkIndex index;
    ASSERT_TRUE(cache.writeChunk(keep, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    ASSERT_TRUE(cache.writeChunk(drop, 0, 0, patches(),
                                 QList<QRect>(3, QRect(0, 0, 16, 16)),
                                 &index, &error));
    cache.collectGarbage({keep}, 0, &error);
    EXPECT_TRUE(QFileInfo::exists(cache.cacheDir(keep)));
    EXPECT_FALSE(QFileInfo::exists(cache.cacheDir(drop)));
}
```

- [ ] **Step 7: 实现旁车存储**

`MaskSidecarStore.h` 关键签名：

```cpp
class MaskSidecarStore {
public:
    explicit MaskSidecarStore(QString projectFilePath = {});
    static QString rootForProject(const QString &projectFilePath,
                                  const QString &fallbackRoot = {});
    void setProjectPath(const QString &projectFilePath,
                        const QString &fallbackRoot = {});
    QString projectFilePath() const;
    QString sidecarRoot() const;
    QString maskPath(const QString &maskHash) const;
    QString trackingPath(const QString &confidenceHash) const;
    bool ensureLayout(QString *error = nullptr);
    bool writeMask(const BinaryMask &mask, QString *hashOut,
                   QString *error = nullptr);
    std::optional<BinaryMask> readMask(const QString &maskHash,
                                       QString *error = nullptr) const;
    bool writeTracking(const QJsonObject &json, QString *hashOut,
                       QString *error = nullptr);
    std::optional<QJsonObject> readTracking(const QString &confidenceHash,
                                            QString *error = nullptr) const;
    bool relocateTo(const QString &newProjectFilePath, QString *error = nullptr);
    bool removeUnreferenced(const QSet<QString> &maskHashes,
                            const QSet<QString> &trackingHashes,
                            QString *error = nullptr);
private:
    QString m_projectFilePath;
    QString m_fallbackRoot;
};
```

规则：
- `rootForProject(path, fallback)`：`path` 非空 → `path + ".assets"`；为空 → `fallback + "/removal-sidecars/unsaved"`。
- `maskPath(hash)` = `<root>/masks/<hash>.rle`；`trackingPath(hash)` = `<root>/tracking/<hash>.json`。路径拼接前必须用 `^[0-9a-f]{64}$` 校验 hash，否则返回空。
- 写入 `QSaveFile` 原子提交；写入前 `mkpath`。
- `relocateTo`：若新 root 与旧 root 不同，递归复制 `masks/`、`tracking/`（`QDir::mkpath` + 文件 `QFile::copy`），成功后更新 `m_projectFilePath`；复制失败返回中文错误，旧目录保留。
- `removeUnreferenced` 只删除不在集合中的文件；绝不删除整个 `masks/`/`tracking/` 目录。

- [ ] **Step 8: 实现 `RemovalCache` 与 `CacheStore` 扩展**

`RemovalCache.h` 关键签名：

```cpp
class RemovalCache {
public:
    explicit RemovalCache(QString root);
    QString root() const;
    QString cacheDir(const QString &cacheKey) const;
    bool ensureDir(const QString &cacheKey, QString *error = nullptr) const;

    bool writeChunk(const QString &cacheKey, int chunkIndex, int firstFrame,
                    const QList<QImage> &patches, const QList<QRect> &crops,
                    RemovalChunkIndex *written, QString *error = nullptr);
    bool writeManifest(const RemovalCacheManifest &manifest,
                       QString *error = nullptr);
    std::optional<RemovalCacheManifest> readManifest(
        const QString &cacheKey, QString *error = nullptr) const;
    std::optional<RemovalChunkIndex> readChunkIndex(
        const QString &cacheKey, int chunkIndex,
        QString *error = nullptr) const;
    QImage readFrameImage(const QString &cacheKey,
                          const RemovalChunkFrame &frame,
                          QString *error = nullptr) const;

    RemovalCacheState state(const QString &cacheKey,
                            QString *reason = nullptr) const;
    bool verify(const QString &cacheKey, QString *reason = nullptr) const;
    bool invalidateFrameRange(const QString &cacheKey, int firstFrame,
                              int lastFrame, QString *error = nullptr);
    bool collectGarbage(const QSet<QString> &pinnedKeys, qint64 maxBytes,
                        QString *error = nullptr);
    qint64 sizeOnDisk(QString *error = nullptr) const;
private:
    QString m_root;
};
```

`manifest.json` 字段固定为：`schema("m4.removal.cache.1"), cacheKey, clipId, sourceInUs, sourceOutUs, width, height, fps{num,den}, trackingModelId, inpaintModelId, pipelineVersion, status("complete"|"partial"|"failed"), totalFrames, createdAt, chunks[{index,firstFrame,frameCount,treeSha256}]`。读取时对未知字段忽略，对缺失的 `cacheKey/status/chunks` 视为损坏。

写入算法（必须按此顺序）：
1. `tmp = <root>/<key>/chunks/.tmp-<uuid>`；`mkpath(tmp)`。
2. 对每帧 `i`：`frame_%06d.png` 保存为 RGBA（`QImage::save(..., "PNG")`）；`RemovalChunkFrame{firstFrame+i, crop, "frame_%06d.png", sha256(fileBytes)}`。
3. 写 `index.json`（`QSaveFile`），内容 `{index, firstFrame, frames:[...]}`。
4. 计算树哈希：按相对路径排序，对每个文件更新 `SHA256(relativePath + '\0' + size + '\0' + fileSha + '\0')`。
5. 若目标 `chunks/<index>` 已存在，先删除；`QDir().rename(tmp, target)`。失败则保留 tmp 并返回中文错误。
6. `writeChunk` 不修改 manifest；调用方写 manifest 最后提交。

`state`：manifest 不存在/`cacheKey` 不匹配 → `Missing`；`status=="partial"` → `Partial`；`status=="failed"` → `Corrupt`；`status=="complete"` 时逐 chunk 检查目录与 `index.json` 存在，缺一 → `Corrupt`，否则 `Complete`。
`verify`：在 `state==Complete` 基础上重新计算每个 chunk 树哈希并与 `treeSha256` 比较；不匹配返回 false 并写中文原因。
`invalidateFrameRange`：删除与 `[firstFrame,lastFrame]` 相交的 chunk 目录；manifest `status="partial"`；写 manifest；返回成功。
`collectGarbage`：只遍历 `<root>/<64hex>` 目录；不在 `pinnedKeys` 的按最近修改时间升序删除，直到 `sizeOnDisk <= maxBytes`；`maxBytes<=0` 表示全部未 pin 的可删。

`CacheStore` 增加：

```cpp
QString removalRoot() const;
QString removalPath(const QString &cacheKey) const;
bool ensureRemovalDir(QString *error = nullptr) const;
bool clearRemoval(QString *error = nullptr) const;
```

`removalRoot()` = `<root>/removal`；`clearRemoval` 只删该目录；`clear()` 保持现有行为（只删缓存根，不影响工程旁车，因为旁车不在缓存根下）。

- [ ] **Step 9: 运行测试并提交**

Run:
`cmake --build --preset windows-x64-release --target core_tests media_tests && ctest --preset windows-x64-release -R "Mask|RemovalCache|RemovalCacheKey|Sidecar" --output-on-failure`
Expected: PASS；`ProxyTest`、`Export*` 无回归。

```bash
git add src/core/MaskCodec.h src/core/MaskCodec.cpp src/core/MaskEditor.h src/core/MaskEditor.cpp src/core/MaskSidecarStore.h src/core/MaskSidecarStore.cpp src/core/RemovalCacheKey.h src/core/RemovalCacheKey.cpp src/media/cache/RemovalCache.h src/media/cache/RemovalCache.cpp src/media/cache/CacheStore.h src/media/cache/CacheStore.cpp src/core/CMakeLists.txt src/media/CMakeLists.txt tests/core/MaskCodecTest.cpp tests/core/MaskSidecarStoreTest.cpp tests/core/RemovalCacheKeyTest.cpp tests/media/RemovalCacheTest.cpp tests/CMakeLists.txt
git commit -m "feat(core): add mask sidecars, cache key and removal cache"
```

---

### Task 3: 遮罩编辑器 UI、关键帧与预览叠加

**Files:**
- Create: `src/ui/RemovalController.h/.cpp`
- Create: `src/ui/MaskImageProvider.h/.cpp`
- Modify: `src/ui/PreviewController.h/.cpp`（Task 7 再加 compare；本任务只读取播放头）
- Modify: `src/ui/VideoPreviewItem.h/.cpp`（`contentRect`）
- Create: `src/app/RemovalPanel.qml`
- Create: `src/app/MaskEditorOverlay.qml`
- Modify: `src/app/InspectorPanel.qml`、`src/app/Main.qml`、`src/app/TimelineClipDelegate.qml`、`src/app/main.cpp`
- Modify: `src/ui/CMakeLists.txt`、`src/app/CMakeLists.txt`
- Create: `tests/ui/RemovalControllerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ProjectDocument`、`MaskSidecarStore`、`MaskCodec/MaskEditor`、`RemovalLayerCommand`、`PreviewController`、`VideoPreviewItem`。
- Produces:
  - `RemovalController(ProjectDocument&, MaskSidecarStore&, QObject *parent=nullptr)`；`void setPlayheadProvider(std::function<int64_t()>)`（生产环境注入 `Preview.positionMs*1000`/`Timeline.positionUs`，测试注入固定值）。
  - Q_PROPERTY：`maskMode`、`activeClipId`、`brushSize`、`featherPx`、`currentMaskHash`、`maskRevision`、`maskKeyframes`、`hasLayer`、`layerEnabled`、`error`。
  - Q_INVOKABLE：`beginMask(clipId)`、`endMask()`、`beginStroke(radiusSourcePx, erase)`、`continueStroke(QPointF fromNorm, QPointF toNorm)`、`endStroke()`、`paintBox(QRectF normalized, bool erase)`、`setFeatherPx(int)`、`addMaskKeyframeAtPlayhead()`、`removeMaskKeyframe(int)`、`setLayerEnabled(bool)`、`deleteLayer()`、`clearError()`。
  - `MaskImageProvider::setMask(const QImage&)`、`revision()`；QML 通过 `image://mask/<revision>` 使用。
  - `VideoPreviewItem::contentRect`（QRectF，预览项坐标）。
  - QML objectName：`removal-panel`、`removal-enter-button`、`removal-brush-button`、`removal-box-button`、`removal-erase-button`、`removal-feather-slider`、`removal-add-keyframe`、`removal-delete-keyframe`、`removal-keyframe-list`、`removal-exit-button`、`mask-editor-overlay`。

- [ ] **Step 1: 写失败的控制器测试**

`tests/ui/RemovalControllerTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "ProjectDocument.h"
#include "RemovalController.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalController, StrokeCreatesOneUndoCommandAndUndoRestoresMask)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    ASSERT_TRUE(controller.maskMode());
    const QString before = controller.currentMaskHash();

    controller.beginStroke(20, false);
    controller.continueStroke(QPointF(0.4, 0.4), QPointF(0.6, 0.6));
    controller.endStroke();

    EXPECT_NE(controller.currentMaskHash(), before);
    EXPECT_TRUE(doc.canUndo());
    EXPECT_EQ(doc.project().removalLayers.size(), 1);

    doc.undo();
    EXPECT_EQ(controller.currentMaskHash(), before);
    doc.redo();
    EXPECT_NE(controller.currentMaskHash(), before);
}

TEST(RemovalController, FeatherChangeDoesNotChangeMaskHash)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();
    const QString maskHash = controller.currentMaskHash();

    controller.setFeatherPx(24);
    EXPECT_EQ(controller.currentMaskHash(), maskHash);
    ASSERT_EQ(doc.project().removalLayers.size(), 1);
    EXPECT_EQ(doc.project().removalLayers[0].maskKeyframes[0].featherPx, 24);
}

TEST(RemovalController, AddAndDeleteKeyframesStaySorted)
{
    QTemporaryDir dir;
    ProjectDocument doc;
    doc.setProject(projectWithClip());
    MaskSidecarStore sidecars(dir.filePath(QStringLiteral("p.veproj")));
    RemovalController controller(doc, sidecars);

    controller.beginMask(QStringLiteral("clip-1"));
    controller.beginStroke(10, false);
    controller.continueStroke(QPointF(0.5, 0.5), QPointF(0.5, 0.5));
    controller.endStroke();

    // 测试直接调用控制器的时间点版本，避免依赖 QML 播放头。
    controller.addMaskKeyframeAtSourceTime(0, QStringLiteral("mk-a"));
    controller.addMaskKeyframeAtSourceTime(2'000'000, QStringLiteral("mk-b"));
    controller.addMaskKeyframeAtSourceTime(1'000'000, QStringLiteral("mk-c"));
    const auto layer = doc.project().removalLayers.first();
    EXPECT_EQ(layer.maskKeyframes.size(), 3);
    EXPECT_EQ(layer.maskKeyframes[1].sourceTimeUs, 1'000'000);

    controller.removeMaskKeyframe(1);
    EXPECT_EQ(doc.project().removalLayers.first().maskKeyframes.size(), 2);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build --preset windows-x64-release --target ui_tests`
Expected: 编译失败（`RemovalController` 未定义）。

- [ ] **Step 3: 实现 `RemovalController` 的遮罩编辑状态**

关键头文件：

```cpp
class RemovalController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool maskMode READ maskMode NOTIFY maskModeChanged)
    Q_PROPERTY(QString activeClipId READ activeClipId NOTIFY activeClipChanged)
    Q_PROPERTY(int brushSize READ brushSize WRITE setBrushSize NOTIFY brushSizeChanged)
    Q_PROPERTY(int featherPx READ featherPx NOTIFY featherChanged)
    Q_PROPERTY(QString currentMaskHash READ currentMaskHash NOTIFY maskChanged)
    Q_PROPERTY(int maskRevision READ maskRevision NOTIFY maskChanged)
    Q_PROPERTY(QVariantList maskKeyframes READ maskKeyframes NOTIFY layerChanged)
    Q_PROPERTY(bool hasLayer READ hasLayer NOTIFY layerChanged)
    Q_PROPERTY(bool layerEnabled READ layerEnabled NOTIFY layerChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    explicit RemovalController(ProjectDocument &document,
                               MaskSidecarStore &sidecars,
                               QObject *parent = nullptr);

    bool maskMode() const;
    QString activeClipId() const;
    int brushSize() const; void setBrushSize(int px);
    int featherPx() const;
    QString currentMaskHash() const;
    int maskRevision() const;
    QVariantList maskKeyframes() const;
    bool hasLayer() const;
    bool layerEnabled() const;
    QString error() const;

    Q_INVOKABLE void beginMask(const QString &clipId);
    Q_INVOKABLE void endMask();
    Q_INVOKABLE void beginStroke(int radiusSourcePx, bool erase);
    Q_INVOKABLE void continueStroke(QPointF fromNorm, QPointF toNorm);
    Q_INVOKABLE void endStroke();
    Q_INVOKABLE void paintBox(QRectF normalized, bool erase);
    Q_INVOKABLE void setFeatherPx(int px);
    Q_INVOKABLE void addMaskKeyframeAtPlayhead();
    Q_INVOKABLE void removeMaskKeyframe(int index);
    Q_INVOKABLE void setLayerEnabled(bool enabled);
    Q_INVOKABLE void deleteLayer();
    Q_INVOKABLE void clearError();

    // 测试/时间线委托使用；sourceTimeUs 相对源素材。
    Q_INVOKABLE void addMaskKeyframeAtSourceTime(int64_t sourceTimeUs,
                                                 const QString &id = {});
    Q_INVOKABLE QVariantList maskKeyframesForClip(const QString &clipId) const;

signals:
    void maskModeChanged();
    void activeClipChanged();
    void brushSizeChanged();
    void featherChanged();
    void maskChanged();
    void layerChanged();
    void errorChanged();
    void overlayChanged(QImage image, QRectF contentRect); // 连接 MaskImageProvider

private:
    struct WorkingState {
        QString clipId;
        QString layerId;
        QString keyframeId;
        int64_t sourceTimeUs = 0;
        BinaryMask mask;
        QString beforeMaskHash;
        bool strokeActive = false;
    };
    // ...
};
```

实现规则：
- `beginMask(clipId)`：只接受视频轨上存在、`asset` 有 `hasVideo` 的片段；否则 `error="只能对视频片段使用 AI 消除。"` 并返回。遮罩尺寸用 `displaySizeForAsset(*asset)`（旋转 90/270 时交换宽高）。若片段已有 `removalLayerId`，加载该层；否则创建内存中的待提交层（`id = "rl-" + QUuid`），首次落笔时才 push 命令。
- 当前播放头来源：`Preview.positionUs` 或 `Timeline.positionUs`（通过 `AppController` 注入的 `std::function<int64_t()>`)；`RemovalController` 增加 `setPlayheadProvider(std::function<int64_t()>)`，测试可注入。`sourceTimeUs = sourceTimeAt(clip, playheadUs)`。
- `beginStroke`：若当前时间没有关键帧，选最近关键帧的遮罩克隆（无则新建全零遮罩）；记录 `beforeMaskHash`；`strokeActive=true`。
- `continueStroke`：把归一化坐标映射到 `mask.width/height`，`MaskEditor::paintStroke`；`emit maskChanged()`；更新 overlay provider。
- `endStroke`：若哈希变化，`MaskCodec` 写 sidecar，更新/创建 `MaskKeyframe`，用 `RemovalLayerEdit` push 一个命令（`text="AI 消除遮罩"`），`strokeActive=false`。
- `paintBox`：映射归一化矩形到像素，`MaskEditor::paintBox`，立即写 sidecar + 一个命令。
- `setFeatherPx`：clamp 0..64，只改当前关键帧 `featherPx`，push 一个命令；不写遮罩 sidecar。
- `addMaskKeyframeAtSourceTime`：按时间插入并保持排序；新关键帧复制最近遮罩（无则空遮罩），写 sidecar；一个命令。
- `removeMaskKeyframe`：至少保留 1 个；否则 `error="至少保留一个遮罩关键帧。"`。
- `deleteLayer`：`RemovalLayerEdit{before=layer, after=nullopt}`；`endMask()`。
- 每次 `ProjectDocument::changed` 后重新读取当前层/关键帧，刷新 `maskKeyframes` 与 overlay（避免撤销后 UI 不更新）。

- [ ] **Step 4: 预览叠加与坐标映射**

`VideoPreviewItem` 增加：

```cpp
Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)
QRectF contentRect() const { return aspectFitRect(QSizeF(m_frameSize)); }
```

`m_frameSize` 在 `submitFrame` 中更新；大小变化时 `emit contentRectChanged()`；`geometryChange` 也 emit。

`MaskImageProvider`（`QQuickImageProvider`）：

```cpp
class MaskImageProvider : public QQuickImageProvider {
public:
    MaskImageProvider();
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
    void setMask(const QImage &image);
    int revision() const;
private:
    QImage m_image;
    int m_revision = 0;
};
```

`RemovalController` 在每次遮罩变化时把 `MaskEditor::toOverlay(mask)` 推给 provider；`main.cpp` 用 `engine.addImageProvider("mask", provider)` 注册，并把 `engine.rootContext()->setContextProperty("Removal", controller.removal())` 加入 QML 上下文（与现有 `App/Preview/Timeline/Export` 一致）。`MaskEditorOverlay.qml` 中：

```qml
Image {
    id: maskImage
    anchors.fill: parent
    fillMode: Image.PreserveAspectFit
    source: Removal.maskRevision === 0 ? "" : "image://mask/" + Removal.maskRevision
    visible: Removal.maskMode
    opacity: 0.85
}
```

叠加图必须与视频内容矩形一致：`MaskEditorOverlay` 放在 `previewArea` 内，`maskImage` 的 `x/y/width/height` 绑定 `previewItem.contentRect`；画笔坐标用 `(mouse.x - rect.x)/rect.width` 归一化。遮罩叠加只存在于预览 QML，不进入 `TimelineCompositor`，因此导出永远不包含编辑遮罩，也不需要为 overlay 增加导出分支。`MaskImageProvider` 只持有当前编辑遮罩的 ARGB 图；`RemovalController` 在 `maskChanged` 时更新它。

- [ ] **Step 5: 写 QML 面板与时间线关键帧**

`RemovalPanel.qml` 首屏（右侧检查器内，`objectName: "removal-panel"`）：

- `Button { objectName: "removal-enter-button"; text: "AI 消除"; enabled: Timeline.selectedClipId !== ""; onClicked: Removal.beginMask(Timeline.selectedClipId) }`
- 遮罩模式工具条：画笔 `removal-brush-button`、框选 `removal-box-button`、擦除 `removal-erase-button`、画笔大小 Slider、羽化 `removal-feather-slider`（0–64）、`removal-add-keyframe`、`removal-delete-keyframe`、`removal-exit-button`。
- 关键帧列表 `removal-keyframe-list`，每项显示时间码与“删”。
- 层开关 `removal-layer-enabled`、删除层 `removal-delete-layer`。
- 高级折叠 `removal-advanced-toggle` 在 Task 8 加模型/分辨率/稳定化。

`MaskEditorOverlay.qml`：

```qml
Item {
    id: overlay
    objectName: "mask-editor-overlay"
    anchors.fill: parent
    visible: Removal.maskMode
    property rect content: previewItem.contentRect
    property bool boxMode: false
    property bool eraseMode: false

    function norm(point) {
        return Qt.point((point.x - content.x) / Math.max(1, content.width),
                        (point.y - content.y) / Math.max(1, content.height))
    }

    MouseArea {
        anchors.fill: parent
        onPressed: (mouse) => {
            const p = overlay.norm(Qt.point(mouse.x, mouse.y))
            if (overlay.boxMode) {
                overlay.boxStart = p
            } else {
                Removal.beginStroke(Removal.brushSize, overlay.eraseMode)
                Removal.continueStroke(p, p)
            }
        }
        onPositionChanged: (mouse) => {
            if (!pressed) return
            const p = overlay.norm(Qt.point(mouse.x, mouse.y))
            if (!overlay.boxMode) Removal.continueStroke(overlay.lastPoint, p)
            overlay.lastPoint = p
        }
        onReleased: (mouse) => {
            const p = overlay.norm(Qt.point(mouse.x, mouse.y))
            if (overlay.boxMode) {
                Removal.paintBox(Qt.rect(overlay.boxStart.x, overlay.boxStart.y,
                                         p.x - overlay.boxStart.x,
                                         p.y - overlay.boxStart.y),
                                 overlay.eraseMode)
            } else {
                Removal.endStroke()
            }
        }
    }
    property point boxStart: Qt.point(0, 0)
    property point lastPoint: Qt.point(0, 0)
}
```

反向拖拽的框选在 Controller 内用 `QRectF::normalized()` 处理。

`TimelineClipDelegate.qml` 增加遮罩关键帧菱形：读取 `Removal.maskKeyframesForClip(model.id)`，在 clip 宽度内按 `(timeUs - sourceInUs)/(sourceOutUs-sourceInUs)` 定位；点击 `App.seekTimeline(clipStartUs + timeUs / speed)`（实现时用 `Timeline` 的 clip map 字段）。无层时列表为空。

`InspectorPanel.qml` 在视频片段区域加入 `RemovalPanel { visible: inspector.hasClip() && !inspector.isAudioClip() }`；`Main.qml` 在 `previewArea` 内 `VideoPreview` 之后加 `MaskEditorOverlay { anchors.fill: parent }`。

- [ ] **Step 6: 运行控制器测试并修到通过**

Run: `cmake --build --preset windows-x64-release --target ui_tests && ctest --preset windows-x64-release -R "RemovalController" --output-on-failure`
Expected: PASS；`TimelineControllerTest`、`TimelineModelTest` 无回归。

- [ ] **Step 7: 运行 QML 冒烟并提交**

Run:
`cmake --build --preset windows-x64-release --target app_qml_tests && ctest --preset windows-x64-release -R "TimelineQml|ExportQml" --output-on-failure`
Expected: 既有 QML 测试 PASS；`RemovalPanel.qml`/`MaskEditorOverlay.qml` 能被 `Main.qml` 加载（Task 8 再加专门 QML 测试）。

```bash
git add src/ui/RemovalController.h src/ui/RemovalController.cpp src/ui/MaskImageProvider.h src/ui/MaskImageProvider.cpp src/ui/PreviewController.h src/ui/PreviewController.cpp src/ui/VideoPreviewItem.h src/ui/VideoPreviewItem.cpp src/app/RemovalPanel.qml src/app/MaskEditorOverlay.qml src/app/InspectorPanel.qml src/app/Main.qml src/app/TimelineClipDelegate.qml src/app/main.cpp src/ui/CMakeLists.txt src/app/CMakeLists.txt tests/ui/RemovalControllerTest.cpp tests/CMakeLists.txt
git commit -m "feat(ui): add mask editor, keyframes and preview overlay"
```

---

### Task 4: AI 子进程、JSON 协议、任务管理与模型门禁

**Files:**
- Create: `src/ai/AiProtocol.h/.cpp`（含 `AiJobSpec`/`AiJobResult`）
- Create: `src/ai/AiTransport.h`
- Create: `src/ai/LocalAiTransport.h/.cpp`
- Create: `src/ai/AiJobManager.h/.cpp`
- Create: `src/ai/ModelRegistry.h/.cpp`
- Create: `src/ai/AiFeatureGate.h/.cpp`
- Create: `src/ai/AiEngineMain.cpp`
- Create: `src/ai/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`、`CMakeLists.txt`、`src/app/CMakeLists.txt`、`src/ui/CMakeLists.txt`
- Modify: `src/app/AppController.h/.cpp`
- Create: `tests/app/AiJobManagerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `RemovalParameters`、`MaskKeyframe`、`Rational`、`models.manifest.json`、Qt Network（`QLocalServer`/`QLocalSocket`/`QProcess`）。
- Produces:
  - `struct AiJobSpec { QString jobId, clipId, assetPath, sidecarRoot, cacheDir, cacheKey, modelsManifestPath, trackingModelId, inpaintModelId; int64_t sourceInUs, sourceOutUs; int width, height; Rational fps; QList<MaskKeyframe> maskKeyframes; RemovalParameters parameters; int redoFirstFrame=-1; int redoLastFrame=-1; bool resume=false; bool trackingOnly=false; }`。
  - `struct AiJobResult { bool ok; bool cancelled; QString errorCode; QString errorMessage; QString cacheKey; int completedFrames; QString confidenceHash; RemovalTrackingState trackingState; }`。
  - `AiProtocol::encodeLine(QJsonObject)`、`parseLine(QByteArray, QJsonObject*, QString*)`。
  - `class AiTransport : QObject`，纯虚 `start(const AiJobSpec&, QString*)`、`cancel()`，信号 `message(QJsonObject)`、`finished(int,QProcess::ExitStatus)`。
  - `LocalAiTransport`、`AiJobManager`、`ModelRegistry`、`AiFeatureGate`。
  - `AppController::aiJobs()`、`AppController::removal()`（Task 8 完整接线）。

- [ ] **Step 1: 写失败的协议/门禁测试**

`tests/app/AiJobManagerTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "AiJobManager.h"
#include "AiProtocol.h"
#include "ModelRegistry.h"

using namespace VideoEditor;

class FakeTransport : public AiTransport {
    Q_OBJECT
public:
    bool start(const AiJobSpec &spec, QString *error) override
    {
        m_spec = spec;
        QTimer::singleShot(0, this, [this] {
            emit message(QJsonObject{{QStringLiteral("event"), QStringLiteral("progress")},
                                     {QStringLiteral("stage"), QStringLiteral("tracking")},
                                     {QStringLiteral("frame"), 1},
                                     {QStringLiteral("totalFrames"), 10},
                                     {QStringLiteral("percent"), 10.0}});
            emit message(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                                     {QStringLiteral("cacheKey"), m_spec.cacheKey},
                                     {QStringLiteral("confidenceHash"), QString(64, QLatin1Char('a'))}});
            emit finished(0, QProcess::NormalExit);
        });
        return true;
    }
    void cancel() override { emit finished(0, QProcess::NormalExit); }
    AiJobSpec m_spec;
};

TEST(AiProtocol, RoundTripsJobSpec)
{
    AiJobSpec spec;
    spec.jobId = QStringLiteral("job-1");
    spec.clipId = QStringLiteral("clip-1");
    spec.assetPath = QStringLiteral("C:/视频/示例.mp4");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    spec.maskKeyframes.append(MaskKeyframe{
        QStringLiteral("mk-1"), 0, QString(64, QLatin1Char('b')), 4, 1});
    spec.parameters.mode = RepairMode::Fast;
    spec.fps = Rational{30, 1};

    QString error;
    const auto restored = aiJobSpecFromJson(aiJobSpecToJson(spec), &error);
    ASSERT_TRUE(restored.has_value()) << error.toStdString();
    EXPECT_EQ(restored->assetPath, spec.assetPath);
    EXPECT_EQ(restored->parameters.mode, RepairMode::Fast);
}

TEST(AiProtocol, RejectsMalformedAndOversizedLines)
{
    QJsonObject object;
    QString error;
    EXPECT_FALSE(AiProtocol::parseLine("{not json", &object, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(AiProtocol::parseLine(QByteArray(2 * 1024 * 1024, 'x'),
                                       &object, &error));
}

TEST(AiFeatureGate, FailsClosedWithoutLegalApproval)
{
    QTemporaryDir dir;
    const QString manifestPath = dir.filePath(QStringLiteral("models.manifest.json"));
    QFile file(manifestPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"schemaVersion":1,"commercialApproved":false,
                  "legalSignOff":{"approvedBy":"","date":"","scope":""},
                  "models":[]})");
    file.close();

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    QString reason;
    EXPECT_FALSE(registry->featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("许可证")));
}

TEST(AiJobManager, RunsProgressAndDoneWithFakeTransport)
{
    QTemporaryDir dir;
    QString gateError;
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new FakeTransport);
    });

    AiJobSpec spec;
    spec.jobId = QStringLiteral("job-1");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(spec, &error)) << error.toStdString();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result = qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.cacheKey, spec.cacheKey);
}

TEST(AiJobManager, RejectsDuplicateAndGateClosedJobs)
{
    AiFeatureGate gate;
    AiJobManager manager(gate);
    AiJobSpec spec;
    spec.jobId = QStringLiteral("job-1");
    QString error;
    EXPECT_FALSE(manager.start(spec, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("许可证")) || !error.isEmpty());
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build --preset windows-x64-release --target app_tests`
Expected: 编译失败（AI 头文件不存在）。

- [ ] **Step 3: 实现协议与模型清单**

`AiProtocol.h` 关键内容：

```cpp
struct AiJobSpec {
    QString jobId;
    QString clipId;
    QString assetPath;
    QString sidecarRoot;
    QString cacheDir;
    QString cacheKey;
    QString modelsManifestPath;
    QString trackingModelId;
    QString inpaintModelId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    QList<MaskKeyframe> maskKeyframes;
    RemovalParameters parameters;
    int redoFirstFrame = -1;
    int redoLastFrame = -1;
    bool resume = false;
};
QJsonObject aiJobSpecToJson(const AiJobSpec &spec);
std::optional<AiJobSpec> aiJobSpecFromJson(const QJsonObject &json,
                                           QString *error = nullptr);

struct AiJobResult {
    bool ok = false;
    bool cancelled = false;
    QString errorCode;
    QString errorMessage;
    QString cacheKey;
    int completedFrames = 0;
    QString confidenceHash;
    RemovalTrackingState trackingState = RemovalTrackingState::NotTracked;
};
Q_DECLARE_METATYPE(VideoEditor::AiJobSpec)
Q_DECLARE_METATYPE(VideoEditor::AiJobResult)

class AiProtocol {
public:
    static QByteArray encodeLine(const QJsonObject &object);
    static bool parseLine(const QByteArray &line, QJsonObject *out,
                          QString *error = nullptr);
    static constexpr int kMaxLineBytes = 1024 * 1024;
};
```

协议规则：`parseLine` 去掉末尾 `\n`/`\r`，拒绝空行、`> kMaxLineBytes`、非 JSON 对象、缺少 `event`（子进程→父）或 `cmd`（父→子）。`AiJobSpec` 的 JSON 字段名与结构体字段 camelCase 一致；`trackingState` 用 `removalTrackingStateToString`。

`ModelRegistry.h`：

```cpp
struct AiModelFile { QString path; QString sha256; };
struct AiModelRecord {
    QString id;
    QString family; // tracking | inpaint
    QString version;
    QString codeLicense;
    QString weightsLicense;
    QStringList trainingData;
    bool commercialApproved = false;
    QMap<QString, AiModelFile> files;
    QJsonObject io;
};
struct ModelManifest {
    int schemaVersion = 1;
    QString pipelineVersion;
    bool commercialApproved = false;
    QString legalSignOffBy;
    QString legalSignOffDate;
    QString legalScope;
    QList<AiModelRecord> models;
};
class ModelRegistry {
public:
    static std::optional<ModelRegistry> load(const QString &manifestPath,
                                             QString *error = nullptr);
    // 测试注入：不读磁盘模型文件；哈希仍由调用方填入 manifest。
    static ModelRegistry forTest(const ModelManifest &manifest,
                                 const QString &rootDir = {});
    bool verifyFiles(QString *error = nullptr) const;
    bool featureEnabled(QString *reason = nullptr) const;
    const AiModelRecord *model(const QString &id) const;
    const AiModelRecord *defaultTrackingModel() const;
    const AiModelRecord *defaultInpaintModel(const QString &mode) const;
    QString manifestPath() const;
    QString rootDir() const;
private:
    ModelManifest m_manifest;
    QString m_manifestPath;
    bool m_testMode = false; // forTest(): skip file hashing, keep approval/legal checks
};
```

`featureEnabled` 必须依次检查：
1. manifest 解析成功且 `schemaVersion==1`；
2. `manifest.commercialApproved == true` 且 `legalSignOffBy/date/scope` 非空；
3. 至少一个 `family=="tracking"` 且 `family=="inpaint"` 的模型 `commercialApproved==true`；
4. `verifyFiles` 全部通过（每个文件 SHA-256 匹配；`forTest()` 构造的 registry 跳过文件读取，但仍执行 1–3 与模型角色检查）；
失败原因分别为：`"AI 消除功能未启用：缺少模型许可证审批。"`、`"AI 消除功能未启用：模型文件缺失或校验失败（%1）。"`。缺少 tracking 或 inpaint 任一模型也归入后者（`%1` 写缺失角色），避免把“未安装模型”误报成许可证问题。

`AiFeatureGate`：

```cpp
class AiFeatureGate {
public:
    bool enabled() const;
    QString disabledReason() const;
    bool reload(const QString &modelsDir, QString *error = nullptr);
    const ModelRegistry *registry() const;
    // 仅测试使用；生产路径只能由 reload() 打开。
    void setEnabledForTest(bool enabled);
private:
    bool m_enabled = false;
    QString m_reason = QStringLiteral("AI 消除功能未启用：未加载模型清单。");
    std::optional<ModelRegistry> m_registry;
};
```

- [ ] **Step 4: 实现传输与任务管理**

`AiTransport.h`：

```cpp
class AiTransport : public QObject {
    Q_OBJECT
public:
    explicit AiTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~AiTransport() override = default;
    virtual bool start(const AiJobSpec &spec, QString *error = nullptr) = 0;
    virtual void cancel() = 0;
signals:
    void message(const QJsonObject &message);
    void finished(int exitCode, QProcess::ExitStatus status);
};
```

`LocalAiTransport`：
- `start`：`QLocalServer::removeServer(name)`；`listen(name)`；把 `spec` 写 `<temp>/m4-job-<uuid>.json`；`QProcess` 启动 `QCoreApplication::applicationDirPath()/VisionAiEngine.exe`，参数 `--job <path> --socket <name>`；`QProcessEnvironment` 清空网络代理变量（`HTTP_PROXY` 等），不传递任何 URL。
- 新连接：`nextPendingConnection()`；读缓冲按 `\n` 切分；每行 `AiProtocol::parseLine` 后 `emit message`；`bytesAvailable` 上限 1 MB，超限发 error 并 kill。
- `cancel`：向 socket 写 `{"cmd":"cancel"}`；启动 5 秒 `QTimer`，超时 `QProcess::kill()`；`finished` 原样转发。
- `finished` 后删除临时 job 文件（除非 `VIDEOEDITOR_KEEP_AI_TEMP=1`）。
- 子进程崩溃（`CrashExit`）→ 父进程 `AiJobManager` 生成 `errorCode="crash"`。

`AiJobManager`：

```cpp
class AiJobManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stageText READ stageText NOTIFY stageTextChanged)
    Q_PROPERTY(QString etaText READ etaText NOTIFY etaTextChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    explicit AiJobManager(AiFeatureGate &gate, QObject *parent = nullptr);
    bool running() const; bool paused() const;
    double progress() const; QString stageText() const;
    QString etaText() const; QString error() const;

    bool start(const AiJobSpec &spec, QString *error = nullptr);
    void cancel(); void pause(); void resume();
    void setTransportFactoryForTest(
        std::function<std::unique_ptr<AiTransport>()> factory);

signals:
    void runningChanged(); void pausedChanged();
    void progressChanged(); void stageTextChanged(); void etaTextChanged();
    void errorChanged();
    void warning(const QString &message);
    void confidenceReady(const QJsonObject &confidence);
    void jobFinished(const VideoEditor::AiJobResult &result);
private:
    void handleMessage(const QJsonObject &message);
    void finish(const AiJobResult &result);
    AiFeatureGate &m_gate;
    std::unique_ptr<AiTransport> m_transport;
    QString m_activeJobId;
    bool m_running = false;
    bool m_paused = false;
    double m_progress = 0.0;
    QString m_stageText;
    QString m_etaText;
    QString m_error;
    std::function<std::unique_ptr<AiTransport>()> m_factory;
};
```

规则：
- `start` 先检查 `m_gate.enabled()`、`!m_running`、`spec.cacheKey` 64 hex；失败给出中文错误（`"AI 消除功能未启用：..."`、`"已有 AI 任务在运行。"`、`"AI 任务参数无效。"`）。
- 只接受 `m_activeJobId` 匹配的消息；旧 job 的 progress/done 丢弃。
- `progress` 更新 percent/stage/eta；`confidence` emit `confidenceReady`；`warning` emit `warning`；`chunkDone` 只记日志；`error` 记录 code/message；`cancelled` 结束为 `cancelled=true`；`done` 结束为 `ok=true`。
- `cancel` 调 transport；`pause` 发 `{"cmd":"pause"}` 并置 paused；`resume` 发 `{"cmd":"resume"}`。
- 结束后 `m_running=false`，`emit jobFinished(result)`；transport 延迟删除。
- `finished(code, status)` 在未收到 `done`/`cancelled` 时：`status==CrashExit` → `errorCode="crash"`、`errorMessage="AI 推理进程已退出，已完成的分段已保留，可重试。"`；否则 `errorCode="engine"`、`errorMessage` 用已有 error 或 `"AI 任务失败（退出码 %1）。"`。

- [ ] **Step 5: 实现子进程入口（本任务只做协议与“模型未安装”错误）**

`AiEngineMain.cpp`：

```cpp
int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("job"), QStringLiteral("job spec"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("socket"), QStringLiteral("local socket"), QStringLiteral("name")});
    parser.process(app);

    const QString jobPath = parser.value(QStringLiteral("job"));
    const QString socketName = parser.value(QStringLiteral("socket"));
    QFile jobFile(jobPath);
    if (!jobFile.open(QIODevice::ReadOnly)) return 2;
    QString error;
    const auto spec = aiJobSpecFromJson(
        QJsonDocument::fromJson(jobFile.readAll()).object(), &error);
    if (!spec) return 2;

    QLocalSocket socket;
    socket.connectToServer(socketName);
    if (!socket.waitForConnected(5000)) return 3;

    const auto send = [&socket](const QJsonObject &message) {
        socket.write(AiProtocol::encodeLine(message));
        socket.flush();
    };

    // Task 4 只验证协议；模型依赖的流水线在 Task 5/6 接入。
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("error")},
                     {QStringLiteral("code"), QStringLiteral("model_unavailable")},
                     {QStringLiteral("message"), QStringLiteral("AI 模型尚未安装。")}});
    send(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                     {QStringLiteral("cacheKey"), spec->cacheKey},
                     {QStringLiteral("completedFrames"), 0}});
    socket.disconnectFromServer();
    return 3;
}
```

子进程不读取工程文件，只读 `AiJobSpec`；不访问网络；模型路径从 `modelsManifestPath` 解析（Task 5/6 实现）。

- [ ] **Step 6: CMake 与 AppController 接线**

根 `CMakeLists.txt`：

```cmake
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Qml Network Test)
add_subdirectory(src)
```

`src/CMakeLists.txt` 增加 `add_subdirectory(ai)`（在 core/media 之后、ui 之前或按依赖调整）。

`src/ai/CMakeLists.txt`：

```cmake
add_library(videoeditor_ai STATIC
  AiProtocol.h AiProtocol.cpp
  AiTransport.h
  LocalAiTransport.h LocalAiTransport.cpp
  AiJobManager.h AiJobManager.cpp
  ModelRegistry.h ModelRegistry.cpp
  AiFeatureGate.h AiFeatureGate.cpp
)
target_include_directories(videoeditor_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_ai PUBLIC
  videoeditor_core videoeditor_media Qt6::Core Qt6::Gui Qt6::Network)

option(VIDEOEDITOR_ENABLE_AI "Build local AI removal engine" ON)
set(VIDEOEDITOR_ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime root")
if(VIDEOEDITOR_ENABLE_AI)
  add_executable(VisionAiEngine AiEngineMain.cpp)
  target_link_libraries(VisionAiEngine PRIVATE videoeditor_ai videoeditor_media Qt6::Gui)
  if(VIDEOEDITOR_ONNXRUNTIME_ROOT AND EXISTS "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    target_sources(videoeditor_ai PRIVATE
      engine/OnnxExecutor.h engine/OnnxExecutor.cpp
      engine/Confidence.h engine/Confidence.cpp
      engine/Sam2Tracker.h engine/Sam2Tracker.cpp
      engine/TrackingPipeline.h engine/TrackingPipeline.cpp
      engine/MiGanInpainter.h engine/MiGanInpainter.cpp
      engine/LamaInpainter.h engine/LamaInpainter.cpp
      engine/InpaintPipeline.h engine/InpaintPipeline.cpp
      engine/TemporalStabilizer.h engine/TemporalStabilizer.cpp
      engine/FrameReader.h engine/FrameReader.cpp)
    target_compile_definitions(videoeditor_ai PUBLIC VIDEOEDITOR_HAVE_ORT=1)
    target_include_directories(videoeditor_ai PUBLIC
      "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/include")
    target_link_libraries(videoeditor_ai PUBLIC
      "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
  else()
    target_compile_definitions(videoeditor_ai PUBLIC VIDEOEDITOR_HAVE_ORT=0)
  endif()
endif()
```

`src/ui/CMakeLists.txt` 增加 `RemovalController.*`、`MaskImageProvider.*`，并链接 `videoeditor_ai`；`src/app/CMakeLists.txt` 的 `qt_add_qml_module` 增加四个 QML 文件；`videoeditor_app` 链接 `videoeditor_ai`，并在构建后复制 `VisionAiEngine.exe`、`onnxruntime.dll`、`DirectML.dll`、`models/`（若存在）。

`AppController` 增加：

```cpp
AiJobManager *aiJobs() const { return m_aiJobs; }
RemovalController *removal() const { return m_removal; }
```

构造顺序：`m_gate = std::make_unique<AiFeatureGate>(); m_gate->reload(appDir + "/models", ...); m_aiJobs = new AiJobManager(*m_gate, this);`；`MaskSidecarStore` 用 `document->filePath()` 或 recovery 根；Task 3 的 `RemovalController(document, sidecars, parent)` 构造保持不变，Task 4 增加 `void setJobManager(AiJobManager*)`、`void setCache(RemovalCache*)` 与 `void setFeatureGate(AiFeatureGate*)`，`AppController` 在构造后注入三者；未注入时 `track()/generate()` 返回中文错误“AI 任务管理器未初始化。”，但遮罩编辑仍可用。

- [ ] **Step 7: 运行测试并提交**

Run:
`cmake --build --preset windows-x64-release --target app_tests && ctest --preset windows-x64-release -R "AiProtocol|AiFeatureGate|AiJobManager" --output-on-failure`
Expected: PASS；`AppControllerTest` 无回归；未安装模型时 `AiFeatureGate` 为 disabled。

```bash
git add src/ai src/app/AppController.h src/app/AppController.cpp src/CMakeLists.txt CMakeLists.txt src/app/CMakeLists.txt src/ui/CMakeLists.txt tests/app/AiJobManagerTest.cpp tests/CMakeLists.txt
git commit -m "feat(ai): add local AI subprocess protocol, job manager and model gate"
```

---

### Task 5: 跟踪流水线（SAM 2.1 / EfficientTAM ONNX）

**Files:**
- Create: `src/ai/engine/OnnxExecutor.h/.cpp`
- Create: `src/ai/engine/Confidence.h/.cpp`
- Create: `src/ai/engine/Sam2Tracker.h/.cpp`
- Create: `src/ai/engine/TrackingPipeline.h/.cpp`
- Create: `src/ai/engine/FrameReader.h/.cpp`
- Modify: `src/ai/AiEngineMain.cpp`（接入 tracking 阶段）
- Modify: `src/ai/CMakeLists.txt`（已在 Task 4 声明）
- Create: `tests/ai/ConfidenceTest.cpp`
- Create: `tests/ai/TrackingPipelineTest.cpp`
- Modify: `tests/CMakeLists.txt`（新增 `ai_tests`）

**Interfaces:**
- Consumes: `ModelRegistry`、`AiJobSpec`、`BinaryMask`、`QImage`、ONNX Runtime C++ API。
- Produces:
  - `struct TrackingPrompt { QRectF box; QList<QPointF> points; QList<int> labels; }`（归一化源坐标；label 1 前景、0 背景）。
  - `struct TrackingFrameResult { BinaryMask mask; float predictedIou; float occlusion; int areaPx; }`。
  - `struct TrackingConfig { QMap<QString,QString> partPaths; QJsonObject io; int inputSize=1024; int memoryBankSize=5; int deviceIndex=0; bool allowDml=true; bool allowCpu=true; }`。
  - `Sam2Tracker::initialize(const TrackingConfig&, QString*)`、`segment(const QImage&, const TrackingPrompt&, QString*)`、`propagate(const QImage&, QString*)`、`resetMemory()`、`provider()`、`usingCpuFallback()`。
  - `FrameReader`：`open(path, Rational fps, QString*)`、`frameCount()`、`readFrame(int)`、`readAll()`（FFmpeg 软件解码，返回 RGB32）。
  - `struct ConfidenceSample { float predictedIou; float occlusion; int areaPx; int previousAreaPx; }`；`enum class FrameConfidence { Good, Fair, Low, Missing }`；`classifyConfidence`、`shouldWriteMemory`、`confidenceSummary`。
  - `TrackingPipeline::run(const AiJobSpec&, const QList<QImage>&, const QList<BinaryMask>&, ...)`（Task 5 只做跟踪；Task 6 在 `AiEngineMain` 串行调用）。

- [ ] **Step 1: 写失败的置信度测试**

`tests/ai/ConfidenceTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include "Confidence.h"

using namespace VideoEditor;

TEST(Confidence, ClassifiesBoundaries)
{
    EXPECT_EQ(classifyConfidence({0.90f, 0.10f, 1000, 1000}),
              FrameConfidence::Good);
    EXPECT_EQ(classifyConfidence({0.70f, 0.10f, 1000, 1000}),
              FrameConfidence::Fair);
    EXPECT_EQ(classifyConfidence({0.50f, 0.10f, 1000, 1000}),
              FrameConfidence::Low);
    EXPECT_EQ(classifyConfidence({0.99f, 0.80f, 1000, 1000}),
              FrameConfidence::Missing);
    // IoU 很好但面积变化 40%：Fair，不当作 Good 写入记忆。
    EXPECT_EQ(classifyConfidence({0.90f, 0.10f, 1400, 1000}),
              FrameConfidence::Fair);
    // 遮挡 0.2 边界仍是 Good；0.21 降为 Fair（IoU 仍高）。
    EXPECT_EQ(classifyConfidence({0.90f, 0.20f, 1000, 1000}),
              FrameConfidence::Good);
    EXPECT_EQ(classifyConfidence({0.90f, 0.21f, 1000, 1000}),
              FrameConfidence::Fair);
}

TEST(Confidence, MemoryGatingRejectsLowAndMissing)
{
    EXPECT_TRUE(shouldWriteMemory(FrameConfidence::Good, 5.0f));
    EXPECT_TRUE(shouldWriteMemory(FrameConfidence::Fair, 40.0f));
    EXPECT_FALSE(shouldWriteMemory(FrameConfidence::Low, 10.0f));
    EXPECT_FALSE(shouldWriteMemory(FrameConfidence::Missing, 0.0f));
    EXPECT_FALSE(shouldWriteMemory(FrameConfidence::Fair, 61.0f));
}
```

- [ ] **Step 2: 实现 `Confidence`**

```cpp
enum class FrameConfidence { Good, Fair, Low, Missing };

struct ConfidenceSample {
    float predictedIou = 0.0f;
    float occlusion = 0.0f;
    int areaPx = 0;
    int previousAreaPx = 0;
};

inline float areaChangePercent(int area, int prev)
{
    if (prev <= 0) return area > 0 ? 100.0f : 0.0f;
    return qAbs(area - prev) * 100.0f / float(prev);
}

inline FrameConfidence classifyConfidence(const ConfidenceSample &s)
{
    if (s.occlusion > 0.7f || s.areaPx <= 0)
        return FrameConfidence::Missing;
    const float change = areaChangePercent(s.areaPx, s.previousAreaPx);
    if (s.predictedIou >= 0.85f && s.occlusion <= 0.2f && change <= 30.0f)
        return FrameConfidence::Good;
    if (s.predictedIou >= 0.65f || change <= 60.0f)
        return FrameConfidence::Fair;
    return FrameConfidence::Low;
}

inline bool shouldWriteMemory(FrameConfidence state, float areaChangePct)
{
    return (state == FrameConfidence::Good || state == FrameConfidence::Fair)
        && areaChangePct <= 60.0f;
}

QJsonObject confidenceSummary(const QList<ConfidenceSample> &samples);
```

`confidenceSummary` 输出 `{"frames":[{"timeUs":...,"iou":...,"occlusion":...,"areaPx":...,"state":"good|fair|low|missing"}...]}`；state 用中文映射? 规格要求 UI 中文，但 JSON 用英文枚举，UI 再翻译。

- [ ] **Step 3: 实现 `OnnxExecutor` 与 `Sam2Tracker`**

`OnnxExecutor`：
- `initialize(const QStringList &partNames, const QMap<QString,QString> &paths, const EpConfig&, QString *error)`。
- EP 顺序：DML（`OrtSessionOptionsAppendExecutionProvider_DML(options, deviceId)`）→ CPU；设置 `SetGraphOptimizationLevel(ORT_ENABLE_ALL)`、`SetExecutionMode(ORT_SEQUENTIAL)`、`DisableMemPattern()`、`DisableCpuMemArena()`。
- 每个 session 记录 `provider` 与实际节点分配；若 DML session 创建失败，重试 fp32/CPU；若 CPU 回退，`usingCpuFallback=true` 并返回原因 `"当前显卡无法加速该模型，已改用 CPU，预计耗时增加。"`。
- 输入/输出名从 `TrackingConfig.io` 读取；若 manifest 的 `io` 为空，用 `session.GetInputNameAllocated` 并记录到日志，测试失败。

`Sam2Tracker` 预处理与后处理（精确算法）：
1. 输入 `QImage` → `QImage::Format_RGB888`；按最长边等比缩放到 `inputSize`，剩余区域用 0 填充成正方形；记录 `scale` 与 `padX/padY`。
2. 归一化：RGB/255，减 `{0.485,0.456,0.406}`，除 `{0.229,0.224,0.225}`；按 NCHW float32 填充输入张量。
3. `segment`：image_encoder → image embedding；prompt_encoder 接收 box/points 的连续坐标（已缩放到 1024 网格）→ sparse/dense embedding；mask_decoder → low-res logits + iou score；logits 双线性上采样到 256×256（若输出已是 256×256 则跳过），在缩放后的图像坐标上二值化（>0），再按 `scale/pad` 映射回原图 `BinaryMask`。
4. `propagate`：memory_attention 使用上一帧 memory bank + 当前 embedding；memory_encoder 把当前 mask/embedding 编码为 memory feature；仅在 `shouldWriteMemory` 为真时写入 memory bank；`memoryBankSize` 默认 5，超过时丢最旧。
5. 连续 3 帧 `Low/Missing` → `propagate` 返回 nullopt，调用方标记 `LowConfidence` 并停止；用户修正关键帧后 `resetMemory()` 并从该帧重新传播。
6. `occlusion`：若 `io.objectScore` 非空，取该输出；否则置 0，只用 IoU + 面积稳定性（manifest 记录 `objectScore:""`）。
7. 所有输出张量形状必须校验；不匹配返回中文错误 `"跟踪模型输出形状不符合预期。"`，不得越界读。

`TrackingPipeline::run`：
- 输入源帧由 `FrameReader` 按 `AiJobSpec.fps` 解出 `QList<QImage>`（或流式回调；M4 用 chunk 30 帧，内存 ≤ 30×1080p×4 ≈ 250MB，可接受）。
- 对每个 mask 关键帧调用 `segment` 初始化；向前/向后 `propagate`，直到区间边界或低置信停止。
- 每帧 mask 以 RLE 写 `<cacheDir>/tracking/masks/frame_%06d.rle`；置信度 JSON 写 `<sidecarRoot>/tracking/<hash>.json`，返回 `confidenceHash`。
- 进度事件：`{"event":"progress","stage":"tracking","frame":n,"totalFrames":N,"percent":...,"etaMs":...}`；每 chunk 完成发 `chunkDone`（含 chunk index）。
- 取消检查：每帧读 `AiEngineSession::cancelRequested()`；取消时保留已完成 chunk，发 `cancelled`。

- [ ] **Step 4: 写模型依赖测试（无模型/GPU 时跳过）**

`tests/ai/TrackingPipelineTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QDir>
#include <QFileInfo>
#include "FrameReader.h"
#include "ModelRegistry.h"
#include "RemovalEvalMetrics.h"
#include "Sam2Tracker.h"

using namespace VideoEditor;

namespace {
TrackingConfig configFor(const ModelRegistry &registry,
                         const AiModelRecord &record, bool useDml)
{
    TrackingConfig config;
    config.io = record.io;
    config.allowDml = useDml;
    config.allowCpu = true;
    for (auto it = record.files.cbegin(); it != record.files.cend(); ++it)
        config.partPaths.insert(it.key(),
                                QDir(registry.rootDir()).filePath(it.value().path));
    return config;
}
} // namespace

TEST(TrackingPipeline, DmlMatchesCpuOnFixedFrames)
{
    const QString modelsDir = qEnvironmentVariable("VIDEOEDITOR_M4_MODELS");
    const QString manifestPath = modelsDir + QStringLiteral("/models.manifest.json");
    if (modelsDir.isEmpty() || !QFileInfo::exists(manifestPath))
        GTEST_SKIP() << "M4 models not installed";
    if (!QFileInfo::exists(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)
                           + QStringLiteral("/removal/static_logo.mp4")))
        GTEST_SKIP() << "removal fixtures missing; run generate_fixtures.ps1";

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    const AiModelRecord *record = registry->defaultTrackingModel();
    ASSERT_NE(record, nullptr);

    FrameReader reader;
    ASSERT_TRUE(reader.open(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)
                            + QStringLiteral("/removal/static_logo.mp4"),
                            Rational{30, 1}, &error))
        << error.toStdString();
    const QList<QImage> frames = reader.readAll().mid(0, 20);
    ASSERT_EQ(frames.size(), 20);

    Sam2Tracker cpu;
    Sam2Tracker dml;
    ASSERT_TRUE(cpu.initialize(configFor(*registry, *record, false), &error))
        << error.toStdString();
    ASSERT_TRUE(dml.initialize(configFor(*registry, *record, true), &error))
        << error.toStdString();

    TrackingPrompt prompt;
    prompt.box = QRectF(520.0 / 640.0, 30.0 / 360.0,
                        80.0 / 640.0, 50.0 / 360.0);
    auto cpuResult = cpu.segment(frames[0], prompt, &error);
    auto dmlResult = dml.segment(frames[0], prompt, &error);
    ASSERT_TRUE(cpuResult.has_value()) << error.toStdString();
    ASSERT_TRUE(dmlResult.has_value()) << error.toStdString();

    for (int i = 1; i < frames.size(); ++i) {
        cpuResult = cpu.propagate(frames[i], &error);
        dmlResult = dml.propagate(frames[i], &error);
        ASSERT_TRUE(cpuResult.has_value()) << "cpu frame " << i << ": " << error.toStdString();
        ASSERT_TRUE(dmlResult.has_value()) << "dml frame " << i << ": " << error.toStdString();
        EXPECT_GE(maskIou(cpuResult->mask, dmlResult->mask), 0.98)
            << "frame " << i;
        EXPECT_LE(qAbs(cpuResult->predictedIou - dmlResult->predictedIou), 0.02f);
    }
}
```

模型文件路径通过 `VIDEOEDITOR_M4_MODELS` 指向 Task 0 的 `models.manifest.json` 所在目录；测试不得下载。

- [ ] **Step 5: 运行测试并提交**

在 `tests/CMakeLists.txt` 新增：

```cmake
add_executable(ai_tests
  ai/ConfidenceTest.cpp
  ai/TrackingPipelineTest.cpp
  TestGuiMain.cpp)
target_link_libraries(ai_tests PRIVATE
  videoeditor_ai videoeditor_media videoeditor_core
  GTest::gtest Qt6::Core Qt6::Gui Qt6::Test)
target_include_directories(ai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ai_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(ai_tests PROPERTIES
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30
  LABELS "ai")
```

Run:
`cmake --build --preset windows-x64-release --target ai_tests && ctest --preset windows-x64-release -R "Confidence|TrackingPipeline" --output-on-failure`
Expected: `Confidence.*` PASS；无模型时 `TrackingPipeline.*` SKIP 并写明原因。

```bash
git add src/ai/engine src/ai/AiEngineMain.cpp src/ai/CMakeLists.txt tests/ai/ConfidenceTest.cpp tests/ai/TrackingPipelineTest.cpp tests/CMakeLists.txt
git commit -m "feat(ai): add SAM2/EfficientTAM ONNX tracking pipeline and confidence gating"
```

---

### Task 6: 修复流水线（MI-GAN / LaMa）与时域稳定化

**Files:**
- Create: `src/ai/engine/MiGanInpainter.h/.cpp`
- Create: `src/ai/engine/LamaInpainter.h/.cpp`
- Create: `src/ai/engine/InpaintPipeline.h/.cpp`
- Create: `src/ai/engine/TemporalStabilizer.h/.cpp`
- Modify: `src/ai/AiEngineMain.cpp`（tracking 后串行 inpaint）
- Create: `tests/ai/InpaintPipelineTest.cpp`
- Create: `tests/ai/TemporalStabilizerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `BinaryMask`、`RemovalParameters`、`RemovalCache`（子进程写 PNG chunk 的算法与父进程一致）、`ModelRegistry`。
- Produces:
  - `struct InpaintConfig { QString miganPath; QString lamaPath; int miganTile=512; int lamaTile=512; int deviceIndex=0; bool allowDml=true; bool allowCpu=true; }`。
  - `struct PatchResult { QImage rgba; QRect crop; }`。
  - `class Inpainter { virtual ~Inpainter() = default; virtual std::optional<PatchResult> repair(const QImage &crop, const BinaryMask &maskCrop, QString*) = 0; }`。
  - `MiGanInpainter`、`LamaInpainter`。
  - `InpaintPipeline::repairChunk(const QList<QImage> &frames, const QList<BinaryMask> &masks, const RemovalParameters&, int featherPx, const std::function<bool(int,const QString&)> &progress, QString*) -> std::optional<QList<PatchResult>>`；`setInpainterForTest(std::unique_ptr<Inpainter>)`（测试注入，生产路径按 `RemovalParameters::mode` 选择）；`static int chooseTileSize(int cropW, int cropH, qint64 vramBudgetBytes)`（纯函数，预算紧张时从 512 降到 384/256）。
  - `TemporalStabilizer::stabilize(QList<QImage> &patches, const QList<QRect> &crops, const QList<QPoint> &centroids, int strength)`。

- [ ] **Step 1: 写失败的几何/羽化测试（无需模型）**

`tests/ai/InpaintPipelineTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QColor>
#include "InpaintPipeline.h"

using namespace VideoEditor;

namespace {
class SolidInpainter : public Inpainter {
public:
    std::optional<PatchResult> repair(const QImage &crop,
                                      const BinaryMask &maskCrop,
                                      QString *) override
    {
        QImage out = crop.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x)
                if (maskCrop.at(x, y))
                    out.setPixelColor(x, y, QColor(200, 30, 30, 255));
        return PatchResult{out, QRect()};
    }
};
} // namespace

TEST(InpaintPipeline, CropIsPaddedClampedAndFeathered)
{
    QImage frame(320, 240, QImage::Format_RGB32);
    frame.fill(Qt::darkGreen);
    BinaryMask mask;
    mask.width = 320;
    mask.height = 240;
    mask.bits.assign(320 * 240, 0);
    for (int y = 100; y < 140; ++y)
        for (int x = 140; x < 180; ++x)
            mask.set(x, y, true);

    InpaintPipeline pipeline;
    pipeline.setInpainterForTest(std::make_unique<SolidInpainter>());
    RemovalParameters params;
    params.mode = RepairMode::Fast;
    params.processingWidth = 320;
    params.processingHeight = 240;

    QString error;
    const auto patches = pipeline.repairChunk({frame}, {mask}, params, 8,
                                              [](int, const QString &) { return true; },
                                              &error);
    ASSERT_TRUE(patches.has_value()) << error.toStdString();
    ASSERT_EQ(patches->size(), 1);
    const PatchResult &patch = patches->first();
    EXPECT_FALSE(patch.crop.isEmpty());
    EXPECT_TRUE(QRect(0, 0, 320, 240).contains(patch.crop));
    EXPECT_EQ(patch.rgba.format(), QImage::Format_ARGB32);

    // 遮罩中心 alpha=255；遮罩外 alpha=0；边界有渐变。
    const QPoint center = patch.crop.center() - patch.crop.topLeft();
    EXPECT_EQ(qAlpha(patch.rgba.pixel(center)), 255);
    EXPECT_EQ(qAlpha(patch.rgba.pixel(0, 0)), 0);
}

TEST(InpaintPipeline, ChoosesSmallerTileWhenVramBudgetIsTight)
{
    EXPECT_EQ(InpaintPipeline::chooseTileSize(512, 512, 4ll * 1024 * 1024 * 1024), 512);
    EXPECT_LE(InpaintPipeline::chooseTileSize(512, 512, 512ll * 1024 * 1024), 384);
    EXPECT_LE(InpaintPipeline::chooseTileSize(512, 512, 128ll * 1024 * 1024), 256);
}

TEST(InpaintPipeline, EmptyMaskProducesNoPatch)
{
    QImage frame(64, 64, QImage::Format_RGB32);
    frame.fill(Qt::black);
    BinaryMask mask;
    mask.width = mask.height = 64;
    mask.bits.assign(64 * 64, 0);
    InpaintPipeline pipeline;
    pipeline.setInpainterForTest(std::make_unique<SolidInpainter>());
    QString error;
    const auto patches = pipeline.repairChunk({frame}, {mask}, RemovalParameters{}, 4,
                                              [](int, const QString &) { return true; },
                                              &error);
    ASSERT_TRUE(patches.has_value()) << error.toStdString();
    EXPECT_TRUE(patches->isEmpty());
}
```

`tests/ai/TemporalStabilizerTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QColor>
#include "TemporalStabilizer.h"

using namespace VideoEditor;

namespace {
qreal temporalVariation(const QList<QImage> &frames, const QRect &hole)
{
    qreal total = 0.0;
    int count = 0;
    for (int i = 1; i < frames.size(); ++i) {
        for (int y = hole.top(); y <= hole.bottom(); ++y) {
            for (int x = hole.left(); x <= hole.right(); ++x) {
                const QColor a = frames[i - 1].pixelColor(x, y);
                const QColor b = frames[i].pixelColor(x, y);
                total += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green())
                       + qAbs(a.blue() - b.blue());
                count += 3;
            }
        }
    }
    return count == 0 ? 0.0 : total / count;
}
} // namespace

TEST(TemporalStabilizer, ReducesFrameToFrameVariationInHole)
{
    QList<QImage> patches;
    QList<QRect> crops;
    QList<QPoint> centroids;
    for (int i = 0; i < 5; ++i) {
        QImage image(64, 64, QImage::Format_ARGB32);
        image.fill(QColor(40, 40, 40, 255));
        const int offset = (i % 2 == 0) ? -12 : 12;
        for (int y = 20; y < 44; ++y)
            for (int x = 20; x < 44; ++x)
                image.setPixelColor(x, y, QColor(128 + offset, 128, 128, 255));
        patches.append(image);
        crops.append(QRect(0, 0, 64, 64));
        centroids.append(QPoint(32, 32));
    }
    const qreal before = temporalVariation(patches, QRect(20, 20, 24, 24));
    TemporalStabilizer::stabilize(patches, crops, centroids, 100);
    const qreal after = temporalVariation(patches, QRect(20, 20, 24, 24));
    EXPECT_LT(after, before * 0.8);
}
```

`temporalVariation` 是测试内辅助函数（对每对相邻帧的洞内像素求平均绝对差）。

- [ ] **Step 2: 实现几何、羽化、颜色匹配**

`InpaintPipeline::repairChunk` 算法（固定顺序）：
1. 若所有 mask 为空 → 返回空列表。
2. 计算 chunk 内 mask 并集包围盒 `bbox`；`margin = max(64, bbox.width()/4, bbox.height()/4)`；`crop = bbox.adjusted(-margin,-margin,margin,margin)`；`crop` 四边 clamp 到帧边界；宽高向上取整到 8 的倍数，再 clamp。
3. 对每帧：裁剪 `crop` 得到 `cropImage`；构造 `maskCrop`（尺寸与 crop 相同）；若该帧 mask 为空，跳过修复，输出 alpha 全 0 的 crop。
4. 选择 inpainter：`params.mode==Fast` → MI-GAN；`Quality` → LaMa。
5. 修复结果转 `ARGB32`（直通 alpha；`LayerCompositor` 使用 `SRC_ALPHA/INV_SRC_ALPHA`，不能写预乘 alpha）；用 `MaskEditor::toOverlay` 类似的距离变换生成 alpha：
   - `feather = clamp(featherPx, 0, 64)`；对 mask 内像素到最近 mask 外像素的 chamfer 距离 `d`；`alpha = feather==0 ? 255 : clamp(255*d/feather, 0, 255)`；mask 外 alpha=0。引擎按 chunk 起点最近的关键帧取 `featherPx`，并在 chunk 内保持不变，避免帧间羽化跳变。
6. 颜色匹配：取 mask 外 8px 环带（在 crop 内、mask 外）计算源图与修复图的每通道均值；`out = clamp(patch + (meanSource - meanPatch))`；若环带像素 < 32，跳过。
7. 返回 `PatchResult{rgba, crop}`。

`MiGanInpainter::repair`：
- 输入 crop 转 RGB float32 `[1,3,H,W]`，值域 `[0,1]`；mask 转 `[1,1,H,W]` float32（0/1）；尺寸若不是 8 的倍数，reflect pad 到 8 的倍数，输出裁回。
- 输出 `[1,3,H,W]`，clamp `[0,1]`，转回 RGB；alpha 由上层加。
- `LamaInpainter::repair`：crop+mask 用 reflect pad 到 `512×512`（若 crop 大于 512，按 512 tile + 64px overlap，逐 tile 修复后羽化混合）；输出 resize 回 crop；其余同上。

- [ ] **Step 3: 实现时域稳定化**

`TemporalStabilizer::stabilize`：
- 对每个 patch，取前后各 2 帧（窗口 5）；用 `centroids[i] - centroids[j]` 平移邻居 patch，使目标中心对齐。
- 在洞内（alpha>0）对 5 个样本逐像素取中值；计算 `mad = mean(|sample - median|)`；若 `mad > 12/255` 则认为邻居不可信，不混合。
- 混合强度 `blend = clamp(strength/100.0 * 0.6, 0, 0.6)`；`out = (1-blend)*patch + blend*median`，保持 alpha。
- 颜色匹配在稳定化之后再做一次（Task 6 Step 2 的环带算法），避免中值改变色偏。
- 该算法只做平移对齐，不做光流；文档/UI 明确“快速运动效果有限”。

- [ ] **Step 4: 接入子进程并写模型依赖测试**

`AiEngineMain.cpp` 在 Task 4 的协议骨架上：
1. 读 manifest，`ModelRegistry::load`；若 gate 关闭，发 `error model_unavailable`。
2. 读源帧（`FrameReader`，FFmpeg 软件解码）；若 `spec.redoFirstFrame < 0`，先跑 tracking，写 mask/置信度；否则从 `cacheDir/tracking/masks` 读已有 mask。
3. 若 `spec.trackingOnly`：只写 mask/置信度，发 `done`（`completedFrames=0`、`confidenceHash`）后退出，不进入修复。
4. 按 30 帧 chunk 调 `repairChunk`；每个 chunk 用与父进程 `RemovalCache::writeChunk` 相同的目录/命名/哈希算法写 PNG。
5. 发 `chunkDone`、`progress`；写完 manifest（`status="complete"` 或取消时 `"partial"`）；发 `done`/`cancelled`。

模型质量（PSNR/硬边/闪烁）不在单元测试里伪造：由 Task 9 的 `--selftest-removal` 在固定 fixtures 上跑真实 MI-GAN/LaMa，并把洞内 PSNR、时域差分、失败分类写进验收报告。本任务的自动化测试只覆盖几何、alpha、颜色匹配、时域稳定化与 VRAM 预算，这些都不需要模型。

- [ ] **Step 5: 运行测试并提交**

`tests/CMakeLists.txt` 的 `ai_tests` 源列表追加 `ai/InpaintPipelineTest.cpp`、`ai/TemporalStabilizerTest.cpp`（Task 5 只加了 Confidence/Tracking）。

Run:
`cmake --build --preset windows-x64-release --target ai_tests && ctest --preset windows-x64-release -R "InpaintPipeline|TemporalStabilizer" --output-on-failure`
Expected: 几何/羽化/稳定化测试 PASS；模型测试 SKIP（未安装模型）。

```bash
git add src/ai/engine src/ai/AiEngineMain.cpp tests/ai/InpaintPipelineTest.cpp tests/ai/TemporalStabilizerTest.cpp tests/CMakeLists.txt
git commit -m "feat(ai): add MI-GAN/LaMa inpainting and temporal stabilization"
```

---

### Task 7: RepairFrameSource、合成器集成、预览对比与导出 preflight

**Files:**
- Create: `src/media/render/RepairFrameSource.h/.cpp`
- Modify: `src/media/render/TimelineCompositor.h/.cpp`
- Modify: `src/media/render/LayerCompositor.h/.cpp`（scissor）
- Modify: `src/core/TimelineEvaluator.h/.cpp`（`VideoLayer::removalLayerId`）
- Modify: `src/media/render/TimelineRenderWorker.h/.cpp`
- Modify: `src/ui/PreviewController.h/.cpp`
- Create: `src/ai/RemovalPreflight.h/.cpp`（依赖 `videoeditor_media` 的 `RemovalCache`/`fingerprintFile`；因此放在 `videoeditor_ai`，避免 media → ai 的反向依赖）
- Modify: `src/media/export/ExportPipeline.h/.cpp`
- Modify: `src/ui/ExportController.h/.cpp`
- Create: `tests/media/RepairSourceTest.cpp`
- Create: `tests/media/RemovalRenderTest.cpp`
- Create: `tests/ai/RemovalPreflightTest.cpp`
- Create: `tests/media/RemovalExportTest.cpp`
- Modify: `src/media/CMakeLists.txt`、`tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `RemovalCache`、`RemovalCacheManifest`、`RemovalLayer`、`Project`、`TimelineCompositor`、`LayerCompositor`、`ExportPipeline`。
- Produces:
  - `struct VideoLayer` 增加 `QString removalLayerId;`。
  - `RepairFrameSource`：`open(const RemovalCache&, const QString &cacheKey, QString *error=nullptr)`、`frameAt(int64_t sourceUs, QString *error=nullptr) -> const Frame*`、`isComplete()`、`frameCount()`；`Frame { QImage patch; QRect crop; int sourceWidth; int sourceHeight; }`。
  - `enum class RemovalCompareMode { Repair, Original, SplitWipe }`；`TimelineCompositor::setRemovalCompareMode(mode, int splitPermille=500)`、`setFailOnMissingRemoval(bool)`、`removalLayerCountForTest()`。
  - `LayerCompositor::setScissorRect(const RECT*)`。
  - `RemovalPreflight::check(const Project&, const RemovalCache&, const ModelRegistry&) -> RemovalPreflightResult`。
  - `ExportController::setRemovalChecker(std::function<RemovalPreflightResult(const Project&)>)`、`signal removalGenerationRequired(QString message)`。
  - `PreviewController::setRemovalCompareMode(const QString&, int splitPermille)`。

- [ ] **Step 1: 写失败的 RepairFrameSource 测试**

`tests/media/RepairSourceTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "RepairFrameSource.h"

using namespace VideoEditor;

TEST(RepairFrameSource, MapsSourceTimeToPatchAndCrop)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('a'));
    // 写 3 帧 16x16 补丁，crop=(4,4,16,16)，manifest complete。
    // 1) cache.writeChunk(...); 2) cache.writeManifest(...)。

    auto source = RepairFrameSource::open(cache, key);
    ASSERT_TRUE(source);
    QString error;
    const auto *frame = source->frameAt(0, &error);
    ASSERT_TRUE(frame) << error.toStdString();
    EXPECT_EQ(frame->crop, QRect(4, 4, 16, 16));
    EXPECT_EQ(frame->sourceWidth, 64);
    EXPECT_EQ(frame->sourceHeight, 64);
    EXPECT_FALSE(frame->patch.isNull());
}

TEST(RepairFrameSource, ReturnsNullForPartialOrMissingCache)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    const QString key(64, QLatin1Char('b'));
    QString error;
    EXPECT_FALSE(RepairFrameSource::open(cache, key, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("缺失")));
}
```

- [ ] **Step 2: 实现 `RepairFrameSource`**

```cpp
class RepairFrameSource {
public:
    struct Frame {
        QImage patch;
        QRect crop;
        int sourceWidth = 0;
        int sourceHeight = 0;
    };
    static std::unique_ptr<RepairFrameSource> open(
        const RemovalCache &cache, const QString &cacheKey,
        QString *error = nullptr);
    const Frame *frameAt(int64_t sourceUs, QString *error = nullptr);
    bool isComplete() const { return m_complete; }
    int frameCount() const { return m_manifest.totalFrames; }
private:
    RepairFrameSource(RemovalCache cache, RemovalCacheManifest manifest);
    RemovalCache m_cache;
    RemovalCacheManifest m_manifest;
    QHash<int, RemovalChunkFrame> m_locations;
    QCache<int, Frame> m_frames{12};
    bool m_complete = false;
};
```

`open`：`state(key)==Complete`，`manifest.status=="complete"`；加载所有 chunk index 到 `m_locations`；失败返回中文 `"AI 修复缓存缺失或损坏：%1"`。
`frameAt`：`frame = floor((sourceUs - manifest.sourceInUs) * fps.num / (1e6 * fps.den))`，clamp 到 `[0,totalFrames-1]`；查 `m_locations`；`readFrameImage`；放入 `QCache`；返回指针。若 `sourceUs` 不在 `[sourceInUs,sourceOutUs]`，返回 nullptr（调用方画原片）。

- [ ] **Step 3: 写失败的合成/对比测试**

`tests/media/RemovalRenderTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "TimelineCompositor.h"

using namespace VideoEditor;

TEST(RemovalRender, DrawsRepairOverOriginalAndCompareModesDiffer)
{
    // 用 tests/fixtures/h264.mp4 作为原片（或生成纯色 64x64 fixture）；
    // 修复缓存补丁纯红、全 alpha。
    // 1) OfflineFrameRenderer/TimelineCompositor 画 frame 0。
    // 2) compare=Repair：中心像素接近红；compare=Original：中心像素接近原片。
    // 3) compare=SplitWipe，split=500：左半红、右半原片。
}

TEST(RemovalRender, OutsideLayerRangeDrawsOriginal)
{
    // 层 sourceIn=1s/sourceOut=2s；在 0.5s 渲染时中心应为原片蓝，
    // 在 1.5s 渲染时中心应为补丁红。缩进/分割后仍按源区间判断。
}

TEST(RemovalRender, MissingCacheFallsBackToOriginalAndWarnsOnce)
{
    // 删除缓存文件；drawFrame 返回正常、center 为蓝；warning 计数 == 1。
}

TEST(RemovalRender, FailOnMissingRemovalReturnsError)
{
    // setFailOnMissingRemoval(true)；drawFrame 返回 -1，error 非空且包含“缓存”。
}
```

- [ ] **Step 4: 修改评估器与合成器**

`TimelineEvaluator.h`：

```cpp
struct VideoLayer {
    // ... existing fields ...
    QString removalLayerId; // empty = no repair
};
```

`TimelineEvaluator::frameAt` 在填充 `VideoLayer` 后：

```cpp
const RemovalLayer *removal = m_project.removalLayerById(active->removalLayerId);
if (removal && removal->enabled
    && layer.sourceTimeUs >= removal->sourceInUs
    && layer.sourceTimeUs < removal->sourceOutUs)
    layer.removalLayerId = removal->id;
```

`TimelineCompositor` 增加成员 `std::map<QString, std::unique_ptr<RepairFrameSource>> m_repairSources;`、`RemovalCompareMode m_compare=Repair; int m_splitPermille=500; bool m_failOnMissingRemoval=false;`。`open()` 与 `close()` 必须清空 `m_repairSources`（项目快照/缓存键变化后旧 source 不可复用），并保留 `m_compare`/`m_splitPermille`（预览切换不因编辑重置）。
`repairSourceFor(const RemovalLayer&, QString*)`：key 用 `layer.id + "|" + layer.cacheKey`；打开 `RepairFrameSource`；失败时 `reportOnce("removal:" + layer.id, "AI 修复结果缺失，当前显示原片；请重新生成。")`；若 `m_failOnMissingRemoval` 则设置错误并返回 nullptr（drawFrame 返回 -1）。

绘制循环中，在原片 `drawFrame` 成功后：

```cpp
if (!layer.removalLayerId.isEmpty() && m_compare != RemovalCompareMode::Original) {
    const RemovalLayer *removal = m_project.removalLayerById(layer.removalLayerId);
    if (removal && removal->enabled) {
        if (RepairFrameSource *repair = repairSourceFor(*removal, &layerError)) {
            const RepairFrameSource::Frame *patch = repair->frameAt(layer.sourceTimeUs, &layerError);
            if (patch) {
                const LayerRect patchRect =
                    insetLayerRectForCrop(layerRect, patch->crop,
                                          patch->sourceWidth, patch->sourceHeight);
                if (m_compare == RemovalCompareMode::SplitWipe) {
                    RECT left{0, 0, LONG(m_width * m_splitPermille / 1000), LONG(m_height)};
                    m_compositor->setScissorRect(&left);
                }
                if (m_compositor->drawImage(patch->patch, patchRect, opacity, &layerError))
                    ++drawnLayers;
                if (m_compare == RemovalCompareMode::SplitWipe)
                    m_compositor->setScissorRect(nullptr);
            }
        }
    }
}
```

`insetLayerRectForCrop` 是 `TimelineCompositor.cpp` 内部函数：`x' = rect.x + rect.w * crop.x/sourceW`，`w' = rect.w * crop.width/sourceW`，y/h 同理。

`LayerCompositor` 增加：
```cpp
void setScissorRect(const RECT *rect);
```
实现：`m_scissor` 默认 `{0,0,m_width,m_height}`；`begin` 重置；`applyDrawState` 中 `RSSetScissorRects(1, &m_scissor)`；光栅化状态 `ScissorEnable=TRUE`。

`TimelineRenderWorker` 增加槽 `setRemovalCompare(int mode, int splitPermille)`（用 int 避免 QML 枚举元类型），转发给 compositor；`PreviewController` 增加 `Q_INVOKABLE void setRemovalCompareMode(const QString &mode, int splitPermille)`，映射 `"repair"|"original"|"split"`；预览改模式后请求当前帧重绘。

- [ ] **Step 5: 写失败的导出 preflight 测试**

`tests/ai/RemovalPreflightTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "RemovalPreflight.h"
#include "RemovalTestProject.h"

using namespace VideoEditor;

TEST(RemovalPreflight, MissingAndStaleCacheAreReportedBeforeExport)
{
    QTemporaryDir dir;
    RemovalCache cache(dir.path());
    ModelManifest modelManifest; // approved + 固定 tracking/inpaint 模型哈希
    modelManifest.commercialApproved = true;
    modelManifest.legalSignOffBy = QStringLiteral("legal");
    modelManifest.legalSignOffDate = QStringLiteral("2026-11-15");
    modelManifest.legalScope = QStringLiteral("commercial");
    modelManifest.models.append(AiModelRecord{
        QStringLiteral("sam2.1-hiera-tiny"), QStringLiteral("tracking"),
        QStringLiteral("1.0.0"), QStringLiteral("Apache-2.0"),
        QStringLiteral("Apache-2.0"), {}, true, {{QStringLiteral("image_encoder"),
        AiModelFile{QStringLiteral("unused.onnx"), QString(64, QLatin1Char('a'))}}}, {}});
    modelManifest.models.append(AiModelRecord{
        QStringLiteral("migan-512"), QStringLiteral("inpaint"),
        QStringLiteral("1.0.0"), QStringLiteral("MIT"),
        QStringLiteral("MIT"), {}, true, {{QStringLiteral("model"),
        AiModelFile{QStringLiteral("unused.onnx"), QString(64, QLatin1Char('b'))}}}, {}});
    const ModelRegistry registry = ModelRegistry::forTest(modelManifest);
    Project project = projectWithClip();
    const RemovalLayer layer = sampleLayer();
    project.removalLayers.append(layer);
    project.tracks[0].clips[0].removalLayerId = layer.id;

    RemovalPreflightResult result = RemovalPreflight::check(project, cache, registry);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.issues.size(), 1);
    EXPECT_TRUE(result.message.contains(QStringLiteral("缺失或已过期")));
    EXPECT_EQ(result.issues.first().clipId, QStringLiteral("clip-1"));
}

TEST(RemovalPreflight, AcceptsCompleteCacheAndRejectsKeyMismatch)
{
    // 写完整 cache + 匹配 cacheKey -> ok。
    // 改 project.removalLayers[0].cacheKey -> 仍报告过期。
}
```

`tests/media/RemovalExportTest.cpp`：

```cpp
TEST(RemovalExport, FailsBeforeWritingOutputWhenCacheMissing)
{
    // ExportPipeline::run 带一个 missing removal 层；返回失败；
    // 输出文件与 .parts 目录都不存在。
}

TEST(RemovalExport, PreviewAndExportMatchWithFixedCache)
{
    // 同一帧通过 preview compositor 与 OfflineFrameRenderer 渲染；
    // RGBA 逐像素差 <= 1 LSB（或完全相同）。
}

TEST(RemovalExport, MissingCacheMidExportAborts)
{
    // 渲染 1 帧后删除 cache；OfflineFrameRenderer 返回错误，
    // ExportPipeline 失败，不输出原片。
}
```

- [ ] **Step 6: 实现 `RemovalPreflight`**

```cpp
struct RemovalPreflightIssue {
    QString clipId;
    QString layerId;
    int64_t startUs = 0;
    int64_t endUs = 0;
    QString reason;
};
struct RemovalPreflightResult {
    bool ok = true;
    QList<RemovalPreflightIssue> issues;
    QString message;
};

class RemovalPreflight {
public:
    static RemovalPreflightResult check(const Project &project,
                                        const RemovalCache &cache,
                                        const ModelRegistry &models);
};
```

实现：
- 先检查 `models.featureEnabled(&reason)`；未启用时所有带消除层的片段都记 issue，reason 使用 gate 的中文原因，避免在模型未安装时把问题误报成“缓存过期”。
- 遍历启用视频轨/片段；`clip.removalLayerId` 为空跳过。
- 取 layer；`!layer->enabled` 跳过。
- `trackingState` 为 `Tracking`/`Partial`/`Failed` → issue `"AI 跟踪尚未完成。"`；`LowConfidence` 不阻止导出，但 `message` 追加低置信提醒。
- `asset = project.assetById(clip.assetId)`；缺素材 → issue `"素材缺失"`。
- 构造 `RemovalKeyInput`：`assetFingerprint = fingerprintFile(asset->path)`、`sourceIn/Out`、`maskKeyframes`、`parameters`、`trackingModelId/hash`、`inpaintModelId/hash`（从 `models` 默认模型取）、`pipelineVersion`（manifest）、`colorSpace`、`fps`（asset.info.frameRate，非法则 timeline.fps）、`width/height`（`displaySizeForAsset(*asset)`）。
- `key = computeRemovalCacheKey(input)`；`key != layer->cacheKey` → issue reason `"AI 修复结果已过期（素材、遮罩、参数或模型已变化）。"`。
- `cache.verify(key)` 失败 → issue reason `"AI 修复结果缺失或已损坏。"`。
- `result.ok = issues.isEmpty()`；`message` 汇总为 `"片段“%1”的 AI 修复结果缺失或已过期。请先生成修复，再导出。"`（多个片段用换行）。

- [ ] **Step 7: 接入 ExportPipeline/ExportController**

`ExportPipeline::Hooks` 增加 `std::function<RemovalPreflightResult(const Project&)> removalPreflight;`。`ExportPipeline::run` 在创建任何输出文件/分段前调用；失败返回 `ExportResult{ok=false, message=...}`。

`OfflineFrameRenderer::open` 在 `TimelineCompositor::open` 成功后立即设置：

```cpp
m_compositor->setRemovalCompareMode(RemovalCompareMode::Repair);
m_compositor->setFailOnMissingRemoval(true);
```

`ExportPipeline` 的每个 `renderFrame` 返回 false/错误时立即中止并保留已完成分段；若错误来自修复缓存（包含“AI 修复缓存”），错误信息必须原样上报，禁止回退原片继续编码。

`ExportController` 增加：
```cpp
void setRemovalChecker(std::function<RemovalPreflightResult(const Project&)> checker);
Q_PROPERTY(bool removalRequired READ removalRequired NOTIFY removalRequiredChanged)
Q_PROPERTY(QString removalMessage READ removalMessage NOTIFY removalMessageChanged)
signals: void removalGenerationRequired(QString message);
```
`validate()` 在现有设置/编码检查后调用 checker；若失败把 `removalMessage` 设为预检消息并返回它。`startExport()` 在 `validate()` 失败时，如果 `removalRequired` 为真则 emit `removalGenerationRequired(message)`，不启动 worker。`ExportController` 不直接持有 cache/model；由 `AppController` 注入 lambda。

`AppController` 构造中：
```cpp
m_export->setRemovalChecker([this](const Project &project) {
    return RemovalPreflight::check(project, *m_removalCache, *m_gate->registry());
});
```
并连接 `removalGenerationRequired` 到 `RemovalController::beginGenerateForExport()`（Task 8 完成 UI）。

`AppController` 必须把 `ProjectDocument::removalLayersChanged` 接到已有的 40 ms `m_timelineUpdateTimer`（与 `timelineChanged/tracksChanged` 相同），否则启用/禁用/删除层后预览不会刷新。`PreviewController::updateTimelineProject` 会把带 `removalLayerId` 的快照推给 compositor；`TimelineRenderWorker::setProject` 必须保留 compare mode。

- [ ] **Step 8: 运行测试并提交**

`tests/CMakeLists.txt` 的 `ai_tests` 源列表追加 `ai/RemovalPreflightTest.cpp`；`src/ai/CMakeLists.txt` 增加 `RemovalPreflight.h/.cpp`。

Run:
`cmake --build --preset windows-x64-release --target render_tests media_tests export_tests ai_tests && ctest --preset windows-x64-release -R "Repair|Removal" --output-on-failure`
Expected: `RepairSource.*`、`RemovalRender.*`、`RemovalPreflight.*` PASS；`RemovalExport.*` 在无 GPU 时按既有模式 SKIP 或使用软件路径；`OfflineRendererTest`、`ExportParityTest`、`ExportPipelineTest` 无回归。

```bash
git add src/media/render/RepairFrameSource.h src/media/render/RepairFrameSource.cpp src/media/render/TimelineCompositor.h src/media/render/TimelineCompositor.cpp src/media/render/LayerCompositor.h src/media/render/LayerCompositor.cpp src/core/TimelineEvaluator.h src/core/TimelineEvaluator.cpp src/media/render/TimelineRenderWorker.h src/media/render/TimelineRenderWorker.cpp src/ui/PreviewController.h src/ui/PreviewController.cpp src/ai/RemovalPreflight.h src/ai/RemovalPreflight.cpp src/ai/CMakeLists.txt src/media/export/ExportPipeline.h src/media/export/ExportPipeline.cpp src/ui/ExportController.h src/ui/ExportController.cpp src/app/AppController.h src/app/AppController.cpp src/media/CMakeLists.txt tests/media/RepairSourceTest.cpp tests/media/RemovalRenderTest.cpp tests/ai/RemovalPreflightTest.cpp tests/media/RemovalExportTest.cpp tests/CMakeLists.txt
git commit -m "feat(render): draw cached removal patches and gate export preflight"
```

---

### Task 8: UX 收敛（置信度、进度、局部重做、错误文案）

**Files:**
- Create: `src/app/ConfidenceBand.qml`
- Create: `src/app/JobProgressPanel.qml`
- Modify: `src/app/RemovalPanel.qml`、`MaskEditorOverlay.qml`、`TimelineClipDelegate.qml`、`Main.qml`、`ExportDialog.qml`
- Modify: `src/ui/RemovalController.h/.cpp`
- Create: `src/core/RemovalErrors.h/.cpp`
- Create: `tests/ui/RemovalControllerProgressTest.cpp`
- Create: `tests/app/RemovalQmlTest.cpp`
- Modify: `tests/CMakeLists.txt`、`src/app/CMakeLists.txt`、`src/ui/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 4 `AiJobManager`、Task 5 置信度 sidecar、Task 2 `RemovalCache::invalidateFrameRange`、Task 7 export signal。
- Produces:
  - `RemovalController` 增加 Q_PROPERTY：`featureEnabled`、`featureDisabledReason`、`generating`、`paused`、`progress`、`stageText`、`etaText`、`confidenceBand`、`lowConfidenceCount`、`cacheStatusText`、`advancedOpen`、`compareMode`、`splitPermille`。
  - Q_INVOKABLE：`track()`、`repropagateFromCurrentKeyframe()`、`generate()`、`cancel()`、`pause()`、`resume()`、`jumpToConfidence(int)`、`redoRange(int64_t,int64_t)`、`setCompareMode(QString)`、`setSplitPermille(int)`、`setAdvancedMode(QString)`、`setStabilizationStrength(int)`、`setProcessingHeight(int)`、`generateForExport()`。
  - `removalErrorMessage(const QString &code, const QString &detail) -> QString`。
  - QML objectName：`confidence-band`、`confidence-low-jump`、`job-progress-panel`、`job-progress-bar`、`job-cancel-button`、`job-pause-button`、`removal-track-button`、`removal-generate-button`、`removal-compare-toggle`、`removal-advanced-toggle`、`removal-advanced-panel`、`removal-low-confidence-count`。

- [ ] **Step 1: 写失败的错误文案/局部重做测试**

`tests/ui/RemovalControllerProgressTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include "RemovalErrors.h"

using namespace VideoEditor;

TEST(RemovalErrors, MapsCodesToChineseActionableMessages)
{
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("vram"), QString())
                    .contains(QStringLiteral("显存不足")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("device_lost"), QString())
                    .contains(QStringLiteral("设备已重置")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("model_missing"), QString())
                    .contains(QStringLiteral("重新安装")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("disk_full"), QString())
                    .contains(QStringLiteral("磁盘空间不足")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("low_confidence"), QString())
                    .contains(QStringLiteral("置信度低")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("cancelled"), QString())
                    .contains(QStringLiteral("已取消")));
    EXPECT_TRUE(removalErrorMessage(QStringLiteral("unknown"), QStringLiteral("x"))
                    .contains(QStringLiteral("x")));
}

TEST(RemovalControllerProgress, PartialRedoInvalidatesOnlyIntersectingChunks)
{
    // 3 个 chunk，每 chunk 30 帧；redoRange 覆盖第 40..50 帧。
    // 期望 chunk 1 被删除，chunk 0/2 保留；manifest status == partial。
}

TEST(RemovalControllerProgress, CancelKeepsCompletedChunksAndMarksPartial)
{
    // FakeTransport 完成 1 个 chunk 后 cancel；
    // 期望 layer.trackingState == Partial，缓存 chunk 0 存在，error 为空/提示可继续。
}

TEST(RemovalControllerProgress, CrashRecoveryOffersResume)
{
    // 预置 manifest status=partial；构造 controller 后
    // cacheStatusText 包含“继续生成”，lowConfidenceCount 与 sidecar 一致。
}
```

- [ ] **Step 2: 实现错误映射**

`RemovalErrors.h/.cpp`：

```cpp
QString removalErrorMessage(const QString &code, const QString &detail)
{
    if (code == QStringLiteral("vram"))
        return QStringLiteral("显存不足，已降低处理分辨率并分块处理。预计耗时增加。可在高级设置中进一步降低分辨率。");
    if (code == QStringLiteral("device_lost"))
        return QStringLiteral("显卡设备已重置，AI 任务已中断。已完成的分段已保留，可重试或改用 CPU。");
    if (code == QStringLiteral("model_missing"))
        return QStringLiteral("AI 模型文件缺失或校验失败，请重新安装应用或修复模型包。");
    if (code == QStringLiteral("cache_missing"))
        return QStringLiteral("AI 修复结果缺失或已过期。请先生成修复，再导出。");
    if (code == QStringLiteral("low_confidence"))
        return QStringLiteral("跟踪置信度低，请修正遮罩后从此帧重新传播。");
    if (code == QStringLiteral("disk_full"))
        return QStringLiteral("磁盘空间不足，已停止写入并保留进度。请清理缓存或更换缓存目录。");
    if (code == QStringLiteral("cancelled"))
        return QStringLiteral("已取消，已完成的分段已保留，可继续生成。");
    return detail.isEmpty() ? QStringLiteral("AI 任务失败，请重试或查看日志。") : detail;
}
```

- [ ] **Step 3: 实现置信度条与低置信跳转**

`RemovalController` 在图层加载/跟踪完成后读取 `sidecars.readTracking(layer.trackingConfidenceHash)`，构造：
```cpp
QVariantList confidenceBand() const; // [{timeUs,state,color,sourceTimeUs}]
int lowConfidenceCount() const;     // state == low/missing 的数量
```
`color` 用 `#4CAF50`/`#FFC107`/`#F44336`/`#9E9E9E`。`jumpToConfidence(index)` 把播放头设到对应时间线的 `clip.timelineStartUs + unscaled source offset`，并暂停播放。

`ConfidenceBand.qml`：一个高 6px 的 Row/Canvas，`objectName: "confidence-band"`；每段宽度按时间比例；点击发 `Removal.jumpToConfidence(i)`；`confidence-low-jump` 按钮显示“第 N 段低置信，跳到该帧”。

- [ ] **Step 4: 实现进度/暂停/取消/恢复**

`RemovalController::track()`：与 `generate()` 使用同一 `AiJobSpec` 构造，但设 `trackingOnly=true`、`cacheDir=cache.root()+"/tracking/"+layer.id`（跟踪中间结果，不参与修复 manifest）；引擎只跑跟踪、写 mask/置信度 sidecar，不写修复 chunk。`jobFinished` 后更新 `trackingState` 与 `trackingConfidenceHash`，再让用户点“生成修复”。

`RemovalController::generate()`：
1. 检查 gate、layer、关键帧、播放头在层区间；失败设置 `error`。
2. 先算缓存键：用 `RemovalKeyInput{assetFingerprint, sourceIn/Out, maskKeyframes, parameters, trackingModelId/hash, inpaintModelId/hash, pipelineVersion, colorSpace, fps=asset.info.frameRate, width/height=displaySizeForAsset}` 调 `computeRemovalCacheKey`；若与 `layer.cacheKey` 不同，更新 `layer.cacheKey/cachePath` 并 push `RemovalLayerCommand`（保证导出 preflight 与生成使用同一键）。
3. 构造 `AiJobSpec`：`jobId=QUuid`、`assetPath`、`sidecarRoot`、`cacheDir=cache.cacheDir(layer.cacheKey)`、`cacheKey`、`modelsManifestPath`、`trackingModelId/inpaintModelId`（registry 默认）、源区间、`width/height`（`displaySizeForAsset(*asset)`）、`fps`（asset 帧率）、`maskKeyframes`、`parameters`。
4. `jobs.start(spec, &error)`；`generating=true`；连接 `jobFinished`。
5. 进度属性直接绑定 `AiJobManager` 的 `progress/stageText/etaText`；`stageText` 中文映射 `tracking→"正在跟踪遮罩"`、`inpainting→"正在修复画面"`、`writing→"正在写入缓存"`。
6. `jobFinished`：
   - `ok`：写 manifest `complete`；更新 layer `trackingState=Complete`、`trackingConfidenceHash`、`cacheKey`、`cachePath`、`revision++`；push `RemovalLayerCommand`；通过 `PreviewController` 刷新；内联提示 `"AI 修复已完成。"`。
   - `cancelled`：manifest `partial`；layer `Partial`；提示 `removalErrorMessage("cancelled", {})`。
   - `error`：manifest `failed`；layer `Failed`；`error=removalErrorMessage(code, message)`。
7. `pause/resume` 转发 `AiJobManager`；`cancel` 转发并保留 chunk。
8. `generateForExport()` 设置 `m_exportAfterGeneration=true`；完成后 emit `generationFinishedForExport()`，`AppController` 连接后调用 `Export.startExport()`；失败清除标记，不导出。

- [ ] **Step 5: 实现局部重做**

`RemovalController::redoRange(sourceStartUs, sourceEndUs)`：
- 取 layer，clamp 到 `[layer.sourceInUs, layer.sourceOutUs]`；若范围为空返回。
- `firstFrame = floor((start - sourceInUs) * fps.num / 1e6)`；`lastFrame = ceil((end - sourceInUs) * fps.num / 1e6)`。
- `firstChunk = max(0, firstFrame/30 - 1)`；`lastChunk = min(chunkCount-1, lastFrame/30 + 1)`（多带一个 chunk 以覆盖时域窗口）。
- `cache.invalidateFrameRange(layer.cacheKey, firstChunk*30, (lastChunk+1)*30-1)`。
- 构造 `AiJobSpec` 设 `redoFirstFrame=firstChunk*30`、`redoLastFrame=(lastChunk+1)*30-1`、`resume=true`；启动 job；完成后再写 manifest `complete`。
- 该操作推一个命令（只改 layer 的 revision/cacheKey 不变；若 cacheKey 不变，命令只记录 revision/状态，撤销恢复旧 revision）。

- [ ] **Step 6: 高级面板与对比控件**

`RemovalPanel.qml` 高级区（`removal-advanced-panel`）：
- 模型：`快速（MI-GAN）` / `高质量（LaMa）`，默认 `快速`；对应 `setAdvancedMode("fast"|"quality")`。
- 处理分辨率：`540p / 720p / 1080p`，默认 `1080p`（或源高度）；`setProcessingHeight(int)`。
- 时域稳定化：`关 / 弱 / 标准 / 强`，默认 `标准`（100→强、60→标准、30→弱、0→关）；`setStabilizationStrength(int)`。
- chunk 大小只读显示 `30 帧`（M4 固定，避免用户误设导致性能问题）。
- 对比：`对比原片` 按住按钮（`removal-compare-toggle`）或 `分割对比` 切换；`setCompareMode("original"|"repair"|"split")`；`splitPermille` Slider 0–1000。
- 置信度条、缓存状态、低置信计数始终可见；`removal-low-confidence-count` 显示“低置信 N 帧”。

`JobProgressPanel.qml`：`objectName: "job-progress-panel"`，显示阶段、百分比、ETA、暂停/继续、取消；生成中时间线仍可编辑（`AiJobManager` 在 UI 线程只处理消息，不阻塞）。

- [ ] **Step 7: 导出缺缓存提示**

`ExportDialog.qml` 在 `Export.removalRequired` 为真时显示红色内联提示 `Export.removalMessage` 和两个按钮：
- `objectName: "export-generate-removal"`，文本 `先生成再导出`，`onClicked: App.removal.generateForExport()`。
- `objectName: "export-cancel-removal"`，文本 `取消`，关闭提示。
`AppController` 连接 `RemovalController::generationFinishedForExport` 到 `Export.startExport()`；若生成失败，保留对话框并显示错误。

- [ ] **Step 8: 运行测试并提交**

Run:
`cmake --build --preset windows-x64-release --target ui_tests app_qml_tests && ctest --preset windows-x64-release -R "RemovalController|RemovalQml|TimelineQml|ExportQml" --output-on-failure`
Expected: PASS；`RemovalQml.*` 检查所有 objectName、`maskMode` 显隐、置信度条点击、导出提示按钮。

```bash
git add src/app/ConfidenceBand.qml src/app/JobProgressPanel.qml src/app/RemovalPanel.qml src/app/MaskEditorOverlay.qml src/app/TimelineClipDelegate.qml src/app/Main.qml src/app/ExportDialog.qml src/ui/RemovalController.h src/ui/RemovalController.cpp src/core/RemovalErrors.h src/core/RemovalErrors.cpp tests/ui/RemovalControllerProgressTest.cpp tests/app/RemovalQmlTest.cpp tests/CMakeLists.txt src/app/CMakeLists.txt src/ui/CMakeLists.txt src/core/CMakeLists.txt
git commit -m "feat(ux): add removal confidence, progress, partial redo and export prompt"
```

---

### Task 9: 评估语料、性能/健壮性、验收报告

**Files:**
- Create: `tests/fixtures/removal/generate_fixtures.ps1`
- Create: `tests/fixtures/removal/README.md`
- Create: `tests/ai/RemovalEvalTest.cpp`
- Create: `tests/ai/RemovalEvalMetrics.h`（测试内 IoU/PSNR/时域差分辅助函数）
- Create: `tests/ai/RemovalRobustnessTest.cpp`
- Create: `docs/superpowers/research/2026-11-15-m4-acceptance.md`
- Modify: `tests/CMakeLists.txt`、`src/app/main.cpp`（新增 `--selftest-removal`）

**Interfaces:**
- Consumes: 全部 M4 组件；Task 0 阈值；FFmpeg CLI（仅生成 fixtures）。
- Produces: 验收报告；固定样本与指标；性能/健壮性测试。

- [ ] **Step 1: 生成固定语料**

`tests/fixtures/removal/generate_fixtures.ps1`（用 LGPL 可用的 `mpeg4` 编码；视频 640×360、5 秒、30 fps，控制仓库体积）：

```powershell
$ff = $env:VIDEOEDITOR_FFMPEG
if (-not $ff) { $ff = "ffmpeg" }
$out = $PSScriptRoot
$size = "640x360"
$rate = 30
$dur = 5

# 1) 静态台标：纯色/纹理背景 + 右上角红块。
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x=520:y=30:w=80:h=50:color=red@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\static_logo.mp4"

# 2) 慢速移动目标：行人方块从左向右移动。
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x='80+60*t':y=140:w=70:h=120:color=yellow@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\slow_target.mp4"

# 3) 干净背景（GT）：同一背景，无目标。
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -c:v mpeg4 -q:v 3 "$out\clean_background.mp4"

# 4) 遮挡样本：目标经过一条黑色竖条。
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x='80+60*t':y=140:w=70:h=120:color=yellow@1:t=fill,drawbox=x=300:y=0:w=30:h=360:color=black@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\occlusion.mp4"

# 5) 真值遮罩：与目标同运动的白块黑底。
& $ff -y -f lavfi -i "color=c=black:size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x='80+60*t':y=140:w=70:h=120:color=white@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\slow_target_mask.mp4"
& $ff -y -f lavfi -i "color=c=black:size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x=520:y=30:w=80:h=50:color=white@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\static_logo_mask.mp4"
```

`README.md` 写明：这些是**测试素材**，由 FFmpeg CLI 生成，不进入安装包；覆盖静态台标、慢速移动、遮挡、干净 GT、真值遮罩；每个样本 5 秒 640×36030，便于 CI。

- [ ] **Step 2: 写评估测试**

`tests/CMakeLists.txt` 的 `ai_tests` 源列表追加 `ai/RemovalEvalTest.cpp`、`ai/RemovalRobustnessTest.cpp`。

`tests/ai/RemovalEvalMetrics.h`：

```cpp
#pragma once
#include <cmath>
#include <QColor>
#include <QImage>
#include <QList>
#include <QtGlobal>
#include "MaskCodec.h"

namespace VideoEditor {

inline double maskIou(const BinaryMask &a, const BinaryMask &b)
{
    if (a.width != b.width || a.height != b.height || !a.isValid() || !b.isValid())
        return 0.0;
    qint64 intersection = 0;
    qint64 unionPixels = 0;
    for (int i = 0; i < int(a.bits.size()); ++i) {
        const bool pa = a.bits[size_t(i)] != 0;
        const bool pb = b.bits[size_t(i)] != 0;
        if (pa && pb) ++intersection;
        if (pa || pb) ++unionPixels;
    }
    return unionPixels == 0 ? 1.0 : double(intersection) / double(unionPixels);
}

inline double psnrInMask(const QImage &a, const QImage &b, const BinaryMask &mask)
{
    if (a.size() != b.size() || !mask.isValid() || mask.width != a.width())
        return 0.0;
    double mse = 0.0;
    qint64 count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!mask.at(x, y)) continue;
            const QColor ca = a.pixelColor(x, y);
            const QColor cb = b.pixelColor(x, y);
            const double dr = ca.red() - cb.red();
            const double dg = ca.green() - cb.green();
            const double db = ca.blue() - cb.blue();
            mse += dr * dr + dg * dg + db * db;
            count += 3;
        }
    }
    if (count == 0 || mse <= 1e-9) return 100.0;
    mse /= double(count);
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

inline double temporalDeltaInMask(const QList<QImage> &frames,
                                  const BinaryMask &mask)
{
    if (frames.size() < 2) return 0.0;
    double total = 0.0;
    qint64 count = 0;
    for (int i = 1; i < frames.size(); ++i) {
        for (int y = 0; y < mask.height; ++y) {
            for (int x = 0; x < mask.width; ++x) {
                if (!mask.at(x, y)) continue;
                const QColor a = frames[i - 1].pixelColor(x, y);
                const QColor b = frames[i].pixelColor(x, y);
                total += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green())
                       + qAbs(a.blue() - b.blue());
                count += 3;
            }
        }
    }
    return count == 0 ? 0.0 : total / double(count);
}

} // namespace VideoEditor
```

`tests/ai/RemovalEvalTest.cpp`（不依赖模型；模型端到端由 Task 9 Step 3 的 `--selftest-removal` 记录到验收报告）：

```cpp
#include <gtest/gtest.h>
#include <QFileInfo>
#include <QTemporaryDir>
#include "MediaProbe.h"
#include "RemovalCacheKey.h"
#include "RemovalEvalMetrics.h"

using namespace VideoEditor;

TEST(RemovalEvalMetrics, ComputesIouPsnrAndTemporalDelta)
{
    BinaryMask a;
    a.width = a.height = 8;
    a.bits.assign(64, 0);
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 6; ++x) a.set(x, y, true);
    BinaryMask b = a;
    EXPECT_DOUBLE_EQ(maskIou(a, b), 1.0);
    b.set(0, 0, true);
    EXPECT_LT(maskIou(a, b), 1.0);
    EXPECT_GT(maskIou(a, b), 0.9);

    QImage black(8, 8, QImage::Format_ARGB32);
    black.fill(Qt::black);
    QImage white = black;
    white.fill(Qt::white);
    EXPECT_DOUBLE_EQ(psnrInMask(black, black, a), 100.0);
    EXPECT_LT(psnrInMask(black, white, a), 1.0);

    QList<QImage> frames{black, white, black};
    EXPECT_GT(temporalDeltaInMask(frames, a), 0.0);
}

TEST(RemovalEval, FixturesDecodeAtExpectedGeometry)
{
    const QString fixtures = QStringLiteral(VIDEOEDITOR_FIXTURE_DIR) + "/removal";
    if (!QFileInfo::exists(fixtures + "/static_logo.mp4"))
        GTEST_SKIP() << "removal fixtures missing; run generate_fixtures.ps1";
    QString error;
    const auto info = MediaProbe::probe(fixtures + "/static_logo.mp4", &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->width, 640);
    EXPECT_EQ(info->height, 360);
    EXPECT_EQ(info->frameRate, (Rational{30, 1}));
    EXPECT_NEAR(info->durationUs, 5'000'000, 40'000);
}

TEST(RemovalEval, CacheBudgetIsAtMostTwiceSource)
{
    QTemporaryDir dir;
    const qint64 sourceBytes = 2'000'000; // 5s 640x360 mpeg4 近似上限
    // 端到端缓存大小由 --selftest-removal 输出；这里锁定预算计算函数。
    EXPECT_LE(removalCacheBudgetBytes(sourceBytes), sourceBytes * 2);
}
```

`removalCacheBudgetBytes` 定义在 `src/core/RemovalCacheKey.h/.cpp`（Task 2），UI 缓存状态、selftest 与验收报告共用。

`tests/ai/RemovalRobustnessTest.cpp`：

```cpp
#include <gtest/gtest.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include "AiFeatureGate.h"
#include "AiJobManager.h"
#include "MaskSidecarStore.h"
#include "ModelRegistry.h"
#include "RemovalPreflight.h"

using namespace VideoEditor;

namespace {
class CrashingTransport : public AiTransport {
    Q_OBJECT
public:
    bool start(const AiJobSpec &, QString *) override {
        QTimer::singleShot(0, this, [this] {
            emit finished(1, QProcess::CrashExit);
        });
        return true;
    }
    void cancel() override {}
};

class DiskFullTransport : public AiTransport {
    Q_OBJECT
public:
    bool start(const AiJobSpec &, QString *) override {
        QTimer::singleShot(0, this, [this] {
            emit message(QJsonObject{{QStringLiteral("event"), QStringLiteral("error")},
                                     {QStringLiteral("code"), QStringLiteral("disk_full")},
                                     {QStringLiteral("message"), QStringLiteral("no space")}});
            emit finished(1, QProcess::NormalExit);
        });
        return true;
    }
    void cancel() override {}
};
} // namespace

TEST(RemovalRobustness, CrashMarksFailedAndKeepsProject)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new CrashingTransport);
    });
    AiJobSpec spec;
    spec.jobId = QStringLiteral("crash");
    spec.cacheKey = QString(64, QLatin1Char('a'));
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(spec, &error)) << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result = qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("crash"));
    EXPECT_TRUE(result.errorMessage.contains(QStringLiteral("已完成的分段已保留")));
}

TEST(RemovalRobustness, DiskFullStopsWritingAndKeepsCompletedChunks)
{
    AiFeatureGate gate;
    gate.setEnabledForTest(true);
    AiJobManager manager(gate);
    manager.setTransportFactoryForTest([] {
        return std::unique_ptr<AiTransport>(new DiskFullTransport);
    });
    AiJobSpec spec;
    spec.jobId = QStringLiteral("disk");
    spec.cacheKey = QString(64, QLatin1Char('b'));
    QSignalSpy finished(&manager, &AiJobManager::jobFinished);
    QString error;
    ASSERT_TRUE(manager.start(spec, &error)) << error.toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 2000);
    const AiJobResult result = qvariant_cast<AiJobResult>(finished.at(0).at(0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("disk_full"));
    EXPECT_FALSE(result.errorMessage.isEmpty());
}

TEST(RemovalRobustness, ModelHashMismatchDisablesFeature)
{
    QTemporaryDir dir;
    const QString modelPath = dir.filePath(QStringLiteral("model.onnx"));
    QFile model(modelPath);
    ASSERT_TRUE(model.open(QIODevice::WriteOnly));
    model.write("not-a-real-model");
    model.close();

    const QString manifestPath = dir.filePath(QStringLiteral("models.manifest.json"));
    QJsonObject manifest{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("commercialApproved"), true},
        {QStringLiteral("legalSignOff"), QJsonObject{
             {QStringLiteral("approvedBy"), QStringLiteral("legal")},
             {QStringLiteral("date"), QStringLiteral("2026-11-15")},
             {QStringLiteral("scope"), QStringLiteral("commercial")}}},
        {QStringLiteral("models"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("sam2.1-hiera-tiny")},
             {QStringLiteral("family"), QStringLiteral("tracking")},
             {QStringLiteral("commercialApproved"), true},
             {QStringLiteral("files"), QJsonObject{
                  {QStringLiteral("image_encoder"), QJsonObject{
                       {QStringLiteral("path"), modelPath},
                       {QStringLiteral("sha256"), QString(64, QLatin1Char('0'))}}}}}}}}};
    QFile out(manifestPath);
    ASSERT_TRUE(out.open(QIODevice::WriteOnly));
    out.write(QJsonDocument(manifest).toJson());
    out.close();

    QString error;
    const auto registry = ModelRegistry::load(manifestPath, &error);
    ASSERT_TRUE(registry.has_value()) << error.toStdString();
    QString reason;
    EXPECT_FALSE(registry->featureEnabled(&reason));
    EXPECT_TRUE(reason.contains(QStringLiteral("校验失败")));
}

TEST(RemovalRobustness, ChineseProjectPathRoundTripsMasks)
{
    QTemporaryDir dir;
    const QString projectPath = dir.filePath(QStringLiteral("我的 工程.veproj"));
    MaskSidecarStore store(projectPath);
    ASSERT_TRUE(store.ensureLayout());
    BinaryMask mask;
    mask.width = mask.height = 16;
    mask.bits.assign(256, 0);
    mask.set(8, 8, true);
    QString hash;
    QString error;
    ASSERT_TRUE(store.writeMask(mask, &hash, &error)) << error.toStdString();
    const auto read = store.readMask(hash, &error);
    ASSERT_TRUE(read.has_value()) << error.toStdString();
    EXPECT_EQ(read->bits, mask.bits);
}
```

每个测试用 `FakeTransport`/临时目录/注入 `FreeBytesFn`，不得真的杀系统进程或写满磁盘。

- [ ] **Step 3: 运行性能门禁**

在参考 AMD GPU 与 RTX 3060 级别机器上运行：

```powershell
cmake --build --preset windows-x64-release --target ai_tests export_tests VideoEditor
ctest --preset windows-x64-release -R "RemovalEval|RemovalRobustness" --output-on-failure
.\build\windows-x64-release\src\app\Release\VideoEditor.exe --selftest-removal
```

`--selftest-removal`（`main.cpp`/`AppController` 新增，Task 9 必做）：
1. `main.cpp` 增加 `QCommandLineOption("selftest-removal")`，命中时调用 `AppController::selftestRemoval()`，成功退出 0，失败 4。
2. `AppController::selftestRemoval()` 用 `MediaProbe` 打开 `tests/fixtures/removal/static_logo.mp4`，构造一个 `Project` + 视频轨 + 5 秒 clip；用 `FrameReader` 读 `static_logo_mask.mp4` 第 0 帧，按亮度 >127 生成 `BinaryMask`，写 `MaskSidecarStore`，建 `RemovalLayer`（`sourceIn=0, sourceOut=5s`，一个关键帧）。
3. 用 `AiFeatureGate`/`AiJobManager` 启动 `AiJobSpec`（cache 到临时目录），`QEventLoop` 等待 `jobFinished`；超时 10 分钟退出 4。
4. 用 `RemovalCache` 读回修复 patch，与 `clean_background.mp4` 逐帧比较洞内 PSNR；用 `RemovalEvalMetrics` 计算；检查缓存大小 ≤ `removalCacheBudgetBytes(source size)`、耗时 ≤ 10 分钟。
5. 打印 `REMOVAL_SELFTEST_OK frames=<n> psnr=<db> seconds=<s> cacheBytes=<b>`；任一项超阈值打印 `REMOVAL_SELFTEST_FAIL <reason>` 并退出 4。

阈值：
- 1080p30 10 秒 ≤ 10 分钟（RTX 3060 级；AMD 参考机记录实测）。
- tracking ≥ 5 fps；MI-GAN ≥ 10 fps；LaMa ≥ 2 fps；预览 ≥ 24 fps；VRAM 峰值 ≤ 4 GB（6 GB 卡）。
- 缓存 ≤ 2× 源大小。

- [ ] **Step 4: 写验收报告**

`2026-11-15-m4-acceptance.md` 必须包含：
1. 环境：CPU/GPU/驱动/VRAM/Windows 版本/构建号。
2. 测试矩阵：静态台标、慢速移动、遮挡、干净 GT、24/30/60 fps、中文路径、4 GB/6 GB 显存、CPU-only。
3. 指标：mask IoU/J&F、洞内 PSNR、时域差分、主观 1–5 分（3 名评审）、失败分类（遮挡/快速运动/反射/大洞）。
4. 性能：tracking/inpaint fps、10 秒片段端到端、缓存大小、VRAM 峰值、预览 fps。
5. 健壮性：kill 子进程、设备重置、磁盘满、缓存损坏、模型哈希不符、工程另存为。
6. 已知限制：快速运动/大遮挡/反射效果有限；一个片段一个目标；1080p SDR；每层 ≤30 秒。
7. 结论：对照规格 §11.2 第 5 条与 §11.3；签字/日期。
8. 若未达阈值：列出具体失败样本与后续 M5 工作，不得把未达标写成通过。

- [ ] **Step 5: 运行全套回归并提交**

Run:
`cmake --build --preset windows-x64-release && ctest --preset windows-x64-release --output-on-failure`
Expected: M1/M2/M3/M4 全部 PASS，模型/GPU 缺失的测试 SKIP 且消息明确。

```bash
git add tests/fixtures/removal tests/ai/RemovalEvalTest.cpp tests/ai/RemovalEvalMetrics.h tests/ai/RemovalRobustnessTest.cpp tests/CMakeLists.txt docs/superpowers/research/2026-11-15-m4-acceptance.md src/app/main.cpp src/app/AppController.h src/app/AppController.cpp
git commit -m "test(m4): add removal evaluation corpus, robustness gates and acceptance report"
```

---

## Self-Review

### 1. Spec Coverage

| 规格 | 实现任务 |
|---|---|
| §3.1 静态/运动目标、本地推理、遮罩跟踪、关键帧修正、局部重做、原片对比 | Task 0（模型/许可）、Task 3（遮罩/关键帧）、Task 5（跟踪）、Task 6（修复）、Task 7（对比）、Task 8（局部重做） |
| §4.6 画笔/框选/羽化/边缘修正 | Task 2（RLE/羽化）、Task 3（画笔/框选/擦除） |
| §4.6 逐帧跟踪与置信状态 | Task 5（跟踪 + confidence）、Task 8（置信度条/低置信跳转） |
| §4.6 修复按时间段生成并写缓存 | Task 2（cache/manifest）、Task 4（job/chunk）、Task 6（chunk 写入） |
| §4.6 生成层不修改原素材，可删除/禁用/局部重做 | Task 1（schema/undo）、Task 7（compositor 原片+补丁）、Task 8（删除/禁用/redoRange） |
| §4.6 全部推理本地、不上传 | Task 4（子进程、无网络、无遥测） |
| §4.8 渐进披露、少层级、中文提示 | Task 3（首屏）、Task 8（高级/进度/错误） |
| §5.5 预览清晰度/代理不影响最终结果 | Task 7（修复补丁独立于代理；预览/导出同一 compositor） |
| §6.1 AI 子进程、崩溃不影响工程 | Task 4（LocalAiTransport/AiJobManager）、Task 9（kill 子进程） |
| §6.3 AI 引擎只接受帧序列+遮罩+参数 | Task 4（AiJobSpec）、Task 5/6（引擎） |
| §7.3 RemovalLayer 字段（range/maskKeyframes/trackingState/parameters/cacheKey/cachePath/revision） | Task 1 |
| §7.4 缓存可重建/可清理，清理不影响工程 | Task 2（旁车 vs 缓存、GC 不删旁车） |
| §8 数据流：选区→关键帧→跟踪→修复→缓存→生成层→预览/导出 | Task 3→5→6→2→1→7 |
| §9.3 首帧遮罩/分割跟踪/修复/时域后处理/缓存回写/返修 | Task 2/3/5/6/2/8 |
| §9.3 显存不足降规格/分块；暂停/取消/续跑；崩溃标记失败 | Task 5/6（fallback/chunk）、Task 8（pause/cancel/resume）、Task 9（robustness） |
| §11.1 AI 评估（静态/运动、遮罩覆盖、闪烁、主观、撤销/局部重做） | Task 9 |
| §11.1 健壮性（损坏、强杀、磁盘满、GPU 掉线/驱动重置） | Task 9 |
| §11.2 第 5 条（跟踪、关键帧修正、局部重做、完全撤销） | Task 1/3/5/7/8/9 |
| §11.3 AI 修复 1080p30 10 秒 ≤ 10 分钟 | Task 0（阈值）、Task 9（验收） |
| §12 模型许可证/商业使用选型门禁 | Task 0 |
| §12 驱动重置/硬件多样性/软件回退 | Task 4（EP 回退）、Task 5/6（CPU fallback）、Task 8（错误文案） |

### 2. Placeholder Scan

已检查：计划中没有 “TBD/TODO/实现稍后/类似 Task N/添加适当错误处理” 之类占位。Task 0 的 manifest 中 `<64-hex>`、`<sam2-tiny-split-onnx-url>` 是**执行时必须用实际值替换的外部输入**，不是产品代码占位；Task 0 的验收要求每个模型写入真实 SHA-256 与真实路径。所有代码步骤都给出实际签名、测试代码或精确算法。

### 3. Type Consistency

- `RemovalLayer`/`MaskKeyframe`/`RemovalParameters` 在 Task 1 定义，Task 2/3/4/7/8 始终使用同名字段。
- `RemovalCacheManifest`/`RemovalCacheChunk`/`RemovalChunkIndex`/`RemovalChunkFrame` 在 Task 2 定义；Task 6/7 使用 `writeChunk`、`readChunkIndex`、`readFrameImage`、`invalidateFrameRange`，名称一致。
- `AiJobSpec`/`AiJobResult` 在 Task 4 定义；Task 5/6/8 使用 `redoFirstFrame`/`redoLastFrame`/`resume`/`trackingState`，名称一致。
- `RepairFrameSource::Frame` 字段 `patch/crop/sourceWidth/sourceHeight` 在 Task 7 Step 2/4 一致。
- `RemovalCompareMode` 只在 Task 7 定义；`PreviewController::setRemovalCompareMode` 接受字符串，映射到该枚举，避免 QML 元类型问题。
- `MaskSidecarStore::writeTracking/readTracking` 在 Task 2 定义，Task 5/8 使用同一签名。
- `removalErrorMessage` 在 Task 8 定义，Task 4 的 `AiJobManager` 只传 code/message，不重复定义文案。

### 4. Review Focus → Tests

1. 片段裁切/移动/替换素材/改帧率 → Task 1 `RejectsLayerOutsideAssetRange`（校验）、Task 2 `RemovalCacheKey.ExcludesTimelinePosition`、Task 7 `RemovalRender.Missing/OutsideRangeFallsBack`。
2. 缓存清理/写一半/损坏/换机 → Task 2 `RemovalCache.DetectsPartialAndCorruptTree`、`MaskSidecarStore` 清理测试、Task 7 `MissingCacheFallsBack`、`RemovalExport.FailsBeforeWritingOutput`、Task 9 robustness。
3. 未保存/另存为/中文路径 → Task 2 `MaskSidecarStore.WritesReadsAndRelocatesMasks`（中文工程名）、Task 3 未保存工程测试、Task 9 `ChineseProjectPathRoundTripsMasks`。
4. 低显存/驱动重置/CPU-only → Task 4 `FailsClosedWithoutLegalApproval` + fake error、Task 5 DML/CPU parity（SKIP 说明）、Task 6 VRAM 预算测试、Task 8 `MapsCodesToChineseActionableMessages`。
5. 退化遮罩/贴边/零面积/羽化过大/关键帧乱序/播放头越界 → Task 2 `MaskCodec`/`MaskEditor` clamp、Task 3 关键帧排序/羽化不改哈希、Task 6 `CropIsPaddedClampedAndFeathered`/`EmptyMaskProducesNoPatch`。

## Unresolved Risks (must be tracked in Task 0/9)

1. **训练数据许可**：SA-1B/Places2/YouTube-VOS 的商业使用结论未定；这是发布级 go/no-go，不是工程问题。
2. **SAM 2/EfficientTAM 拆分 ONNX 的 DirectML 正确性/性能**：complex RoPE、memory attention、算子回退可能导致慢或数值错误；Task 0 必须实测，Task 5 必须 CPU parity。
3. **运动目标闪烁**：逐帧 MI-GAN/LaMa + 平移中值稳定化只能缓解；快速运动/大遮挡/反射达不到 ProPainter 级，必须在 UI/验收报告中如实标注。
4. **参考 AMD GPU/驱动未指定**：Task 0 必须固定型号、VRAM、驱动版本，否则性能门禁不可复现。
5. **PNG 补丁缓存的预览解码成本**：大补丁/全帧回退时可能低于 24 fps；Task 9 必须测，必要时 M5 改为 FFV1/硬件解码。
6. **缓存体积**：2× 源大小目标在复杂纹理/大洞下可能超标；Task 9 记录并给出清理 UI。
7. **模型分发/安装包体积**：短名单约 390–425 MB；是否随包分发或首次下载需产品/法务确认；首次下载不得破坏“本地推理、不上传素材”。
8. **ONNX Runtime/DirectML 维护状态与版本锁定**：DirectML 处于维护模式；`OnnxExecutor` 必须保持 EP 抽象，M5 评估 WinML。
9. **法务对 ONNX 转换权重的再分发义务**：Apache-2.0 修改文件标记、NOTICE、DirectML ThirdPartyNotices 必须由 Task 0 明确。
10. **一个片段一个目标的限制**：规格 `Clip.removalLayerId` 单值；多目标需要分割/复制片段，产品文案必须说明。
11. **未保存工程/另存为的旁车迁移**：Task 2/3 已设计 `relocateTo`，但需在 AppController 保存流程中实际调用并测试；若遗漏，遮罩会丢。
12. **主观质量阈值/签字人**：Task 0/9 需要明确 3 名评审与签字人，否则“运动目标通过”无法验收。
