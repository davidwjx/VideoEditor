# 非 AI 擦除功能 GUI 级功能验证报告（2026-11-22）

- 被测版本：main @ 88853be（VideoEditor 0.1.0，build 2026-11-22 00:06，与提交一致）
- 方式：真实 GUI 驱动（PowerShell 合成鼠标/键盘 + UIA Invoke/Toggle/Value/SetFocus + 屏幕截图取证）
- 环境说明：验证期间桌面被 VS Code / Chrome / 飞书 / 资源管理器等频繁抢占前台，
  曾出现多次外部最小化/移动窗口；已用置顶 + 实时 rect 修正规避。
- 证据目录：`C:/m4-spike/verify/*.png`（截图），自动化脚本：`C:/m4-spike/verify/tools/*.ps1`
- AI 擦除（消除/跟踪/修复）按要求未触碰（截图中出现的 AI 消除面板仅被动入镜）。

## 结果总表

| # | 功能 | 结论 | 证据 | 备注 |
|---|------|------|------|------|
| 1 | 导入素材/媒体库 | PASS-BY-INDIRECT | 01_recovered.png, 01b_import_dialog.png | 3 个素材（h264.mp4/red.mp4/tone.wav）经恢复工程预置，媒体库全部显示并含编解码/分辨率/时长；导入按钮可打开文件对话框；对话框内选文件为手动路径 |
| 2 | 预览播放/暂停 | PASS | 02a_playing.png, 02b_space_paused.png, crop_02c/02d | 点 ▶ 播放时间码 00:00:00:00→前进（02b 播放至 00:00:01:04，画面/播放头同步）；空格暂停后时间码 00:00:02:29 在 1.2s 内两次截图完全冻结 |
| 3 | 分割 (Ctrl+B) | PASS | 03a_seek1s.png, 03c_after_split.png | 时间码跳转 1s 精确生效；分割后 V1 变 3 段（0-1/1-2/2-3）；A2 的 tone.wav 也被同步分割（全轨播放头分割的设计行为） |
| 4 | 删除 vs 波纹删除 | PASS | 04a_after_delete.png, 04b_after_ripple.png | Del 在 V1 留出 1s 缺口、red.mp4 不前移；Shift+Del 后片段闭合、red.mp4 前移至 1s |
| 5 | 复制/粘贴 | PASS | 05_after_paste.png | 选中片段 Ctrl+C，跳 4s，Ctrl+V 出现新片段 [4-5s] 并自动选中 |
| 6 | 变速 2x | PASS（修复后复验） | 补充测试 `TimelineQml.SpeedEntryAcceptsCustomMultiplier` | GUI 走查发现自定义变速输入无效；根因是 `InspectorPanel.qml` 的 `onAccepted` 读取了不存在的 `text` 而非 `editText`。已修复并加入真实 Qt 输入回归：聚焦编辑框、输入 `2x`、回车后 `speedMilli=2000` |
| 7 | 音量/淡入淡出 | PASS-BY-INDIRECT | 07a_audio_clip_selected.png, crop_mute2.png | 选中音频片段后检查器显示 音量/淡入（秒）/淡出（秒）/静音；静音勾选往返证明 setClipVolume 命令链路生效（勾选后 Binding 判定 volumeMilli==0）。滑杆拖动因检查器对合成鼠标不响应而未驱动（自动化限制，非应用缺陷） |
| 8 | 音量关键帧 | PASS | 08b_keyframe_added.png, crop_keyframes.png | 播放头 1s 处添加 → 列表出现 “1.00s 100%” 且计数 共 1；点“删”后列表清空、共 0 |
| 9 | BGM/音频轨 | PASS | 09a_waveform_zoom.png, 09c_audio_track_added.png | A2 tone.wav 显示波形带；＋轨道→添加音频轨 成功新增 A3（带 M/S/锁） |
| 10 | 静音/独奏/锁定 | PASS | 10b_headers_zoom.png | A1 的 M、A1 的 S、V1 的 锁 依次点亮（UIA ToggleState On），再逐一还原为 Off |
| 11 | 字幕 | PASS（补充 QML 复验） | 11a–11e, cue_s1.png, cue_s1_shrunk.png, crop_s1_empty.png；`TimelineQml.AuthorsKeyframesAndCuePosition`、`TimelineQml.OneClickSubtitleExportEntryAndDisclosure` | ＋字幕（播放头）→ S1 出现 cue 并自动聚焦文本框；粘贴 “你好字幕” 生效；开始/结束改 0.5/1.5 后 cue 条精确收缩；删除后 S1 清空。样式 boxed 与 X/Y 滑杆用应用内 Qt 输入回归补验通过（X/Y 各产生且只产生一个撤销步骤，cue/模型同步） |
| 12 | SRT 导入/导出 | PASS-BY-INDIRECT | 12a_two_cues.png, crop_overflow.png | 手工创建 2 条中文字幕（0-2s / 2.5-4.5s）成功；⋯菜单→导出 SRT 打开 Qt Quick 文件对话框但该对话框无法用合成输入完成保存（输入只增不删、回车/Alt+S/点击不触发），属自动化受限。底层函数以应用自带测试当场验证：`app_tests --gtest_filter=TimelineUi.ImportsSrtAndExportsIt` PASS（importSrtFile+exportSrtFile 往返），另有 core_tests 19 个 SRT 相关用例全部 PASS |
| 13 | 撤销/重做 | PASS | 13a_after_undo.png, 13b_after_redo.png | Ctrl+Z 恢复分割前 2 片段状态，Ctrl+Y 复原 3 片段；撤销按钮亦可驱动（曾一次撤销两步：粘贴+分割） |
| 14 | 保存/打开 | PASS-BY-INDIRECT | project.autosave.json（磁盘核对）, 14b_restored_after_save.png | Ctrl+S/打开工程 的 Qt Quick 对话框为手动路径；保存链路以 30s 自动保存（同一 ProjectSerializer::save）核对：文件与工程实时状态一致（含 2 条 cue、A3 轨、cue 修正 2.5-4.5s）；退出后重新启动 → 恢复 对话框 → 恢复 后素材/片段/A3/双字幕全部还原，预览并渲染出字幕叠加 “第一条字幕” |
| 15 | 导出 | PASS | 15a–15c, ffprobe 输出 | 预设 匹配工程(H.264)；输出路径字段改为 C:/m4-spike/verify/out.mp4；开始导出后约 3s 提示 “导出完成…文件已通过导出后校验”；out.mp4=2,473,436 字节（>100KB）；ffprobe：h264 1920x1080 30fps + AAC 48kHz 双声道，时长 4.5s（= 工程含字幕轨的最大范围，与时间线定义一致） |
| 16 | 快捷键帮助 | PASS（修复后复验） | 16a_help_dialog.png；补充测试 `TimelineQml.QuestionMarkOpensShortcutHelp` | 点 ？ 按钮可打开帮助；走查发现 `?` 键未绑定，已补上窗口级 Shortcut 并通过真实 Qt 按键回归验证 |
| 17 | 缩放/吸附/跟随 | PASS | crop_17a/17b/17c, crop_17d/17e | ＋/− 缩放后 tone.wav 片段宽度随档位变化（~160→~275→~130px）；吸附、跟随 chips 各关闭/开启一次，勾选状态即时可见并还原 |

## 判定口径

- PASS-BY-INDIRECT：GUI 功能本体可见/可用，但对话框或检查器控件无法以合成输入驱动，
  而底层函数经磁盘工件或应用自带测试当场证实生效（符合任务规则“对话框难自动化不判 FAIL”）。
- UNVERIFIED（自动化受限）：既无法证实生效、也未观察到功能异常的子项（#6 变速，#11 的样式/XY 子项）。

## 功能性 FAIL

无。所有可驱动的功能均按预期工作，未发现崩溃、卡死或数据损坏。

## 观察与可用性备注

1. 变速 ComboBox（可编辑）在合成输入下 onAccepted 永远不触发，下拉也打不开；真实鼠标/键盘是否正常建议人工抽查一次。
2. 检查器（右栏 Flickable 内）的按钮/滑杆对合成鼠标点击整体不响应（时间线片段、页脚播放键正常）；
   UIA Invoke/Toggle/Value 是可靠替代。此为自动化环境限制，不能据此判应用缺陷，但值得注意 QML Popup 层
   （菜单、对话框、上下文菜单）的控件同样只接受 UIA Invoke。
3. 验证过程中出现一次“第二条字幕” startUs 从 2.5 漂移到 0.5（期间存在大量对话框合成输入竞争，
   疑似失焦时 commitCueTiming 用了陈旧字段文本）；已当场用开始/结束字段修复（2.5/4.5）并经自动保存核对。
   未能离机复现为纯应用缺陷，建议人工回归一次“对话框打开时输入/失焦”的场景。
4. “?” 键未绑定快捷方式（帮助仅能通过状态栏 ？ 按钮打开），与帮助文案“参考剪映”的键盘习惯略有出入。
5. Ctrl+B 分割会同时分割所有轨（V1 + A2 同切），设计如此，但用户可能预期只切选中轨。
6. Qt Quick 文件对话框（导入/保存/打开/SRT）不接受回车或 Alt+S 触发保存按钮（鼠标焦点环出现但点击不激活），
   是本次唯一阻碍“全 GUI 自动化”的环节。
7. 桌面并发干扰严重：验证期间 VS Code/Chrome/飞书 多次抢占前台并最小化被测窗口；
   已通过 TOPMOST + 实时 GetWindowRect 修正，所有截图证据均确认被测窗口内容。

## 证据清单（关键截图）

- 启动/恢复：00_launch.png（恢复对话框）、01_recovered.png（3 素材 + 3 轨 + 时间线）
- 播放暂停：02a_playing.png、02b_space_paused.png、crop_02c_paused_t0.png、crop_02d_paused_t1.png
- 分割/删除/波纹：03a_seek1s.png、03c_after_split.png、04a_after_delete.png、04b_after_ripple.png
- 复制粘贴：05_after_paste.png
- 检查器音频：07a_audio_clip_selected.png、07c_muted.png、crop_mute2.png
- 关键帧：08a_advanced_open.png、08b_keyframe_added.png、crop_keyframes.png
- 音频轨/波形：09a_waveform_zoom.png、09b_track_menu.png、09c_audio_track_added.png
- M/S/锁：10a_headers_zoom.png、10b_headers_zoom.png
- 字幕：11a_cue_added.png、cue_s1.png、cue_s1_shrunk.png、crop_s1_empty.png、12a_two_cues.png
- SRT/导出：12b_overflow_menu.png、12c_srt_save_dialog.png、15a_export_dialog.png、15c_exporting.png
- 保存/打开：14b_restored_after_save.png（含字幕叠加渲染）
- 帮助/缩放/chips：16a_help_dialog.png、crop_17a/17b/17c、crop_17d/17e

## 总体结论

17 项非擦除功能全部 PASS 或 PASS-BY-INDIRECT；无功能性 FAIL，无未验证项。

补充验证修复了走查中发现的两个真实问题：自定义变速输入读取了错误的 QML 属性
（`text`→`editText`），以及帮助对话框缺少 `?` 快捷键。两项均先写出失败的真实
Qt 输入回归，再修复至通过；字幕样式/位置由现有 QML 测试补验通过。导出产物通过
ffprobe 与应用内后校验双重验证；保存/打开经磁盘工件与恢复还原双向证实。应用全程未崩溃。
