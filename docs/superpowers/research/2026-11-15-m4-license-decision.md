# M4 License Decision — Local AI Object Removal

- Date: 2026-11-15
- Branch / base: `feature/m4-ai-removal` / `305152d`
- Scope: licensing gate for the M4 model shortlist. No product code was written; `src/` is unchanged.
- Related: [M4 research memo](2026-11-15-m4-ai-removal-research.md), [DirectML spike](2026-11-15-m4-dml-spike.md), [M4 plan](../plans/2026-11-15-m4-ai-removal.md)
- Manifest: [`assets/models/models.manifest.json`](../../../assets/models/models.manifest.json)

## Status

**TECHNICAL_EVALUATION_ONLY.** No legal sign-off has been provided. Nothing in this document is a
legal opinion, an approval, or a licence grant. The shortlist is engineering-safe at the
**code/weights** level (Apache-2.0/MIT), but every practical model has a **training-data
provenance question** (SA-1B, Places2, YouTube-VOS, MOSE/internal). Per the M4 plan, the product
feature stays disabled and the installer ships no model files until legal signs a scope.

- `models.manifest.json`: `commercialApproved = false`, `legalSignOff` empty, `releaseBlocked = true`.
- Release is **blocked pending legal sign-off**, independently of the technical gate result.
- The technical gate is also **NO_GO on the tested hardware** (see the DirectML spike); Task 5–9 must
  not start from this record.

## 1. Three-licence check table

Every row must answer all three questions: **code licence**, **weights licence**, and **training-data
terms**. The "commercial redistribution conclusion" column is deliberately unresolved; only legal
can fill it.

| Role | Model | Code licence | Weights licence | Training data (must be listed) | Commercial redistribution conclusion | Evidence |
|---|---|---|---|---|---|---|
| Tracking primary | **SAM 2.1 Hiera-Tiny** (upgrade path: **Hiera-Small**) | Apache-2.0 | Apache-2.0 (Meta checkpoints) | **SA-1B** (research-only), **YouTube-VOS** (CC BY-NC-SA 4.0), **DAVIS** (BSD-3-Clause, source-video rights separate), **MOSE**, internal Meta video data | **待法务 / UNANSWERED.** Apache-2.0 weights do not answer the dataset terms. No commercial approval. | [Meta repo](https://github.com/facebookresearch/sam2), [SA-1B licence summary](https://huggingface.co/datasets/xiuqhou/SA-Det-100k/blob/main/LICENSE), [YouTube-VOS terms](https://youtube-vos.org/dataset/vos/), [DAVIS licence](https://github.com/fperazzi/davis-2017), [SAM 2 paper](https://arxiv.org/abs/2408.00714) |
| Tracking fallback | **EfficientTAM-Ti 512** | Apache-2.0 | Apache-2.0 | **SA-V / SA-1B-related video data**; exact mixture must be obtained from the authors and confirmed by legal | **待法务 / UNANSWERED.** The card says Apache-2.0; the training mixture is not fully specified in the model card. | [EfficientTAM repo](https://github.com/yformer/EfficientTAM), [weights](https://huggingface.co/yunyangx/efficient-track-anything), [ONNX bundle](https://huggingface.co/egordm/efficienttam-ti-512) |
| Last-resort tracking | **MobileSAM** | Apache-2.0 | Apache-2.0 | Distilled from SAM on **SA-1B**; inherits the SA-1B question | **待法务 / UNANSWERED.** Image-only; not evaluated in Task 0. | [MobileSAM repo](https://github.com/ChaoningZhang/MobileSAM) |
| Inpainting fast | **MI-GAN 512** | MIT | MIT (`LICENSE-WEIGHTS`) | **Places2** (non-commercial, no redistribution), **FFHQ** (per-image CC licences including BY-NC) | **待法务 / UNANSWERED.** MIT weights do not resolve Places2/FFHQ terms. ONNX mirror provenance must also be confirmed. | [MI-GAN repo](https://github.com/Picsart-AI-Research/MI-GAN), [ONNX mirror used for the spike](https://huggingface.co/andraniksargsyan/migan), [Places2 terms](https://places.csail.mit.edu/) |
| Inpainting quality | **LaMa 512** | Apache-2.0 | Apache-2.0 (original weights; community ONNX export is tagged Apache-2.0) | **Places2** (non-commercial) | **待法务 / UNANSWERED.** ONNX conversion is a modification and may need notices/marking. | [LaMa repo](https://github.com/advimman/lama), [ONNX export](https://huggingface.co/g-ronimo/lama), [Places2 terms](https://places.csail.mit.edu/) |
| Runtime | **ONNX Runtime + DirectML** | MIT (ONNX Runtime) | Microsoft redistributable terms (DirectML) | Not applicable | **Redistributable, with obligations.** Ship the official `ThirdPartyNotices.txt` and the DirectML licence/terms with the installer; legal/compliance must confirm the exact text and placement. | [ORT licence](https://github.com/microsoft/onnxruntime/blob/main/LICENSE), [DirectML NuGet](https://www.nuget.org/packages/Microsoft.AI.DirectML), [ORT DirectML EP docs](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html) |

### 1.1 Provenance notes for the files evaluated in Task 0

The Task 0 downloads are evaluation artifacts only. They live in `C:/m4-spike/models`, are **not**
committed, and are **not** approved for redistribution.

| Evaluated file | Provenance | Licence tag observed | Open question |
|---|---|---|---|
| `sam2_1_tiny_video_onnx/onnx/*.onnx` | `square-zero-labs/sam2.1-tiny-video-onnx` (community ONNX export of `facebook/sam2.1-hiera-tiny`, revision `3b2984dd865f6e9d2cc6aed0be6a5a5c2eb352ce`) | `license: apache-2.0` in the model card | Does Meta's Apache-2.0 grant cover this third-party ONNX conversion and its redistribution? Q3/Q4. |
| `efficienttam_ti_512_fp16/*.onnx` | `egordm/efficienttam-ti-512` (revision `ef8a773fc653fc46a2e87dd182cac0f8ef980cc1`); `bundle.toml` names `models/efficienttam-ti-512/export.py` as the export source | `license: apache-2.0` | Is the export script's provenance reviewable; is the SA-V/SA-1B mixture acceptable? Q1/Q3. |
| `migan_512/migan_pipeline_v2.onnx` | `andraniksargsyan/migan` (revision `406830d0fa60666da0071c342ad2fbc8f30c5c64`); the mirror ships Picsart's MIT `LICENSE` | MIT (Picsart AI Research) | Is this mirror's ONNX an authorised derivative of the official repo? If not, rebuild with the official `scripts/create_onnx_pipeline.py` from `Picsart-AI-Research/MI-GAN`. |
| `lama_512/lama_512_fp16.onnx` | `g-ronimo/lama` (revision `386a67cedf9b8bb4fe76798c10c8d77abc0a5cc8`) | `license: apache-2.0` | ONNX conversion is a modification; notices/marking and Places2 terms. Q1/Q4. |

Network note: `huggingface.co` and `github.com` were unreachable from the test machine; downloads
were resolved through `hf-mirror.com` and hashes were computed locally. The canonical URLs above are
the authoritative evidence links.

## 2. Eight questions that legal must answer

The eight questions below are reproduced **verbatim** from the Task 0 brief / research memo §3.5.
Each is **UNANSWERED / 待法务**. Legal must answer **是 / 否 / 有条件** for each, and state the
jurisdiction, distribution channel (installer vs. optional download), and whether ONNX-converted
weights are in scope.

1. **在目标司法辖区，Apache-2.0/MIT 权重许可是否覆盖 SA-1B/Places2/YouTube-VOS 的训练数据限制？**
   - 状态：**UNANSWERED / 待法务**
   - 要求回答：是 / 否 / 有条件（注明条件）
   - 影响：若为“否”，短名单不可发布，必须重训或取得商业许可。

2. **Places2 是否存在商业许可路径；若无，重训/微调的成本与周期？**
   - 状态：**UNANSWERED / 待法务**
   - 要求回答：是 / 否 / 有条件（注明报价、周期、数据来源）
   - 影响：直接决定 MI-GAN/LaMa 能否作为商业修复路径。

3. **SAM 2 权重是否被视为 SA-1B/YouTube-VOS 的衍生作品；YouTube-VOS 的 ShareAlike 是否传播到 ONNX 转换权重？**
   - 状态：**UNANSWERED / 待法务**
   - 要求回答：是 / 否 / 有条件（注明 ShareAlike 是否传播）
   - 影响：决定跟踪主选与兜底是否可用，以及 ONNX 转换是否构成衍生。

4. **Apache-2.0 NOTICE、修改文件标记（ONNX 导出属于修改/衍生）、MIT 版权声明、About/Licenses 对话框的具体义务？**
   - 状态：**UNANSWERED / 待法务**
   - 要求回答：是 / 否 / 有条件（列出必须随包分发的文件与 UI 文案）
   - 影响：决定安装包内容、About/Licenses 对话框和文件标记义务。

5. **ONNX Runtime 的 ThirdPartyNotices 与 DirectML 的再分发条款如何随安装包分发？**
   - 状态：**UNANSWERED / 待法务 / 待合规**
   - 要求回答：是 / 否 / 有条件（列出文件路径、版本绑定与更新方式）
   - 影响：运行时合规；不解决则不得发布安装包。

6. **是否可以在 UI 中 factual 地使用模型名而不暗示 Meta/Picsart 背书？**
   - 状态：**UNANSWERED / 待法务**
   - 要求回答：是 / 否 / 有条件（注明允许的措辞与位置）
   - 影响：主界面与 Licenses 对话框的文案边界。

7. **若质量不可妥协，ProPainter 商业许可的报价/周期；另立 12–16 GB VRAM 的“AI Pro”轨道是否可行？**
   - 状态：**UNANSWERED / 待法务 / 待产品**
   - 要求回答：是 / 否 / 有条件（报价、周期、是否单独产品轨道）
   - 影响：若短名单被否，是否用商业许可的高质量模型替代。

8. **选定模型/ORT/DirectML 是否有已知专利负担？**
   - 状态：**UNANSWERED / 待法务**
   - 要求回答：是 / 否 / 有条件（逐模型 + 运行时）
   - 影响：发布前的专利风险结论。

## 3. Models that must not be used (不得采用)

These are excluded from M4. Do not add them to the manifest, installer, or dependency graph.

| Model | Reason |
|---|---|
| **ProPainter** | NTU S-Lab License 1.0 — non-commercial; commercial use requires contacting the authors. No maintained ONNX export; 1080p memory is not practical on consumer cards. |
| **E2FGVI** | CC BY-NC 4.0 — non-commercial. No ONNX export. |
| **MAT** | Research-only; built on StyleGAN2-ADA (NVIDIA non-commercial). |
| **DiffuEraser** | Apache-2.0 code, but the default pipeline depends on ProPainter (non-commercial) and Stable Diffusion 1.5 (OpenRAIL-M); no ONNX. |
| **MiniMax-Remover** | Official weights are CC-BY-NC-4.0; a third-party Apache-2.0 mirror is unverified. |
| **Netflix VOID** | Apache-2.0 but 5B parameters / 40 GB+ VRAM, outside M4; the supplied mask pipeline uses a Gemini API and conflicts with the local-only requirement. |
| **XMem** | GPL-3.0 — incompatible with a closed-source commercial distribution. |
| **SAM 3** | Custom Meta SAM License, not Apache-2.0; acceptable-use restrictions and different redistribution obligations. |
| **COCOCO** | License not confirmed; inherits AnimateDiff, SD inpainting, SAM 2 and ProPainter dependencies. |
| **ZITS** | Weights/training-data terms unclear (Places2); classified as research/fine-tune-only. |
| **Cutie** | MIT code, but no maintained ONNX export and YouTube-VOS/DAVIS training-data risk; research item, not an M4 candidate. |

**Not evaluated in Task 0:** MobileSAM and the SAM 2.1 Hiera-Small variant (only Tiny was downloaded
and probed). They remain on the conditional shortlist but are not shipped or manifest-listed.

## 4. What the shortlist does and does not mean

- **Does mean:** the shortlist has no known **code or weights** restriction for commercial
  redistribution; Apache-2.0/MIT grants are explicit.
- **Does not mean:** the weights are free of **training-data** restrictions. SA-1B is research-only,
  Places2 is non-commercial/no-redistribution, YouTube-VOS is CC BY-NC-SA. Do not describe the
  shortlist as “legally clean” in product, investor, or marketing material.
- **Distributing the app without model files** avoids shipping the weights, but the app still
  downloads or instructs the user to obtain them; legal must review that flow as well. Task 4's
  `AiFeatureGate` must remain closed while `commercialApproved=false`.

## 5. Signature and scope (NOT SIGNED)

```text
Legal reviewer: ______________________________   Date: ______________

Jurisdiction(s): ______________________________________________________

Distribution: [ ] installer bundle  [ ] first-use download  [ ] both

Scope includes ONNX-converted / quantised weights?  [ ] yes  [ ] no

Training-data position (SA-1B / Places2 / YouTube-VOS):
______________________________________________________________

Redistribution obligations accepted:
______________________________________________________________

Signature: ______________________________
```

**Current state: no signature, no scope, no approval. 待法务。**

## 6. Decision record

```text
DECISION: TECHNICAL_EVALUATION_ONLY
COMMERCIAL_APPROVED: false
LEGAL_SCOPE: unanswered — jurisdiction, distribution channel, and ONNX-converted weight
  redistribution have not been provided by legal
TRACKING_DEFAULT: sam2.1-hiera-tiny
INPAINT_FAST: migan-512; INPAINT_QUALITY: lama-512
QUALITY_THRESHOLDS: tracking_jf>=0.75; mask_parity_iou>=0.98;
  static_flicker<=2/255; moving_subjective>=3.5/5; psnr_hole>=28dB
PERF_THRESHOLDS: 1080p30 10s <=10min (RTX3060-class / reference AMD);
  tracking>=5fps; migan>=10fps; lama>=2fps; preview>=24fps; vram<=4GB
TECHNICAL_GATE: NO_GO on the tested hardware (see 2026-11-15-m4-dml-spike.md)
OVERALL_TASK0_GATE: NO_GO
RELEASE: BLOCKED pending legal sign-off and a passing technical gate
```

`TECHNICAL_EVALUATION_ONLY` is the maximum permitted activity: the spike and document work may
continue, but no product implementation, installer bundling, or release is authorised. Because the
technical gate is also NO_GO on the only available hardware, Task 5–9 do not start. If the project
wants to continue, the required sequence is: (1) legal answers the eight questions and signs a
scope, (2) the technical gate is re-run on a discrete reference AMD GPU and an NVIDIA QA machine,
(3) this decision record is updated with the signed scope and the new evidence.

## 7. Sources

- SAM 2: <https://github.com/facebookresearch/sam2>; SAM 2.1 Tiny ONNX bundle evaluated:
  <https://huggingface.co/square-zero-labs/sam2.1-tiny-video-onnx>
- SA-1B research licence: <https://huggingface.co/datasets/xiuqhou/SA-Det-100k/blob/main/LICENSE>
- YouTube-VOS: <https://youtube-vos.org/dataset/vos/>
- DAVIS: <https://github.com/fperazzi/davis-2017>
- EfficientTAM: <https://github.com/yformer/EfficientTAM>;
  <https://huggingface.co/egordm/efficienttam-ti-512>
- MobileSAM: <https://github.com/ChaoningZhang/MobileSAM>
- MI-GAN: <https://github.com/Picsart-AI-Research/MI-GAN>; ONNX mirror:
  <https://huggingface.co/andraniksargsyan/migan>
- Places2 terms: <https://places.csail.mit.edu/>
- LaMa: <https://github.com/advimman/lama>; ONNX export: <https://huggingface.co/g-ronimo/lama>
- ONNX Runtime: <https://github.com/microsoft/onnxruntime/blob/main/LICENSE>;
  DirectML: <https://www.nuget.org/packages/Microsoft.AI.DirectML>;
  DirectML EP: <https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html>
- Research memo: [2026-11-15-m4-ai-removal-research.md](2026-11-15-m4-ai-removal-research.md)
