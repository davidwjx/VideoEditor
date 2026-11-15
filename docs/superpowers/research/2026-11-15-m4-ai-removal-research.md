# M4 Research: Local AI Video Object Removal

- Date: 2026-11-15
- Status: Decision memo for M4 planning (read-only research; no source modified)
- Scope: interactive segmentation/tracking, video inpainting/object removal, licensing gate, ONNX Runtime + DirectML inference, M2 timeline/M3 export integration, UX, risks, and a 10-task implementation outline.
- Product spec: `docs/superpowers/specs/2026-11-14-video-editor-design.md` §3.1, §4.6, §5.5, §7.3, §9.3, §11–§12.
- Architectural baseline inspected: `src/core` (Project, Timeline, TimelineEvaluator, ProjectSerializer, commands), `src/media/render` (TimelineCompositor, LayerCompositor, ClipVideoSource, OfflineFrameRenderer), `src/media/decode`, `src/media/cache`, `src/media/proxy`, `src/ui/PreviewController.h`, `src/app/*.qml`.

---

## 0. Decision Summary

| Decision | Recommendation |
|---|---|
| **Tracking model** | **SAM 2.1 Hiera-Tiny** (primary) with **EfficientTAM-Ti 512** as the ONNX-first fallback. **MobileSAM** only as a last-resort image-only tracker if video-memory ONNX cannot run on DirectML. |
| **Inpainting model** | **MI-GAN 512** (fast path) + **LaMa 512** (quality path), both ONNX, with a project-owned temporal stabilization pass. Do **not** ship ProPainter/E2FGVI/MAT/DiffuEraser/MiniMax-Remover. |
| **Inference stack** | ONNX Runtime with **DirectML EP + CPU EP** on Windows/AMD; CUDA EP optional for NVIDIA QA. Keep an execution-provider abstraction, because DirectML is in maintenance mode and Microsoft is steering new work to WinML. |
| **Process model** | AI runs in the existing spec-mandated **separate subprocess** (`AI Engine`), not in the UI or compositor process. It communicates via local IPC and writes content-addressed cache files. No frame upload, no network. |
| **Architecture fit** | Add `RemovalLayer` to the Project schema (v3), attach it to `VideoLayer` in `TimelineEvaluator`, and teach `TimelineCompositor` to draw a cache-backed repair source instead of the decoded source. Preview and M3 export then share one path by construction. |
| **Recommended M4 scope** | 1080p SDR; user-painted/boxed mask; static/rigid targets and **slow-to-medium moving targets**; confidence display; keyframe correction; partial redo; lossless/patch cache; preview compare; export preflight. |
| **Explicit non-goals** | ProPainter-class temporal quality, diffusion/generative fill, fast complex occlusion, 4K/HDR, multi-object joint removal, auto object detection, cloud, model training/fine-tuning. |
| **Licensing verdict** | Engineering-safe shortlist exists (Apache-2.0/MIT code and weights), but **every practical model has a training-data provenance caveat** (SA-1B research-only, Places2 non-commercial, YouTube-VOS non-commercial). “Safe” here means “no known code/weights restriction”; it is **conditional on legal sign-off** for training data and on the exact redistribution obligations. |

**Bottom line:** M4 is feasible as *tracked mask + per-frame inpainting + temporal stabilization*, not as a ProPainter-class video inpainting product. The single biggest technical risk is not the inpainting model; it is making SAM 2/EfficientTAM’s split ONNX graph run correctly and fast on an AMD DirectML stack. The single biggest business risk is training-data licensing, not source-code licensing.

---

## 1. Baseline and Integration Constraints

### 1.1 What exists today

| Layer | Current design | Consequence for M4 |
|---|---|---|
| Project model | `Project` is a value type: `name`, `TimelineSettings`, `assets`, `tracks`, `subtitleStyles`; schema v2 (`src/core/Project.h`, `ProjectSerializer.cpp`). | Add `removalLayers` and a schema v3 migration; keep value semantics so preview/export snapshots stay copyable. |
| Timeline evaluation | `TimelineEvaluator::frameAt(us)` returns `FrameComposition { video[], subtitles[] }`; video layers are bottom-to-top. | Add a removal-layer reference to `VideoLayer`; no new mutable global state. |
| Composition | `TimelineCompositor` owns one D3D11 device, `ClipVideoSource` per clip, `LayerCompositor`, and subtitle rasterization. Preview (`TimelineRenderWorker`) and M3 export (`OfflineFrameRenderer`) both drive it. | The repair layer must be drawn by this same class. Do not create an “AI-only” compositor, or preview/export will diverge. |
| Source normalization | `ClipVideoSource` decodes and normalizes to RGBA; hardware NV12 and software frames converge here. | A repair cache source should implement the same “RGBA texture + timestamp” contract, ideally by reusing the decoder + converter path. |
| Cache | `CacheStore` currently exposes proxy paths, size, and clear; content-addressed by asset fingerprint. | Extend it with a `removal/` namespace, manifests, and GC. Masks are **not** cache and must not live under the clearable cache root. |
| Undo | `ProjectDocument` owns a `QUndoStack`; `TimelineEditCommand` edits tracks/clips/cues. | Add a `RemovalLayerCommand` (or a generic project-edit command) that stores before/after removal-layer and mask-hash state. |
| Export | M3 plan: freeze a project snapshot, render via `OfflineFrameRenderer`, encode CFR, checkpoint by segments. | Export must use only pre-generated repair cache; never start inference mid-export. Preflight must detect missing/stale cache. |
| UI | Qt Quick; `PreviewController` exposes timeline mode; `InspectorPanel` edits the selected clip; strings are Chinese. | Add a dedicated removal panel/overlay and a compare control; keep existing M2/M3 controls intact. |

### 1.2 Hard constraints carried into M4

- Original media remains read-only; repair results are **generation layers** with cache references, not edits to source files.
- All inference is local. No upload, no cloud API, no telemetry containing frames.
- 1080p SDR/BT.709 is the performance baseline. 4K is not an M4 requirement.
- Preview and export must share the same composition function; any repair compositing must be identical in both.
- Errors are user-facing Chinese messages explaining cause, impact, and next action.
- Unit tests must not require a GPU or model download; GPU/path tests use `GTEST_SKIP()` with a reason.

---

## 2. Candidate Models

### 2.1 Interactive segmentation and tracking

The product needs two distinct capabilities that are often conflated:

1. **Promptable segmentation** on a reference frame (brush/box/click -> mask).
2. **Mask propagation** across frames, with per-frame confidence and occlusion awareness, plus the ability to correct a keyframe and re-propagate a segment.

SAM 2 and its efficient derivatives provide both; MobileSAM provides only the first and needs an external propagation mechanism.

| Model | Code / weights license | ONNX status | Size / VRAM (reported) | Speed (reported / estimate) | Strengths | Limits | M4 verdict |
|---|---|---|---|---|---|---|---|
| **SAM 2.1 Hiera-Tiny** (Meta) | **Apache-2.0** for code and checkpoints ([repo](https://github.com/facebookresearch/sam2), [license summary](https://deepwiki.com/huggingface/segment-anything-2/7-legal-and-governance)). Training data includes SA-1B (research-only), YouTube-VOS (CC BY-NC-SA), DAVIS (BSD), MOSE, and internal data ([SAM 2 paper](https://arxiv.org/abs/2408.00714)). | **No official monolithic export.** Community splits into `image_encoder`, `image_decoder`, `memory_attention`, `memory_encoder`; C++/DirectML wrapper exists ([sam2-onnx-cpp](https://github.com/pagarcia/sam2-onnx-cpp)). Complex-number RoPE in memory attention is the main obstacle; AMD also publishes a 3-stage ONNX pipeline ([AMD-PAVS-AI/sam2](https://huggingface.co/AMD-PAVS-AI/sam2)). | Checkpoint ~148 MB; ~39 M params. Runtime VRAM at 1024² is **not publicly reported for DML**; plan for ~2–4 GB peak (estimate to be measured). | ~91 FPS on A100 at 1024² in bf16 ([SAM 2 speed summary](https://aiwiki.ai/wiki/sam_2)). On a consumer AMD GPU via DirectML, expect **5–20 FPS tracking** as an estimate, not a guarantee. | Best-in-class promptable video segmentation; memory bank; predicted-IoU and occlusion scores; multi-object; strong small-object behavior at 1024². | Export complexity; DirectML operator risk; long-video drift from greedy memory selection; 1024² input loses very small objects; VRAM. | **Primary tracking candidate**, gated by a DirectML spike. Start with Tiny; Small only if quality requires it. |
| **SAM 2.1 Hiera-Small** | Same Apache-2.0 and same data caveat. | Same split-export approach. | Checkpoint ~176–184 MB; ~46 M params. | Slower than Tiny but still real-time on a datacenter GPU; consumer DML estimate **3–10 FPS**. | Better mask quality than Tiny; same API. | More VRAM and latency; same export/drift issues. | **Quality option** if Tiny’s J&F on the M4 corpus is insufficient. |
| **EfficientTAM-Ti 512** (Meta/“Efficient Track Anything”) | **Apache-2.0** for code and checkpoints ([repo](https://github.com/yformer/EfficientTAM), [HF weights](https://huggingface.co/yunyangx/efficient-track-anything)). Trained on SA-V and related video data; verify the exact mixture with legal. | **Ready 5-session ONNX bundle**: `image_encoder`, `prompt_encoder`, `mask_decoder`, `memory_encoder`, `memory_attention`; fp32 ~77 MB / fp16 ~40 MB ([egordm/efficienttam-ti-512](https://huggingface.co/egordm/efficienttam-ti-512)). | ~40 MB fp16; ~<1 GB VRAM at 512² (estimate). | ~28 FPS on iPhone 15 Pro Max at 512² (ICCV 2025 version) ([paper](https://openaccess.thecvf.com/content/ICCV2025/papers/Xiong_Efficient_Track_Anything_ICCV_2025_paper.pdf)); desktop AMD should be comfortably faster. | Designed for on-device; ONNX bundle exists; low VRAM; good temporal memory efficiency. | 512² input is weaker for small objects in 1080p; lower accuracy than SAM 2.1 Tiny on SA-V (68.6 vs 76.5 J&F reported for the mobile variant). | **Primary fallback / ONNX-first choice.** Spike against SAM 2 Tiny; choose by measured J&F and DML speed. |
| **EdgeTAM** (Meta, CVPR 2025) | **Apache-2.0** ([repo](https://github.com/facebookresearch/EdgeTAM)). | ONNX/PTQ toolchain exists from D-Robotics ([EdgeTAM_PTQ](https://github.com/D-Robotics-AI-Lab/EdgeTAM_PTQ)). | Small, on-device oriented. | EfficientTAM-Mobile reports ~1.8× faster than EdgeTAM on iPhone 15 Pro Max, with +1.5 J&F. | Mature on-device lineage; Apache-2.0. | Less accurate than EfficientTAM-Mobile; another model to package. | **Optional third candidate**; not needed if SAM 2 Tiny and EfficientTAM pass. |
| **MobileSAM** | **Apache-2.0** for code and weights ([repo](https://github.com/ChaoningZhang/MobileSAM); third-party confirmation [LibreYOLO](https://github.com/LibreYOLO/libreyolo/pull/471)). Trained via distillation from SAM on SA-1B; data caveat. | Easy: encoder + decoder ONNX; no video memory. | ~9.66 M params total; checkpoint ~40 MB. | Encoder ~8 ms, full image pipeline ~12 ms on a GPU ([MobileSAM README summary](https://huggingface.co/dhkim2810/MobileSAM)). | Very fast, small, trivial ONNX; good interactive mask on a single frame. | **Image-only**; no temporal memory. Propagating masks requires optical flow or per-frame prompting, which accumulates drift. | **Last-resort fallback** if SAM 2/EfficientTAM ONNX cannot run on the target AMD stack. Expect visibly worse tracking. |
| **Cutie** (CVPR 2024) | **MIT** for the core VOS framework ([repo](https://github.com/hkchengrex/Cutie)); GUI bundles RITM under a separate license. Trained on YouTube-VOS/DAVIS (data caveat). | No maintained ONNX export found; PyTorch-oriented. | ~? (not published in a form suitable for planning). | Research-grade; not benchmarked for DirectML. | Strong object-level video segmentation with memory; MIT code. | No ONNX; PyTorch dependency; data risk; more engineering. | **Research alternative**, not M4. |
| **XMem** | **GPL-3.0** ([repo listing](https://libraries.io/github/hkchengrex), [Supervisely listing](https://ecosystem.supervisely.com/ecosystem/apps/xmem/supervisely_integration/serve)). | No. | — | — | Long-term VOS memory. | GPL-3.0 is incompatible with a closed-source commercial application unless the whole distributed work is GPL. | **Exclude.** |
| **SAM 3** | Custom **Meta SAM License**, not Apache-2.0; acceptable-use restrictions ([comparison](https://playground.roboflow.com/models/compare/sam-2-vs-sam-3)). | Emerging. | Larger. | — | Newer segmentation. | License is not the clean Apache-2.0 grant of SAM 2; redistribution obligations differ. | **Exclude from M4**; revisit only with legal review. |

**Tracking recommendation:** spike **SAM 2.1 Hiera-Tiny** and **EfficientTAM-Ti** in parallel on the actual AMD GPU. Select one as the shipped default. Keep the tracking interface model-agnostic (`segment(referenceFrame, prompt)`, `propagate(segment, direction, confidenceOut)`) so the fallback is a configuration change, not a rewrite.

**Known SAM 2 failure mode to design around:** SAM 2’s memory is greedily updated with recent frames; a bad/occluded mask enters memory and contaminates later predictions. This is documented and addressed by SAM2Long, SAMURAI, AS-SAM2, and confidence-gated memory updates ([SAM2Long](https://arxiv.org/abs/2410.16268), [SAMURAI](https://ieeexplore.ieee.org/document/11351313)). M4 should not implement a full memory-tree search; it should:
- gate memory writes on predicted-IoU / occlusion / mask-area stability,
- stop propagation and surface a low-confidence state rather than silently drifting,
- let the user correct a keyframe and reset memory at that frame.

### 2.2 Video inpainting / object removal

The key distinction is **image inpainting applied per frame** (MI-GAN, LaMa) versus **temporally aware video inpainting** (ProPainter, E2FGVI, FGT, STTN, diffusion models). Image models are far easier to export and run, but produce flicker because they re-synthesize the hole independently each frame. Temporal models produce better coherence but are memory-hungry, slow, and mostly non-commercial or not exportable.

| Model | Code / weights license | ONNX status | Size / VRAM (reported) | Speed at 1080p (reported / estimate) | Strengths | Limits | M4 verdict |
|---|---|---|---|---|---|---|---|
| **MI-GAN 512** (Picsart, ICCV 2023) | **MIT** code; weights under a separate `LICENSE-WEIGHTS` described as MIT ([repo](https://github.com/Picsart-AI-Research/MI-GAN), [weights analysis](https://hysenlabs.com/projects/picsart-ai-research-mi-gan)). Trained on **Places2**, which is non-commercial/no-redistribution ([Places2 terms](https://places.csail.mit.edu/)). | **Official ONNX pipeline** (`scripts/create_onnx_pipeline.py`); accepts arbitrary-resolution image + mask; prebuilt `migan_pipeline_v2.onnx` exists ([repo](https://github.com/Picsart-AI-Research/MI-GAN), [ONNX runtime reference](https://github.com/PyModel/watermark-remover/blob/main/skills/remove-ai-marks/scripts/migan_fill.py)). | ~16 MB fp16 / ~28 MB ONNX. VRAM <1 GB at 512² (estimate). | ~6 ms per 512² on a Pixel 8a GPU best case; ~22–27 ms p50 on flagship mobile/browser ([LiteRT benchmark](https://huggingface.co/litert-community/MI-GAN-512-Places2-LiteRT)). Desktop GPU: **<10–30 ms per tile** (estimate). | Very fast, tiny, ONNX-native, crops around the mask so cost scales with mask size, good for logos/watermarks/small objects. | Weaker on large holes and complex textures; per-frame flicker; Places2 training-data risk. | **Default fast path.** |
| **LaMa / Big-LaMa** (Samsung, WACV 2022) | **Apache-2.0** code ([repo](https://github.com/advimman/lama)); community ONNX weights tagged Apache-2.0 ([g-ronimo/lama](https://huggingface.co/g-ronimo/lama)). Trained on **Places2** (non-commercial) — data caveat. | **ONNX exports exist but are fixed at 512²**; dynamic shapes require resolving FFC `irfft/rfftn` padding ([Carve/LaMa-ONNX](https://huggingface.co/Carve/LaMa-ONNX)). Practical approach: reflect-pad and run 512² tiles. | fp32 ~209 MB, fp16 ~110 MB, int8 ~61.5 MB ([g-ronimo/lama](https://huggingface.co/g-ronimo/lama)). VRAM ~1–2 GB at 512² (estimate). | ~2.1 s/frame CPU at 1080p; ~430 ms per 512² on WebGPU/M-series ([CPU benchmark](https://hackcv.com/en/posts/practice-erase-cpu-benchmark/), [ONNX benchmark](https://huggingface.co/g-ronimo/lama)). RTX 3060 estimate **60–150 ms per 512² tile**; a 1080p frame may need 4–9 tiles. | Better large-hole quality than MI-GAN; resolution-robust Fourier convolutions; mature and widely deployed. | Fixed 512² ONNX; tiling seams; per-frame flicker; Places2 data risk; slower than MI-GAN. | **Quality path.** |
| **FGT** (ECCV 2022) | **MIT** code and weights ([repo](https://github.com/hitachinsk/FGT), [HF weights](https://huggingface.co/hitachinsk/FGT)). Trained on YouTube-VOS/DAVIS (data caveat). | No maintained ONNX export found; PyTorch + RAFT optical flow. | Not publicly packaged; activation memory is the issue. | **~1.89 s/frame at low resolution**; **unable to run 720p/1080p even on an 80 GB A100** due to memory ([Glasgow thesis benchmark](https://theses.gla.ac.uk/84828/1/2024LiShiboPhD.pdf)). | Temporal coherence; flow-guided transformer. | Not practical at 1080p; no ONNX; data risk; heavy dependency. | **Research spike only**; do not put in M4 critical path. |
| **STTN** (ECCV 2020) | **MIT** code ([repo](https://github.com/researchmm/STTN)); pretrained weights trained on YouTube-VOS/DAVIS (data caveat). | No maintained ONNX export found; manual `torch.onnx.export` would be required. | ~?; original STTN fits only **13 frames in 16 GB** VRAM; FastSTTN variant handles ~123 frames in 16 GB ([FastSTTN](http://sibgrapi.sid.inpe.br/col/sid.inpe.br/sibgrapi/2021/09.06.22.26/doc/FastSTTN___SIBGRAPI_2021.pdf)). | ~24.3 FPS on V100 / ~10.4 FPS on P100 at benchmark resolution; community reports a 5-minute 1080p video processed in ~5 minutes on an RTX 3080 Ti ([repo/community summary](https://github.com/researchmm/STTN)). | Temporal transformer; MIT code; faster than ProPainter in some reports. | Older quality; memory scales badly with frame count; no ONNX; data risk; unverified 1080p behavior. | **Optional research spike**; not M4 default. |
| **ProPainter** (ICCV 2023) | **NTU S-Lab License 1.0 — non-commercial only**; commercial use requires contacting the authors ([LICENSE](https://github.com/sczhou/ProPainter/blob/main/LICENSE), [licensing summary](https://deepwiki.com/halfzm/ProPainter-Webui/7.2-licensing)). | No official/community ONNX export found; PyTorch + RAFT. | 720p @ 50 frames: **28 GB fp32 / 19 GB fp16**; 80 frames: OOM / 25 GB ([memory table](https://deepwiki.com/sczhou/ProPainter/4.2-memory-optimization)). 1080p is not in the official table; effectively OOM on consumer cards without heavy downscaling/tiling. | ~0.249 s/frame at 480p on V100; 1080p 10 s clip ~3–5 min on RTX 4090/V100-class, ~8–15 min on RTX 3060 8 GB ([community benchmark](https://maomu.com/a/propainter-video-object-removal)). | Best-in-class open video inpainting quality; temporal propagation; strong object removal. | Non-commercial; memory-heavy; no ONNX; slow; complex flow pipeline. | **Exclude** unless a commercial license is negotiated and a separate engineering track funds ONNX/TensorRT work. |
| **E2FGVI** (CVPR 2022) | **CC BY-NC 4.0 — non-commercial only** ([license change commit](https://github.com/MCG-NKU/E2FGVI/commit/924b56c133fffe37327f9c9b90290fc3d0538581)). | No ONNX export found. | High memory; similar class to FGT. | Research-grade; not 1080p-practical on consumer GPU. | Flow-guided end-to-end video inpainting. | Non-commercial; no ONNX; memory. | **Exclude.** |
| **MAT** (CVPR 2022) | **Research only**; code/license explicitly non-commercial; built on StyleGAN2-ADA (NVIDIA non-commercial) ([repo](https://github.com/fenglinglwb/MAT), [HF mirror](https://huggingface.co/Acly/MAT)). | No. | — | — | High-quality large-hole image inpainting. | Non-commercial; image-only; StyleGAN dependency. | **Exclude.** |
| **DiffuEraser** (2025) | **Apache-2.0 code**, but default pipeline uses **ProPainter as prior** (non-commercial) and Stable Diffusion 1.5 (OpenRAIL-M restrictions) ([repo](https://github.com/lixiaowen-xw/DiffuEraser)). | No ONNX; diffusion pipeline. | Heavy. | Slow; diffusion steps. | Good content completeness; temporal consistency. | Default dependency chain is not cleanly commercial; heavy; no ONNX. | **Exclude** unless the prior model is replaced and legal clears the SD weights. |
| **MiniMax-Remover** (2025) | **CC-BY-NC-4.0** for official weights ([HF model card](https://huggingface.co/zibojia/minimax-remover)); a third-party mirror claims Apache-2.0 but is unverified ([mirror](https://huggingface.co/AEmotionStudio/minimax-remover)). | No ONNX; diffusion. | Heavy. | 6 inference steps, but still diffusion-class. | Strong object removal. | **Non-commercial weights**; license ambiguity; heavy. | **Exclude.** |
| **Netflix VOID** (2026) | **Apache-2.0** code and checkpoints ([HF](https://huggingface.co/netflix/void-model), [GitHub](https://github.com/Netflix/void-model)). | No ONNX; 5B diffusion transformer. | **40 GB+ VRAM** recommended (A100-class); community Q5 quantization claims 24 GB ([VOID summary](https://mindpattern.ai/f/5763)). | Diffusion; not real-time; default 384×672, up to 197 frames. | Commercially licensed; physics/interaction-aware removal; beats ProPainter in human preference. | Far outside consumer-GPU M4 scope; 5B model; no ONNX; supplied mask pipeline uses Gemini API (not local-only). | **Exclude from M4**; track as a future high-end/offline option if a 24 GB+ GPU tier appears. |
| **COCOCO** (AAAI 2025) | **License not confirmed**; built on AnimateDiff + SD inpainting + SAM2 + ProPainter, so inherits non-commercial/restricted components ([repo](https://github.com/zibojia/COCOCO)). | No. | Heavy. | Diffusion. | Text-guided video inpainting. | Unclear license; non-commercial deps; heavy. | **Exclude.** |
| **ZITS** (CVPR 2022) | Code license exists but weights/training data are Places2 and unclear; third-party audits classify it as “fine-tune/retrain only” ([license audit](https://github.com/pdcgomes/redlamp/blob/f635efd4139d7f4798432ccb94448f057bd6803c/docs/research/notes/D-removal.md)). | Some ONNX/OpenVINO ports exist, not maintained for video. | ~? | Image-only. | Structure-aware large-hole image inpainting. | Unclear weights license; image-only; Places2. | **Exclude** for M4. |

**Inpainting recommendation:** ship a **two-model portfolio**:
- **MI-GAN fast path** for small/medium masks, logos, watermarks, and preview/quick results.
- **LaMa quality path** for larger holes and textured backgrounds.
- Both run per frame; add a **project-owned temporal stabilization** stage (flow-warped blend of neighboring repaired frames, color matching, optional temporal median in the hole). This is the only practical way to reduce flicker without a commercial temporal model.
- Treat FGT/STTN as **research spikes**, not M4 deliverables. If a spike proves a 1080p path within the VRAM/time budget, it can become an “experimental quality” mode in M5.

---

## 3. Licensing Gate

### 3.1 The important distinction

There are **three** licenses to check for each model:

1. **Source-code license** — can we link/redistribute the code?
2. **Weights license** — can we redistribute the trained checkpoint/ONNX export?
3. **Training-data terms** — do the datasets on which the weights were trained restrict commercial use or redistribution?

Most public discussions stop at (1) and (2). The M4 decision must include (3), because the practical candidates are trained on datasets with explicit restrictions.

### 3.2 Tier classification

| Tier | Meaning | Models |
|---|---|---|
| **A — engineering-safe** | Code and weights have an explicit permissive license (Apache-2.0/MIT/BSD). No known restriction on commercial use or redistribution. Training-data caveat still applies. | SAM 2.1 (Apache-2.0), EfficientTAM (Apache-2.0), EdgeTAM (Apache-2.0), MobileSAM (Apache-2.0), Cutie (MIT), MI-GAN (MIT), LaMa (Apache-2.0), FGT (MIT), STTN (MIT), Netflix VOID (Apache-2.0). |
| **B — non-commercial / restricted** | Explicit non-commercial, research-only, copyleft, or unclear terms. | ProPainter (NTU S-Lab 1.0), E2FGVI (CC BY-NC 4.0), MAT (research only), MiniMax-Remover (CC-BY-NC-4.0), DiffuEraser default (ProPainter + SD1.5), XMem (GPL-3.0), SAM 3 (custom Meta license), COCOCO (unclear), ZITS (unclear weights/data). |

### 3.3 Training-data risk register

| Dataset | Terms | Models affected | Risk |
|---|---|---|---|
| **SA-1B** (SAM image data) | Custom **research-only** license; prohibits commercial/production use and derivatives for commercial purposes ([SA-1B license summary](https://huggingface.co/datasets/xiuqhou/SA-Det-100k/blob/main/LICENSE), [TFDS catalog](https://www.tensorflow.org/datasets/catalog/segment_anything)). | SAM 2 (15.5% of training mixture), MobileSAM (distilled from SAM), possibly EfficientTAM/EdgeTAM. | **High.** The released SAM 2 weights are Apache-2.0, but SA-1B is research-only. Legal must decide whether Meta’s Apache grant is sufficient or whether the weights inherit the data restriction. |
| **Places2** | Non-commercial research/education only; no redistribution; no published commercial license ([Places2 terms](https://places.csail.mit.edu/)). | LaMa, MI-GAN, ZITS, many inpainting models. | **High.** This is the core inpainting training set. Apache/MIT weights do not erase the dataset terms in many legal analyses ([analysis](https://dev.to/phpboyscout/the-weights-were-mit-the-training-data-wasnt-206d)). |
| **YouTube-VOS** | **CC BY-NC-SA 4.0** (non-commercial, share-alike) ([dataset site](https://youtube-vos.org/dataset/vos/), [license listing](https://tianchi.aliyun.com/dataset/91865)). | STTN, FGT, Cutie, SAM 2 (9.2% mixture), many VOS models. | **High.** ShareAlike could propagate to derived weights; non-commercial use is explicit. |
| **DAVIS** | **BSD-3-Clause** per official repo, which permits commercial use with notices ([DAVIS README](https://github.com/fperazzi/davis-2017), [LICENSE_DAVIS](https://github.com/KevinXu02/segment-anything-2/blob/main/sav_dataset/LICENSE_DAVIS)). Underlying YouTube video rights may be separate. | STTN, FGT, Cutie, SAM 2 (1.3% mixture). | **Medium.** BSD is permissive, but source-video rights are a separate question; confirm with legal. |
| **MOSE / internal Meta video** | Not fully public; internal data terms unknown. | SAM 2 (9.4% MOSE + 15.1% internal). | **Unknown.** Legal must obtain details or rely on Meta’s Apache-2.0 grant for the weights. |
| **Places2 / FFHQ (MI-GAN)** | Places2 non-commercial; FFHQ per-image CC licenses include BY-NC. | MI-GAN. | **High.** Even the MIT `LICENSE-WEIGHTS` does not change the dataset terms. |

### 3.4 What “safe shortlist” means

**Engineering-safe shortlist (conditional on legal sign-off):**

- **Tracking:** SAM 2.1 Hiera-Tiny/Small (Apache-2.0), EfficientTAM-Ti (Apache-2.0), MobileSAM (Apache-2.0) as last resort.
- **Inpainting:** MI-GAN (MIT), LaMa (Apache-2.0).
- **Not in shortlist:** ProPainter, E2FGVI, MAT, DiffuEraser, MiniMax-Remover, XMem, SAM 3, COCOCO, ZITS.

This shortlist has **no known code/weights restriction**. It does **not** have clean training-data provenance. Do not describe it as “legally clean” in product or investor materials until legal signs off.

### 3.5 What must be verified with legal

1. **Weight-vs-data doctrine:** Does an Apache-2.0/MIT grant from the publisher override the training-data terms (SA-1B, Places2, YouTube-VOS) in our target jurisdictions? If not, the shortlist is not shippable without further action.
2. **Places2:** Is there any commercial license path? If not, can we retrain/fine-tune LaMa/MI-GAN on owned or permissively licensed inpainting data? What is the cost/timeline?
3. **SA-1B / YouTube-VOS:** Are the SAM 2 weights a “derivative” of those datasets? Does Meta’s Apache-2.0 grant cover commercial redistribution of ONNX-converted weights? Does YouTube-VOS ShareAlike propagate?
4. **Redistribution obligations:** Apache-2.0 NOTICE/LICENSE, marking modified files (ONNX exports are modified/derived), MIT copyright notices, and attribution in an About/Licenses dialog.
5. **ONNX Runtime / DirectML:** ONNX Runtime is MIT but bundles third-party notices; DirectML is a Microsoft redistributable with its own license terms. Include the official `ThirdPartyNotices.txt` and DirectML terms ([ORT license](https://github.com/microsoft/onnxruntime/blob/main/LICENSE), [DirectML NuGet](https://www.nuget.org/packages/Microsoft.AI.DirectML)).
6. **Trademarks/model names:** Do not imply Meta/Netflix/Picsart endorsement; use factual model names in the licenses dialog, not in the main UI.
7. **Commercial ProPainter license:** If quality is non-negotiable, contact the authors for a quote ([LICENSE](https://github.com/sczhou/ProPainter/blob/main/LICENSE)). Budget for a separate ONNX/TensorRT engineering track; the license alone does not solve 1080p memory/speed.
8. **Patent review:** Segmentation/inpainting model architectures are generally published; confirm no known patent encumbrance for the selected models and for ONNX Runtime/DirectML.

**Recommendation:** start legal review **in parallel with the tracking/inpainting spike**, not after it. If legal cannot accept the training-data risk, the fallback is:
- license/retrain on owned data (expensive, slow), or
- negotiate a commercial ProPainter license (quality path, but heavy engineering), or
- ship M4 without AI removal and move the feature to M5/cloud (product impact).

---

## 4. Inference Stack

### 4.1 Execution-provider matrix

| Provider | Hardware | Windows support | M4 role | Notes |
|---|---|---|---|---|
| **DirectML (DML)** | AMD, NVIDIA, Intel, Qualcomm | Yes (DX12) | **Primary AMD path** | Cross-vendor; D3D12-based; static shapes; opset ≤20; some ops unsupported; maintenance mode ([official docs](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)). |
| **CPU** | Any x86-64 | Yes | **Always-present fallback** and fallback for unsupported DML nodes | Slow for LaMa (~2.1 s/frame at 1080p) but deterministic; essential for tests and low-VRAM machines. |
| **CUDA** | NVIDIA only | Yes | Optional QA/quality path | Not applicable to AMD; useful for comparing DML correctness/perf ([EP overview](https://onnxruntime.ai/docs/execution-providers/)). |
| **TensorRT** | NVIDIA only | Yes | Out of M4 | Extra packaging/complexity; not needed for AMD target. |
| **ROCm / MIGraphX** | AMD | Linux-oriented; not the Windows path | Out of M4 | AMD’s own SAM 2 ONNX work targets ROCm/MIGraphX on Linux ([AMD-PAVS-AI/sam2](https://huggingface.co/AMD-PAVS-AI/sam2)); do not assume it works on Windows. |
| **WinML** | Cross-vendor | Yes | Future migration | Microsoft is steering new Windows ML work here; keep the EP abstraction so a WinML backend can be added without touching model code. |

### 4.2 DirectML constraints that shape the design

- **Opset and operator coverage:** DirectML EP supports up to ONNX opset 20; `GridSample 20:5d` and `DeformConv` are not supported; higher opsets can perform poorly ([official docs](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)).
- **Static shapes:** DML works best with known shapes; dynamic batch/shape graphs can be slow or fail. Use fixed 512²/1024² inputs and `AddFreeDimensionOverride` only where necessary.
- **Sequential execution:** DML does not support memory-pattern optimization or parallel execution; set `execution_mode = ORT_SEQUENTIAL`, disable memory pattern/CPU arena as required ([DML setup notes](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)).
- **Correctness bugs in transformer/attention ops:** Community reports include `GroupQueryAttention` wrong logits on RDNA2 ([#29739](https://github.com/microsoft/onnxruntime/issues/29739)) and `Split→Sigmoid→Mul` (GLU) miscomputation on RX 7800 XT ([#32146](https://github.com/microsoft/onnxruntime/issues/32146)). SAM 2’s memory attention is exactly the kind of graph that needs numerical validation against CPU.
- **Batched readback bugs:** A DML batched-readback size overflow was fixed in ORT PR #28680 ([PR](https://github.com/microsoft/onnxruntime/pull/28680)). Do not rely on batch > 1 without a regression test. Prefer batch = 1.
- **Memory behavior:** DML can fall back to shared system memory when VRAM is exhausted, causing 10–100× slowdowns; `E_OUTOFMEMORY` and `DEVICE_REMOVED/HUNG/RESET` are the failure codes to handle ([field report](https://pixinsight.com/forum/index.php?threads/pi-1-9-6-on-windows-11-mldenoise-not-releasing-gpu-memory-after-completing-task.27615/), [ORT DML tiled memory PR](https://github.com/microsoft/onnxruntime/pull/20303)).
- **D3D12 vs D3D11:** DirectML runs on a D3D12 device; the compositor is D3D11. Zero-copy interop is possible only with shared textures (no mipmaps, `SHARED_NTHANDLE`, shared fence) ([GPUOpen guide](https://gpuopen.com/learn/onnx-directlml-execution-provider-guide-part2/)). **M4 should not attempt zero-copy.** The AI subprocess decodes its own frames via FFmpeg/CPU or receives raw frames over IPC; the compositor reads cached repair frames. This avoids a D3D11↔D3D12 synchronization project inside M4.

### 4.3 Recommended EP policy

```
Session options:
  graph_optimization_level = ORT_ENABLE_ALL
  execution_mode = ORT_SEQUENTIAL
  enable_mem_pattern = false          // DML requirement
  enable_cpu_mem_arena = false        // DML requirement
  intra_op_num_threads = 0            // let ORT choose for CPU fallback
  log_severity_level = WARNING

EP order:
  1. DirectML (device 0, gpu_mem_limit = measured budget)
  2. CPU (for unsupported nodes / fallback)
```

At session creation:
1. Enumerate adapters with DXGI; select the high-performance adapter.
2. Try DML fp16 first (if the model is numerically safe at fp16); validate a reference output against CPU.
3. If session creation fails with `E_OUTOFMEMORY` / `DEVICE_REMOVED`, retry DML fp32, then CPU.
4. Log which nodes were assigned to CPU; if a heavy node (e.g., attention) falls back, warn the user about the performance impact.

Do **not** silently accept a full CPU fallback on a GPU machine. Show: “当前显卡无法加速该模型，已改用 CPU，预计耗时增加 N 倍。”

### 4.4 Fallback ladder for insufficient VRAM

| Tier | Action | Expected effect |
|---|---|---|
| 0 | DML fp16, full resolution | Fastest; baseline |
| 1 | DML fp32 | More VRAM, slightly slower; use if fp16 correctness fails |
| 2 | Reduce SAM 2 memory bank (`num_maskmem` 5→3) and/or input 1024→768/512 | Lower VRAM, lower small-object accuracy |
| 3 | Crop/tile around the mask (inpainting 512² tiles; tracking ROI) | VRAM scales with crop, not frame |
| 4 | Chunk frames (16–32 frames) with 4–8 frame overlap | VRAM independent of clip length; slight boundary cost |
| 5 | CPU fp32 | Works anywhere; LaMa ~2.1 s/frame at 1080p; may exceed the 10-minute target |
| 6 | Pause with checkpoint | Preserve progress; user can lower resolution or close other GPU apps |

Implementation notes:
- Query `IDXGIAdapter3::QueryVideoMemoryInfo` before the job; set a conservative budget (e.g., 70% of available).
- Catch `E_OUTOFMEMORY` and device-lost HRESULTs at the session/run boundary; recreate the subprocess on device loss.
- Never let the UI process allocate model tensors; all VRAM lives in the AI subprocess.

### 4.5 Frame batching, tiling, and caching

- **Batch = 1.** SAM 2 tracking is inherently sequential (memory attention over previous frames); inpainting can tile but should not batch tiles on DML until a regression test proves correctness.
- **Tracking tiling:** run the SAM 2 image encoder on the full frame downscaled to 1024². For very small objects (e.g., a distant logo), crop a padded ROI around the prompt and run at 1024²; this improves mask quality more than increasing global resolution.
- **Inpainting tiling:** crop a padded bounding box around the mask, run MI-GAN/LaMa at 512² (or 512×768), paste back with feathered alpha. This is both faster and lower-VRAM than full-frame inpainting. For masks larger than 512², tile with 32–64 px overlap and blend.
- **Model caching:** create sessions once per subprocess; warm up with one dummy inference; keep sessions alive across chunks. Unload the tracking session before loading the inpainting session if peak VRAM is tight (sequential phases).
- **Data caching:**
  - Persistent: repair cache chunks, mask keyframes, tracking confidence, manifests.
  - In-memory only: SAM 2 image embeddings for the current range (allow fast mask edits without re-encoding), LRU-bounded.
  - Never persist raw decoded frames unless needed for resume; they are large and reproducible.
- **Chunking:** 16–32 frames per chunk with 4–8 frame overlap, aligned to source frame boundaries. Each chunk is independently cacheable and resumable.

### 4.6 AI subprocess protocol

Use a local IPC channel (QLocalSocket/named pipe) with newline-delimited JSON messages. Suggested protocol:

```json
{"cmd":"start","jobId":"...","clipId":"...","assetPath":"...","sourceInUs":0,"sourceOutUs":10000000,
 "maskKeyframes":[{"timeUs":0,"maskPath":"...","featherPx":4}],
 "params":{"mode":"fast","temporalStabilization":true,"processingWidth":1920,"processingHeight":1080},
 "modelIds":{"tracking":"sam2.1-tiny@1.0.0","inpaint":"migan-512@1.0.0"},
 "cacheDir":"..."}
```

Events: `progress` (stage, frame, percent, eta), `confidence` (per-frame array or range), `chunkDone` (path, hash), `warning`, `error`, `done`, `cancelled`.

Rules:
- The subprocess never reads the project file; it receives only a frozen job description.
- The subprocess writes to a temp directory and atomically renames completed chunks; a crash leaves only temp files.
- The main process owns the job queue and the project; it can cancel by closing the socket and terminating the child after a grace period.
- Resume uses the manifest + completed chunk list; it never trusts a partially written chunk.
- No network access; no telemetry; model files are loaded from the app installation directory and hash-verified.

---

## 5. Architecture Fit

### 5.1 Proposed data model (schema v3)

Extend `Project`:

```cpp
struct MaskKeyframe {
    QString id;
    int64_t sourceTimeUs = 0;     // source time, not timeline time
    QString maskRef;              // relative sidecar path + content hash
    int featherPx = 4;
    int revision = 1;
    bool operator==(const MaskKeyframe &) const = default;
};

struct RemovalLayer {
    QString id;
    QString clipId;
    int64_t sourceInUs = 0;
    int64_t sourceOutUs = 0;
    QList<MaskKeyframe> maskKeyframes;
    QString trackingState;         // complete | partial | lowConfidence | failed
    QString trackingConfidenceRef; // sidecar JSON with per-frame confidence/occlusion
    QString parametersJson;        // mode, quality, stabilization, resolution
    QString cacheKey;
    QString cachePath;             // relative to removal cache root
    int revision = 1;
    bool enabled = true;
    bool operator==(const RemovalLayer &) const = default;
};
```

Add `QList<RemovalLayer> removalLayers` to `Project` and `QString removalLayerId` to `Clip`. Bump `Project::kSchemaVersion` to 3; `ProjectSerializer::fromJson` accepts v1/v2 and migrates to an empty removal layer list. Validation:
- ids unique across layers;
- `clipId` exists and is a video clip;
- `sourceOutUs > sourceInUs` and within the clip’s asset duration;
- mask keyframes sorted by source time, unique ids, refs non-empty;
- cache key length/format;
- no removal layer on a non-video track.

**Mask storage decision:** masks are **user data**, not cache. Do not store them under `%LOCALAPPDATA%` cache that `CacheStore::clear()` deletes. Recommended layout:

```
<project>.veproj
<project>.veproj.assets/
  masks/<maskHash>.rle
  tracking/<trackingHash>.json
```

The project JSON stores hashes + relative paths. Save is atomic: write sidecars to temp files, rename, then write the project JSON via `QSaveFile`. On load, missing sidecars mark the layer `failed` with a clear message and allow re-painting; never silently drop the layer. This preserves the spec’s “cache cleanup must not affect project correctness” requirement.

**Mask encoding:** run-length encoded binary mask at source display resolution (or at a documented processing resolution with a transform). RLE is compact for typical object masks, deterministic, and easy to hash. Feather is a parameter, not baked into the mask, so changing feather does not require re-painting. If RLE proves unwieldy for complex masks, fall back to 1-bit PNG with the same hash/ref contract.

### 5.2 Tracking confidence

SAM 2 exposes a **predicted-IoU** score per mask and an **occlusion score** per object; both are useful but not calibrated ([SAM 2 paper](https://arxiv.org/abs/2408.00714), [confidence-use survey](https://isprs-archives.copernicus.org/articles/XLIX-B3-2026/205/2026/isprs-archives-XLIX-B3-2026-205-2026.pdf)). Store per-frame:

```json
{"frames":[{"timeUs":0,"iou":0.97,"occlusion":0.01,"areaPx":12034,"state":"good"}, ...]}
```

State mapping:
- `good`: iou ≥ 0.85 and occlusion ≤ 0.2 and area stable within ±30%.
- `fair`: iou ≥ 0.65 or area change 30–60%.
- `low`: iou < 0.65 or area change > 60%.
- `missing`: occlusion > 0.7 or empty mask for > N frames.

UI: a thin confidence band under the clip; green/yellow/red; click a red frame to jump and correct. The band is derived data; store it in the project (small) so it survives reload and undo. Do not treat predicted-IoU as ground truth; calibrate thresholds on the M4 corpus and let the user override.

### 5.3 Repair generation layer and compositor

`TimelineEvaluator::frameAt()` already returns `VideoLayer` for the active clip. Add:
- `QString removalLayerId` and `QString repairCachePath` (or a copy of the `RemovalLayer`).
- A helper `activeRemovalLayer(clip)` that checks `clip.enabled`, `removalLayer.enabled`, range coverage, and cache completeness.

`TimelineCompositor`:
- Add a `RepairSource` abstraction analogous to `ClipVideoSource`:
  - opens the cache chunk/manifest for a `RemovalLayer`;
  - `frameAt(sourceTimeUs)` returns an RGBA texture;
  - owns a decoder for the cache format (FFV1/MKV or H.264) and uses the same `VideoConverters`.
- In `drawFrame()`, when a layer has an active repair source and the cache is complete, draw the repair frame instead of the decoded frame. Apply the clip’s `Transform` and opacity to the repair frame, exactly as for the source. Subtitles remain topmost.
- If the repair cache is missing/stale in **preview**, draw the original and emit one warning per layer: “AI 修复结果缺失，当前显示原片；请重新生成。” Do **not** block preview.
- If the repair cache is missing/stale in **export**, fail preflight (see 5.7). Export must not silently output the original.

Why replace the whole frame instead of compositing a patch? For M4, whole-frame replacement is simpler and guarantees the repair layer’s transform/opacity behavior matches the source. The cache can still store only the mask-bounded patch plus alpha to save disk; the `RepairSource` composites the patch over the source frame at draw time. Recommended target: patch cache + alpha, with a full-frame fallback if patch compositing causes seams.

### 5.4 Cache key and manifest

Cache key (SHA-256 of a canonical byte string):

```
removal-v1
| assetFingerprint          // existing fingerprintFile(): size+mtime+head+tail
| sourceInUs
| sourceOutUs
| maskKeyframes: [timeUs, maskHash, featherPx, revision]
| parametersCanonicalJson   // mode, quality, stabilization, processingSize, fps
| trackingModelId@version + onnxHash
| inpaintModelId@version + onnxHash
| pipelineVersion
| colorSpace
| cacheKeyVersion
```

Notes:
- Do **not** include `timelineStartUs`: moving a clip on the timeline must not invalidate the repair cache.
- Do include `sourceIn/Out`, because the repair covers a source range.
- Include the model file hash, not just a human version string; otherwise a silent model update reuses stale results.
- Include the processing resolution and fps; a different sampling grid produces different results.
- Use a stable canonical JSON serializer (sorted keys, fixed number formatting) to avoid hash drift across Qt versions.

Manifest (`manifest.json`):
```json
{"cacheKey":"...","clipId":"...","sourceInUs":0,"sourceOutUs":10000000,
 "width":1920,"height":1080,"fps":{"num":30,"den":1},
 "modelIds":{...},"pipelineVersion":"m4.0",
 "chunks":[{"index":0,"startFrame":0,"frameCount":32,"path":"chunk_00000.mkv","sha256":"..."}],
 "trackingConfidence":"tracking.json","masks":["masks/....rle"],
 "status":"complete","createdAt":"..."}
```

Atomicity: write chunks to temp names, fsync/close, rename; write manifest last. On resume, only chunks listed with matching hashes are trusted. On any mismatch, discard the cache directory and regenerate.

### 5.5 Undo/redo and partial redo

- New `RemovalLayerCommand` (or `ProjectEditCommand`) stores before/after `RemovalLayer` snapshots and the mask/tracking sidecar hashes. `QUndoStack` stays the single source of truth; Ctrl+Z/Y behaves as in M2.
- Mask brush strokes: one stroke = one command. Use a `MaskStrokeCommand` that stores the before/after mask hash; content-addressed sidecars make this cheap. Coalesce continuous strokes with `mergeWith`.
- Tracking is a derived operation but produces project-visible state (propagated masks + confidence). Treat “run tracking” as a command; undo restores the previous tracking revision. If the user undoes, the old cache directory remains (content-addressed) for fast redo.
- Delete layer: set `enabled=false` or remove it; either way the original is restored immediately. Undo restores the layer and its cache key; cache files are not deleted synchronously.
- Partial redo: user selects a range or a low-confidence segment; invalidate only the chunks whose source frame range intersects `[prevKeyframe, nextKeyframe]` (plus overlap). Keep other chunks. Increment `revision`; old cache stays for undo until GC.
- Cache GC: only delete unreferenced `removal/<key>` directories, LRU, and never delete project-sidecar masks/tracking. Never GC while an export is running or a repair layer references the key.

### 5.6 Local-only processing and privacy

- The AI subprocess has no network permission; model files are local.
- Frames and masks never leave the machine. Temp frames are written under the cache root and deleted after the job.
- Logs contain paths and timings, not pixel data or user content.
- Model files are hash-verified at startup; a mismatch disables the feature with a reinstall message rather than running an unknown model.

### 5.7 Export integration

M3 export freezes a `Project` snapshot and renders via `OfflineFrameRenderer` (same `TimelineCompositor`). M4 adds a **preflight** step:

1. For every enabled `RemovalLayer` in the snapshot, compute the cache key and check the manifest is `complete` and chunk hashes match.
2. If any layer is missing/stale, stop before creating output files and show:
   - which clip/range needs generation,
   - estimated time,
   - buttons “先生成再导出” / “取消”.
3. “先生成再导出” runs the AI job to completion, then restarts the export preflight. **The export pipeline itself never runs inference.**
4. During export, pin the cache keys and forbid GC. If a cache file disappears mid-export (external deletion), fail with a specific error; do not silently fall back to the original.
5. Preview/export consistency test: render the same frame through preview and `OfflineFrameRenderer` with a fixed cache fixture; compare RGBA within tolerance (target: identical, or ≤1 LSB if hardware paths differ).

This preserves M3’s determinism and the spec’s “preview and export must agree” requirement.

### 5.8 File-level integration map

| Change | File(s) |
|---|---|
| Schema v3, `RemovalLayer`, validation | `src/core/Project.h`, `src/core/ProjectSerializer.cpp`, `src/core/Project.cpp` |
| Removal commands / undo | `src/core/commands/` (new `RemovalLayerCommand.*`), `src/core/ProjectDocument.*` |
| Evaluator attaches removal layer | `src/core/TimelineEvaluator.h/.cpp` |
| Compositor repair source | `src/media/render/TimelineCompositor.h/.cpp`, new `RepairFrameSource.*` |
| Cache namespace + manifests | `src/media/cache/CacheStore.h/.cpp`, new `RemovalCache.*` |
| AI subprocess / job manager | new `src/ai/` (process, protocol, model registry), `src/app/AppController.*` |
| Mask editor UI | new `src/app/RemovalPanel.qml`, `MaskEditorOverlay.qml`, `src/ui/RemovalController.*` |
| Preview compare | `src/ui/PreviewController.*`, `src/media/render/TimelineRenderWorker.*` |
| Export preflight | M3 `ExportPipeline`/`ExportController` (per plan), `src/core/ExportSettings.*` if needed |
| Tests | `tests/core/RemovalLayerTest.cpp`, `tests/media/RemovalCacheTest.cpp`, `tests/media/RepairSourceTest.cpp`, `tests/media/RemovalExportTest.cpp`, `tests/ui/RemovalControllerTest.cpp`, QML tests |

---

## 6. UX

### 6.1 Default flow (CapCut-like simplicity)

1. Select a video clip.
2. Click **“AI 消除”** in the inspector or preview toolbar. The preview enters mask mode: dimmed overlay, brush cursor, a compact toolbar.
3. Paint or drag a box around the target. SAM 2 can also use a click/box prompt to snap the mask.
4. Adjust **羽化** (feather) with a small slider; use **擦除/恢复** brush to refine edges.
5. Click **“跟踪”**. The AI subprocess propagates the mask; a confidence band appears under the clip.
6. Review low-confidence frames; jump to a red frame, correct the mask, click **“从此帧重新传播”**.
7. Click **“生成修复”**. Progress panel shows 跟踪/修复/写入缓存, percent, ETA, pause/cancel.
8. When complete, the repair layer appears on the clip; toggle **“原片对比”** (press-and-hold or split wipe) to inspect.
9. Advanced controls are collapsed by default: model (快速/高质量), processing resolution, temporal stabilization strength, chunk size. Professional users can open them; nothing is removed.

### 6.2 Controls and states

| Control | Default | Advanced |
|---|---|---|
| Brush / box select | Visible | — |
| Brush size / hardness | Visible | — |
| Feather | Visible (small slider) | Numeric px |
| Add/delete mask keyframe | Visible on timeline | — |
| Track / re-propagate | Visible | Direction (forward/backward/both) |
| Model choice | Hidden (“自动”) | 快速 (MI-GAN) / 高质量 (LaMa) |
| Temporal stabilization | Hidden (on) | Off/weak/strong |
| Processing resolution | Hidden (auto) | 540p/720p/1080p |
| Cache status / confidence | Visible | Per-frame details |
| Compare | Visible | Split wipe / difference view |

### 6.3 Progress, cancel, resume

- The AI job runs in the background; the UI remains responsive and the timeline remains editable. A job panel shows status and a cancel button.
- Cancel: stop after the current chunk; keep completed chunks; mark the layer `partial`. Resume from the manifest.
- Crash: on restart, detect an incomplete manifest, mark the layer `failed/partial`, and offer “继续生成” or “重新开始”.
- Pause: stop submitting new chunks; keep the subprocess alive if VRAM should be retained, or release sessions if the user needs the GPU. Default: release after 30 s idle.
- Notifications: one inline Chinese message on completion, not a modal dialog, unless the user must choose (e.g., missing cache before export).

### 6.4 Error messages (examples)

| Situation | Message |
|---|---|
| VRAM insufficient | “显存不足，已降低处理分辨率并分块处理。预计耗时增加。可在高级设置中进一步降低分辨率。” |
| Device lost | “显卡设备已重置，AI 任务已中断。已完成的分段已保留，可重试或改用 CPU。” |
| Model missing/hash mismatch | “AI 模型文件缺失或校验失败，请重新安装应用或修复模型包。” |
| Cache missing before export | “片段‘…’的 AI 修复结果缺失或已过期。请先生成修复，再导出。” |
| Mask tracking low confidence | “第 00:00:03:12 帧跟踪置信度低，请修正遮罩后从此帧重新传播。” |
| Disk full | “磁盘空间不足，已停止写入并保留进度。请清理缓存或更换缓存目录。” |

### 6.5 “Do not hide controls”

- Always show the confidence band and cache status; users must see *why* a result may be wrong.
- The advanced panel exposes model, resolution, stabilization, and chunk size.
- Do not auto-apply a low-confidence result as final; mark it and ask for review.
- Do not remove M2/M3 capabilities; the removal panel is additive and collapsible.

---

## 7. Risks and Mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **Training-data licensing** (SA-1B, Places2, YouTube-VOS) blocks commercial shipment despite Apache/MIT weights | High | Critical | Legal review in parallel with the spike; document provenance; fallback to owned-data retraining or commercial license; do not claim “clean” until signed off. |
| 2 | **SAM 2/EfficientTAM ONNX on DirectML** is slow, numerically wrong, or fails session creation | High | High | Two-model spike; numerical parity vs CPU; fallback to EfficientTAM/MobileSAM; EP abstraction; CPU fallback; static shapes; batch=1. |
| 3 | **Quality/flicker on moving objects** fails the spec’s “moving target” acceptance | High | High | Per-frame MI-GAN/LaMa + temporal stabilization; confidence + keyframe correction + partial redo; define a quality corpus and honest limitation copy; consider FGT/STTN spike only if budget allows. |
| 4 | **VRAM/GPU variance** (AMD driver, 4–6 GB cards, device lost) | High | High | VRAM budget + fallback ladder; tiling/cropping; chunking; device-lost recovery; CPU fallback; test on at least one AMD and one NVIDIA GPU. |
| 5 | **Cache correctness/storage** (stale results, wrong key, disk growth) | Medium | High | Canonical hash, model-file hashes, manifest + chunk hashes, atomic writes, GC only unreferenced, preflight on export; cache-size UI. |
| 6 | **Preview/export divergence** | Medium | High | Same `TimelineCompositor`; compare test; export preflight; no inference in export. |
| 7 | **Tracking drift/error accumulation** | High | Medium | Confidence gating; stop on low confidence; keyframe reset; memory-bank size limit; SAM2Long/SAMURAI ideas as future work. |
| 8 | **UX complexity** (masking is inherently fiddly) | Medium | Medium | CapCut-like default flow; progressive disclosure; large brush; box select; one-click compare; clear Chinese messages. |
| 9 | **Installer size / model distribution** | Medium | Medium | Bundle ~350 MB of models; hash-verify; optional download for quality model; clear licenses dialog. |
| 10 | **DirectML maintenance mode** | Medium | Medium | Keep EP abstraction; plan WinML evaluation in M5; do not bake DML-specific assumptions into model code. |
| 11 | **Antivirus/code-signing** of AI subprocess and model files | Medium | Medium | Sign binaries; ship models as data; document hashes; test on clean Windows 11. |
| 12 | **Performance target** (10 min for 1080p30 10 s on RTX 3060-class) on AMD | Medium | High | MI-GAN fast path; crop/tile; SAM2 Tiny; measure early; if target misses, reduce default processing resolution or narrow supported scope. |

---

## 8. Recommended M4 Scope and Non-Goals

### 8.1 In scope

- **Targets:** static/rigid objects (logos, watermarks, signs, tripods, fixed clutter) and slow-to-medium moving objects (pedestrians, vehicles) with a user-provided mask.
- **One object per removal layer.** Multiple layers are allowed but processed sequentially; no joint multi-object optimization.
- **Resolution/fps:** 1080p SDR, 30/60 fps project; source clips up to 30 s per removal layer (longer clips can be split into layers).
- **Masking:** brush, box, click-to-segment, erase/restore, feather, add/delete mask keyframes.
- **Tracking:** SAM 2.1 Tiny (or EfficientTAM fallback); per-frame confidence/occlusion; low-confidence surfacing; correction and re-propagation.
- **Inpainting:** MI-GAN fast path + LaMa quality path; temporal stabilization; crop/tile; color matching.
- **Generation layer:** cache-backed; enable/disable/delete; partial redo; undo/redo.
- **Preview:** original/repair compare (toggle/split); confidence band; cache status.
- **Export:** preflight; frozen snapshot; same compositor; no inference during export.
- **Process:** AI subprocess; local-only; progress/cancel/resume; crash recovery; VRAM fallback.
- **Models:** bundled and hash-verified; licenses/attributions dialog.

### 8.2 Explicit non-goals

- ProPainter-class temporal consistency; diffusion/generative content synthesis; physics-aware removal (VOID).
- Fast/complex motion, large disocclusion, reflections/shadows that require scene understanding.
- 4K/HDR end-to-end; 120 fps; multi-camera.
- Automatic object detection or text-prompt removal; user mask only.
- Joint multi-object removal; mask-based relighting.
- Cloud processing, collaboration, model training/fine-tuning.
- Real-time repair preview during generation (preview uses completed cache; generation is offline).
- Non-Windows platforms; CUDA/TensorRT-specific optimizations as a shipped requirement.
- Guaranteed quality on every clip; the UI must communicate “experimental” for hard cases.

### 8.3 Quality positioning

Ship M4 as **“AI 消除（本地）”** with honest capability copy:
- “适合固定物体、水印、台标和移动较慢的目标。”
- “快速运动、大面积遮挡或复杂反射可能需要手动修正，效果有限。”
- Do not compare to cloud generative removal; do not promise ProPainter-level results.

---

## 9. Proposed 10-Task Implementation Outline

Dependencies are shown; tasks 5 and 6 can run in parallel after 4. Each task ends with build + tests green and a commit.

### Task 1 — Project schema v3 and RemovalLayer model

**Depends on:** none.
**Files:** `src/core/Project.h/.cpp`, `ProjectSerializer.cpp`, `ProjectDocument.*`, `commands/RemovalLayerCommand.*`, `tests/core/RemovalLayerTest.cpp`, `ProjectSerializerTest.cpp`, CMake.
**Work:**
- Add `RemovalLayer`, `MaskKeyframe`, `Clip.removalLayerId`, `Project.removalLayers`.
- Bump schema to 3; accept v1/v2 and migrate with empty removal layers; reject v4+.
- Validation: ids, clip references, source range, keyframe ordering, cache key format.
- `RemovalLayerCommand` with before/after snapshots; undo/redo.
**Tests/acceptance:**
- v2 project loads with empty removal layers; v3 round-trips exactly.
- Duplicate ids, bad ranges, non-video clip references rejected with Chinese errors.
- Undo/redo restores layer metadata and `removalLayerId`.
- Existing M1/M2 tests stay green.

### Task 2 — Mask sidecars, RLE codec, cache key, manifest, CacheStore

**Depends on:** Task 1.
**Files:** new `src/core/MaskCodec.*`, `src/media/cache/RemovalCache.*`, `CacheStore.*`, `tests/core/MaskCodecTest.cpp`, `tests/media/RemovalCacheTest.cpp`.
**Work:**
- RLE encode/decode binary masks; deterministic hash; feather as a separate parameter.
- Project sidecar layout `<project>.veproj.assets/masks|tracking`.
- Cache key computation (canonical JSON, SHA-256, model hashes, pipeline version).
- Manifest read/write; atomic chunk writes; complete/partial/failed states.
- `CacheStore::removalPath(key)`, `ensureRemovalDir`, GC that never touches project sidecars.
**Tests/acceptance:**
- RLE round-trip for random/complex masks; hash stable across runs/Qt versions.
- Cache key changes when mask/params/model version changes; unchanged when timeline position changes.
- Partial manifest + corrupt chunk -> cache invalid; cache clear leaves masks intact.
- Concurrent read during write never sees a partial manifest.

### Task 3 — Mask editor UI and mask keyframes

**Depends on:** Tasks 1–2.
**Files:** new `src/app/RemovalPanel.qml`, `MaskEditorOverlay.qml`, `src/ui/RemovalController.*`, `tests/ui/RemovalControllerTest.cpp`, QML tests.
**Work:**
- Enter/exit mask mode; dim overlay; brush/eraser/box; brush size/hardness; feather slider.
- Add/delete mask keyframes; timeline diamonds; click to seek.
- One stroke = one undo command; coalesce.
- Expose mask state to QML; Chinese labels.
**Tests/acceptance:**
- QML objectNames for all controls; brush/box produce expected mask hash changes.
- Keyframe add/delete/seek; undo/redo; feather changes do not change mask hash.
- No regression in `TimelineQml`/`TimelineUiTest`.

### Task 4 — AI subprocess, protocol, job manager, model registry

**Depends on:** Tasks 1–2.
**Files:** new `src/ai/AiProcess.*`, `AiProtocol.*`, `AiJobManager.*`, `ModelRegistry.*`; `src/app/AppController.*`; `tests/app/AiJobManagerTest.cpp`.
**Work:**
- Spawn/monitor subprocess; QLocalSocket JSON protocol; progress/cancel/pause/resume; crash recovery.
- Model registry: id, version, file hash, license/attribution, EP policy.
- Job queue: one active job; frozen job description; no project access.
- Fake engine for tests (no model/GPU).
**Tests/acceptance:**
- Start/progress/cancel/resume with fake engine; kill subprocess -> job failed, project unchanged.
- Duplicate job rejected; stale generation ignored; no network calls.
- Model hash mismatch disables feature with clear error.

### Task 5 — Tracking pipeline (SAM 2 / EfficientTAM ONNX)

**Depends on:** Task 4.
**Files:** new `src/ai/TrackingPipeline.*`, `Sam2Onnx.*`, `EfficientTamOnnx.*`; `tests/ai/TrackingPipelineTest.cpp`; model assets.
**Work:**
- Load split ONNX sessions (image encoder, decoder, memory attention, memory encoder).
- Prompt from box/points/mask; propagate forward/backward; memory gating; confidence/occlusion output.
- EP selection (DML -> CPU), static shapes, batch=1, warmup.
- Numerical parity vs CPU reference; benchmark on target GPU.
**Tests/acceptance:**
- CPU vs DML mask IoU ≥ 0.98 on 20 fixed frames (or `GTEST_SKIP` without GPU).
- Tracking J&F on 5 M4 fixture clips ≥ agreed threshold (e.g., 0.75; set after baseline).
- Speed: ≥ 5 fps tracking at 1080p on the reference AMD GPU; else fall back to EfficientTAM/MobileSAM.
- Low-confidence frames surfaced; re-prompt resets memory.

### Task 6 — Inpainting pipeline (MI-GAN / LaMa ONNX) and temporal stabilization

**Depends on:** Task 4.
**Files:** new `src/ai/InpaintPipeline.*`, `MiganOnnx.*`, `LamaOnnx.*`, `TemporalStabilizer.*`; `tests/ai/InpaintPipelineTest.cpp`.
**Work:**
- MI-GAN pipeline (image + mask, arbitrary resolution) and LaMa 512 tiling.
- Crop around mask, tile with overlap, paste with feathered alpha; color matching.
- Temporal stabilization: flow-warp neighboring repaired frames, blend; optional median in hole.
- Fast/quality model selection; VRAM budget; chunking.
**Tests/acceptance:**
- Synthetic hole: output PSNR/SSIM above baseline; no NaNs; deterministic on CPU.
- Temporal metric (E_warp/VFID) improves vs per-frame baseline on a fixed clip; flicker subjective check.
- 1080p30 10 s on RTX 3060-class ≤ 10 min (spec); AMD target set after spike.
- VRAM cap honored; fallback ladder triggers.

### Task 7 — RepairFrameSource, compositor integration, preview compare

**Depends on:** Tasks 2, 3, 6.
**Files:** new `src/media/render/RepairFrameSource.*`; modify `TimelineCompositor.*`, `TimelineEvaluator.*`, `TimelineRenderWorker.*`, `PreviewController.*`; `tests/media/RepairSourceTest.cpp`, `RemovalRenderTest.cpp`.
**Work:**
- `RepairFrameSource` opens cache manifest/chunks; returns RGBA textures.
- Evaluator attaches active removal layer; compositor draws repair instead of source; transforms/opacity apply.
- Compare modes: original, repair, split wipe; `RenderOptions` for preview only.
- Missing/stale cache -> original + warning in preview.
**Tests/acceptance:**
- Fixed cache fixture renders expected pixels; compare mode shows correct halves.
- Layer enable/disable/delete restores original; transform/opacity match source behavior.
- Missing cache does not crash; warning emitted once.
- Existing compositor/render tests green.

### Task 8 — Export integration and preflight

**Depends on:** Task 7.
**Files:** M3 `ExportPipeline`/`ExportController` (per plan); `tests/media/RemovalExportTest.cpp`; `tests/app/ExportRemovalUiTest.cpp`.
**Work:**
- Preflight: compute keys, validate manifests/chunks, list missing ranges.
- “先生成再导出” vs “取消”; never infer inside export.
- Freeze cache keys in snapshot; forbid GC during export; missing mid-export -> specific error.
- Same compositor path; compare preview/export frames.
**Tests/acceptance:**
- Export with complete cache includes repair; without cache fails before writing output.
- Preview/export RGBA identical (or ≤1 LSB) on fixed fixture.
- Cancel during preflight leaves no partial output; export after generation succeeds.
- Existing M3 encoder/segment tests green.

### Task 9 — UX polish: progress, confidence, partial redo, errors

**Depends on:** Tasks 3, 7.
**Files:** `RemovalPanel.qml`, `ConfidenceBand.qml`, `JobProgressPanel.qml`; `RemovalController.*`; QML tests; UAT script.
**Work:**
- Progress stages/ETA/pause/cancel/resume; background continuation.
- Confidence band colors; jump to low-confidence frame; “从此帧重新传播”; “重做选区”.
- Compare press-and-hold/split; cache status; advanced panel.
- Chinese error messages per §6.4.
**Tests/acceptance:**
- QML tests for controls/objectNames/state transitions.
- Partial redo invalidates only selected chunks (manifest evidence).
- Cancel/resume after restart works.
- UAT: a user can mask, track, generate, compare, correct, and redo without docs.

### Task 10 — Evaluation corpus, performance/robustness, acceptance report

**Depends on:** Tasks 5–9.
**Files:** `tests/fixtures/removal/` (small synthetic clips), `tests/ai/RemovalEvalTest.cpp`, docs.
**Work:**
- Corpus: watermark/logo, static sign, slow pedestrian, car, occlusion, textured background, low light, 24/30/60 fps, 1080p.
- Metrics: mask IoU/J&F, PSNR/SSIM where reference exists, VFID/E_warp, subjective 1–5, failure taxonomy.
- Robustness: device lost, VRAM 4/6/8 GB, CPU fallback, disk full, missing model, corrupt cache, kill subprocess.
- Performance: tracking/inpaint fps, end-to-end 10 s clip, cache size, memory peak.
**Tests/acceptance:**
- Acceptance report signed off against §11.2/§11.3; known limitations documented.
- No P0 crashes; all fallbacks produce user-visible explanations.
- M1/M2/M3 test suites remain green.

---

## 10. Acceptance and Evaluation Ideas

### 10.1 Functional acceptance

1. A user can paint/box a mask, track it, see confidence, correct a keyframe, and re-propagate a segment.
2. A user can generate repair for a 10 s 1080p clip, close the app, reopen, and resume/complete.
3. Undo/redo restores masks, keyframes, layer state, and cache references; delete restores the original.
4. Partial redo only recomputes the selected range; other chunks are reused.
5. Preview compare shows original vs repair; export uses the same result.
6. Missing/stale cache is clearly reported before export; export never silently outputs the original.
7. All AI processing is local; no network traffic; model files hash-verified.

### 10.2 Quality acceptance

- **Mask quality:** SAM 2/EfficientTAM ONNX vs CPU reference IoU ≥ 0.98 on fixed frames; tracking J&F on the M4 corpus ≥ threshold agreed after baseline.
- **Static targets:** watermark/logo removal has no visible flicker; background texture continuity is acceptable at 100% zoom.
- **Moving targets:** slow/medium motion with simple background passes subjective 3/5; fast/complex occlusion is documented as a limitation, not a silent failure.
- **Temporal metrics:** E_warp/VFID improve over per-frame baseline; no frame-to-frame color pumping.
- **Subjective:** 3 reviewers rate 1–5; target mean ≥ 3.5 for in-scope clips, with failure cases catalogued.

### 10.3 Performance acceptance

| Scenario | Target |
|---|---|
| 1080p30, 10 s, RTX 3060-class, fast path | ≤ 10 min end-to-end (spec); target ≤ 3 min |
| 1080p30, 10 s, RTX 3060-class, quality path | ≤ 10 min |
| 1080p30, 10 s, reference AMD GPU | Set after spike; target ≤ 10 min |
| Tracking throughput | ≥ 5 fps at 1080p on reference GPU |
| Inpainting throughput | MI-GAN ≥ 10 fps; LaMa ≥ 2 fps at 1080p (tiled) |
| Preview | Repair cache playback does not drop below 24 fps in a 3-video-track project |
| VRAM peak | ≤ 4 GB on 6 GB card; fallback ladder for 4 GB |
| Cache size | ≤ 2× source size for lossless patch cache; UI shows usage and clear action |

### 10.4 Robustness acceptance

- Kill AI subprocess mid-job -> layer marked failed/partial, project unchanged, retry works.
- Device lost/reset -> subprocess restarts; CPU fallback offered; no crash.
- Disk full -> stop writing, keep completed chunks, clear message.
- Corrupt/missing cache -> preflight fails; preview falls back with warning.
- Model hash mismatch -> feature disabled with reinstall message.
- Project save/load with masks and cache refs -> byte-stable schema, masks intact after cache clear.

---

## 11. Open Questions

1. **Reference AMD GPU:** Which exact Radeon model, VRAM size, and driver version is the M4 performance gate? DirectML behavior varies significantly across RDNA generations.
2. **Legal position:** Will legal accept Apache-2.0/MIT weights trained on SA-1B/Places2/YouTube-VOS, or must we retrain/fine-tune on owned data? This is the go/no-go question.
3. **ProPainter commercial license:** Is negotiating a commercial license and funding an ONNX/TensorRT port an option if quality is non-negotiable? What budget/timeline?
4. **Quality bar:** What is the minimum acceptable subjective score and flicker threshold for “moving target” acceptance? Who signs off on the corpus?
5. **Model distribution:** Bundle ~350 MB of models in the installer, or download on first use? Is offline-first a hard requirement?
6. **Resolution/fps ceiling:** Is 4K or 60 fps removal required in M4, or can it be M5? Does the spec’s “1080p baseline, not a ceiling” apply to AI removal?
7. **Clip length:** Is 30 s per removal layer acceptable, or must M4 handle multi-minute continuous removal?
8. **Multi-object:** Is one object per layer acceptable for M4, or is joint multi-object removal required?
9. **Cache budget:** What disk usage is acceptable for lossless patch cache (e.g., 2× source size)? Should the user be able to relocate the cache?
10. **NVIDIA QA:** Even though the target is AMD, do we need an NVIDIA CUDA/TensorRT QA path for comparison and future users?
11. **Model updates:** Who owns model versioning and re-validation when a new SAM 2/EfficientTAM/MI-GAN/LaMa release appears?
12. **UI language:** Confirm all new user-facing strings are Chinese; model names/licenses in an English “Licenses” dialog.

---

## 12. Sources

### Model repositories and licenses

- SAM 2: https://github.com/facebookresearch/sam2 ; paper https://arxiv.org/abs/2408.00714 ; license summary https://deepwiki.com/huggingface/segment-anything-2/7-legal-and-governance
- SAM 2 ONNX split (C++/DirectML): https://github.com/pagarcia/sam2-onnx-cpp
- AMD SAM 2 ONNX/ROCm: https://huggingface.co/AMD-PAVS-AI/sam2
- EfficientTAM: https://github.com/yformer/EfficientTAM ; weights https://huggingface.co/yunyangx/efficient-track-anything ; ONNX bundle https://huggingface.co/egordm/efficienttam-ti-512
- EdgeTAM: https://github.com/facebookresearch/EdgeTAM ; PTQ/ONNX https://github.com/D-Robotics-AI-Lab/EdgeTAM_PTQ
- MobileSAM: https://github.com/ChaoningZhang/MobileSAM ; third-party license confirmation https://github.com/LibreYOLO/libreyolo/pull/471
- Cutie: https://github.com/hkchengrex/Cutie
- XMem: https://github.com/hkchengrex/XMem ; GPL listing https://libraries.io/github/hkchengrex
- SAM 3 license comparison: https://playground.roboflow.com/models/compare/sam-2-vs-sam-3
- ProPainter: https://github.com/sczhou/ProPainter ; LICENSE https://github.com/sczhou/ProPainter/blob/main/LICENSE ; memory optimization https://deepwiki.com/sczhou/ProPainter/4.2-memory-optimization
- E2FGVI: https://github.com/MCG-NKU/E2FGVI ; license commit https://github.com/MCG-NKU/E2FGVI/commit/924b56c133fffe37327f9c9b90290fc3d0538581
- FGT: https://github.com/hitachinsk/FGT ; weights https://huggingface.co/hitachinsk/FGT
- STTN: https://github.com/researchmm/STTN ; FastSTTN https://sibgrapi.sid.inpe.br/col/sid.inpe.br/sibgrapi/2021/09.06.22.26/doc/FastSTTN___SIBGRAPI_2021.pdf
- LaMa: https://github.com/advimman/lama ; ONNX weights https://huggingface.co/g-ronimo/lama ; fixed-512 ONNX https://huggingface.co/Carve/LaMa-ONNX
- MI-GAN: https://github.com/Picsart-AI-Research/MI-GAN ; ONNX runtime reference https://github.com/PyModel/watermark-remover/blob/main/skills/remove-ai-marks/scripts/migan_fill.py ; LiteRT benchmark https://huggingface.co/litert-community/MI-GAN-512-Places2-LiteRT
- MAT: https://github.com/fenglinglwb/MAT ; HF mirror https://huggingface.co/Acly/MAT
- DiffuEraser: https://github.com/lixiaowen-xw/DiffuEraser
- MiniMax-Remover: https://github.com/zibojia/MiniMax-Remover ; weights https://huggingface.co/zibojia/minimax-remover
- Netflix VOID: https://github.com/Netflix/void-model ; https://huggingface.co/netflix/void-model
- COCOCO: https://github.com/zibojia/COCOCO
- ZITS license audit: https://github.com/pdcgomes/redlamp/blob/f635efd4139d7f4798432ccb94448f057bd6803c/docs/research/notes/D-removal.md

### Datasets and training-data terms

- SA-1B research license: https://huggingface.co/datasets/xiuqhou/SA-Det-100k/blob/main/LICENSE ; TFDS catalog https://www.tensorflow.org/datasets/catalog/segment_anything
- Places2 terms: https://places.csail.mit.edu/ ; analysis https://dev.to/phpboyscout/the-weights-were-mit-the-training-data-wasnt-206d
- YouTube-VOS: https://youtube-vos.org/dataset/vos/ ; license listing https://tianchi.aliyun.com/dataset/91865
- DAVIS: https://github.com/fperazzi/davis-2017 ; LICENSE_DAVIS https://github.com/KevinXu02/segment-anything-2/blob/main/sav_dataset/LICENSE_DAVIS

### Inference stack

- ONNX Runtime DirectML EP: https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html
- ONNX Runtime execution providers overview: https://onnxruntime.ai/docs/execution-providers/
- ORT license: https://github.com/microsoft/onnxruntime/blob/main/LICENSE
- DirectML NuGet: https://www.nuget.org/packages/Microsoft.AI.DirectML
- D3D11/D3D12 sharing for DirectML: https://gpuopen.com/learn/onnx-directlml-execution-provider-guide-part2/
- DML batched readback fix: https://github.com/microsoft/onnxruntime/pull/28680
- DML GLU/attention correctness issues: https://github.com/microsoft/onnxruntime/issues/32146 ; https://github.com/microsoft/onnxruntime/issues/29739
- DML tiled memory manager: https://github.com/microsoft/onnxruntime/pull/20303
- SAM 2 speed summary: https://aiwiki.ai/wiki/sam_2
- EfficientTAM paper: https://openaccess.thecvf.com/content/ICCV2025/papers/Xiong_Efficient_Track_Anything_ICCV_2025_paper.pdf
- FGT 1080p memory benchmark: https://theses.gla.ac.uk/84828/1/2024LiShiboPhD.pdf
- LaMa CPU benchmark: https://hackcv.com/en/posts/practice-erase-cpu-benchmark/
- ProPainter 1080p community benchmark: https://maomu.com/a/propainter-video-object-removal

### Tracking robustness

- SAM2Long: https://arxiv.org/abs/2410.16268
- SAMURAI: https://ieeexplore.ieee.org/document/11351313
- Confidence/occlusion use in SAM 2: https://isprs-archives.copernicus.org/articles/XLIX-B3-2026/205/2026/isprs-archives-XLIX-B3-2026-205-2026.pdf
- SAM 2 occlusion/re-entry notes: https://github.com/storytold/effectcraft/blob/79c42749a85ee0f8df50bc1b9eb2b01db088d2cb/docs/roto-brush-sam2.md

---

## Appendix A — Why Not ProPainter for M4?

ProPainter is the quality reference, but it fails the M4 gate on four dimensions simultaneously:

1. **License:** NTU S-Lab 1.0 is non-commercial; commercial use requires negotiation.
2. **Memory:** 720p @ 50 frames needs 19–28 GB; 1080p is not officially supported and is effectively OOM on consumer cards.
3. **Export:** No maintained ONNX export; PyTorch + RAFT flow pipeline; heavy runtime.
4. **Speed:** 3–15 minutes per 10 s 1080p clip depending on GPU; not compatible with a responsive local editor workflow on 6–8 GB cards.

If ProPainter-level quality is a hard product requirement, the correct move is a separate “AI Pro” track with a commercial license, 12–16 GB VRAM minimum, and an ONNX/TensorRT port. It is not an M4 task.

## Appendix B — Why Not Diffusion (VOID / DiffuEraser / MiniMax) for M4?

- **VOID** is Apache-2.0 and commercially attractive, but 5B parameters and 40 GB+ VRAM put it outside consumer laptops; the supplied mask pipeline uses a Gemini API, conflicting with “local-only.” It is a future high-end/offline option, not M4.
- **DiffuEraser** is Apache-2.0 code but depends on ProPainter (non-commercial) and SD1.5 (OpenRAIL-M); swapping the prior is research work.
- **MiniMax-Remover** weights are CC-BY-NC-4.0.
- Diffusion is also slow and non-deterministic; M4’s value is interactive, repeatable local removal, not generative synthesis.

## Appendix C — Suggested Model Bundle

| Model | Purpose | Size (approx.) | License file to ship |
|---|---|---|---|
| SAM 2.1 Hiera-Tiny ONNX (split) | Tracking | ~160 MB | Apache-2.0 + NOTICE |
| EfficientTAM-Ti 512 ONNX | Fallback tracking | ~40–77 MB | Apache-2.0 |
| MI-GAN 512 ONNX | Fast inpainting | ~28 MB | MIT + LICENSE-WEIGHTS |
| LaMa 512 ONNX fp16 | Quality inpainting | ~110 MB | Apache-2.0 |
| ONNX Runtime + DirectML | Runtime | ~50 MB | MIT + ThirdPartyNotices + DirectML terms |
| **Total** | | **~390–425 MB** | |

If installer size is a concern, bundle EfficientTAM + MI-GAN (~120 MB) and make SAM 2/LaMa optional downloads with SHA-256 verification. This does not affect the local-only guarantee, only first-run convenience.
