# M4 Optimization Research — Local AI Object Removal on the AMD Radeon iGPU

- Date: 2026-11-15
- Branch: `feature/m4-ai-removal`
- Base commit: `81a511c` (Task 0 gate: `NO_GO` for the original interactive per-component gates)
- Scope: internal technical validation on the current AMD machine. No product code, no `src/` change, no model binaries in git.
- Licensing state: unchanged — `TECHNICAL_EVALUATION_ONLY`, `commercialApproved=false`, `releaseBlocked=true`.
- All measurements below are real ONNX Runtime sessions on the files under `C:/m4-spike/models` (outside the repo) or derived variants under `C:/m4-spike/models_opt`. No number is extrapolated unless it is explicitly labelled **estimate**.

---

## 0. Executive verdict

**The Task 0 `NO_GO` is confirmed for the original interactive gates, but a reduced offline M4 scope is viable on this hardware.**

Measured facts that drive the decision:

1. **The Radeon iGPU is not faster than the 16-core CPU for these fp32 graphs.** SAM 2 vision encoder: **774 ms CPU vs 1655 ms DML**; SAM 2 memory attention: **637 ms CPU vs 2302 ms DML**. DirectML only wins for MI-GAN (**449 ms DML vs 712 ms CPU**). The machine has a 2-CU-class shared-memory iGPU; the CPU is the practical accelerator.
2. **SAM 2.1 Tiny cannot be resized below 1024² without a re-export.** Its ONNX graph bakes the 256×256 patch grid, 32×32 window layout, relative-position bias, 64×64 neck position encoding and a fixed 28736-token memory bank. Feeding a 512² or 768² input fails shape inference at `/vision_encoder/backbone/Add_1`.
3. **Per-frame SAM 2 tracking is 0.24 fps DML / 0.68 fps CPU**, but **keyframe SAM 2 + OpenCV DIS flow** reaches **8.1 fps effective at 1080p** with mean mask IoU **0.913 vs per-frame SAM 2** at a keyframe interval of 10 frames (0.947 at interval 5).
4. **EfficientTAM's Task 0 parity failure is fp16 rounding, not the export.** The fp32 bundle from the same repository passes every component (worst **3.1e-4**). EfficientTAM fp32 on CPU runs a full tracking step in **250 ms (4.0 fps)** and a keyframe segmentation in **45 ms (22 fps)** — the best parity-safe tracker on this machine.
5. **LaMa's `0x80070057` has a concrete, fixable root cause:** 72 `Reshape` nodes carry `allowzero=1` together with a `-1` dimension. That combination is invalid per the ONNX spec; ORT CPU tolerates it, DirectML returns `E_INVALIDARG`. Patching `allowzero` to 0 is behaviour-preserving (CPU output difference **0**) and makes DML run. DML is then **1328 ms fp32**, slower than **542 ms CPU**. LaMa is usable offline on CPU, not as a DML optimization.
6. **MI-GAN always processes a fixed 512² canvas** (~435–450 ms DML), so ROI cropping cannot get below that floor. Its 1080p input path adds ~100 ms of preprocessing; cropping the ROI outside the ONNX graph removes that. The quality is materially below LaMa (PSNR 14.0–19.8 vs 16.5–33.5), and the official mask contract is **255 = known/background, 0 = hole**, the inverse of the Task 0 probe's random mask.
7. **Best measured end-to-end 1080p30 10 s, one object:** **~2.6 min** (EfficientTAM keyframes + DIS flow + MI-GAN DML) to **~3.1–3.5 min** (same tracking + LaMa CPU quality path), with an estimated **~12 s** of temporal stabilization. This is **offline-acceptable** (target ≤10 min) and **not interactive**.

**Recommendation: continue M4 with a reduced technical scope.** Position it as an offline generation feature, use EfficientTAM-Ti 512 **fp32** on CPU as the default keyframe segmenter, SAM 2.1 Tiny as the quality fallback, DIS flow for propagation, and LaMa int8/fp32 CPU as the quality repair path with MI-GAN as the fast path. Do not claim GPU acceleration on this iGPU. A discrete GPU remains the only route to the original interactive gates. Licensing remains the release blocker.

---

## 1. Environment and method

### 1.1 Environment

| Item | Value |
|---|---|
| OS | Windows 11 Pro 10.0.26300 (x64) |
| CPU | AMD Ryzen 9 9950X, 16 cores / 32 threads, 4.3 GHz |
| GPU | **AMD Radeon(TM) Graphics iGPU**, PCI `1002:13C0`, driver `32.0.21042.62`, 2-CU-class |
| GPU memory | Dedicated 485.7 MiB; shared 31.15 GiB |
| ONNX Runtime | `onnxruntime-directml` **1.22.0** (`DmlExecutionProvider` + `CPUExecutionProvider`) |
| Python / numpy | 3.12.10 / 2.5.4 in `C:/m4-spike/venv` |
| Tooling added to the spike venv (not the repo) | `onnx` 1.23.2, `onnxconverter-common` 1.16.0, `onnxoptimizer` 0.4.2, `onnx-simplifier` 0.5.0, `opencv-python-headless` 5.0.0.93, `scikit-image` 0.26.0 |

**Hardware-specific verdict.** No NVIDIA QA machine is available; the Task 0 NVIDIA row remains unmeasured. All conclusions apply to this AMD iGPU (and, where stated, its CPU). A discrete AMD/NVIDIA re-run with the same harness is required before any multi-vendor GO.

### 1.2 Session options and measurement protocol

Unless stated otherwise:

```
graph_optimization_level = ORT_ENABLE_ALL
execution_mode           = ORT_SEQUENTIAL
enable_mem_pattern       = false
enable_cpu_mem_arena     = false
DML device_id            = 0 (AMD iGPU)
warmup                   = 1–2 runs
timing                   = p50 and p95 over 5–8 runs (p50 over 2–3 for full pipelines)
parity                   = max(|cpu-dml|)/max(1e-6, max|cpu|), max absolute difference,
                           thresholded mask IoU at logit 0
```

Every session checks `session.get_providers()` actually contains `DmlExecutionProvider`; ORT silently drops DML on an unusable device.

### 1.3 Measurement harness (outside the repo)

All scripts are under `C:/m4-spike/scripts/opt/` and are not committed:

| Script | Purpose |
|---|---|
| `bench.py` | one-graph CPU/DML benchmark, provider options, graph optimizations, threads, profiling, parity |
| `matrix.py` | matrix driver for `bench.py` |
| `inspect_graphs.py`, `shape_nodes.py` | ONNX I/O, opset, op histogram, constant/shape inspection |
| `make_variants.py` | LaMa fp32/BN-fold/simplify variants; SAM 2 fp16 conversion |
| `bisect_dml.py` | finds the first DML-failing node by extracting graph prefixes |
| `sam2_pipeline.py`, `sam2_seq.py` | real SAM 2 encoder→decoder→memory pipeline; per-frame sequence masks |
| `efficienttam_pipeline.py` | EfficientTAM image→prompt→mask pipeline and mask-IoU comparison |
| `flow_eval.py`, `flow_vs_sam2.py` | DIS/Farneback timing and mask propagation accuracy |
| `inpaint_quality.py`, `inpaint_matrix.py` | MI-GAN vs LaMa PSNR/SSIM/latency over several hole types |

Real test imagery: frames from `D:/works/VideoEditor/badminton.mp4` (portrait 720×1280) and `tests/fixtures/perf1080.mp4` (1920×1080). Masks and frames used as pseudo-ground-truth are described in each section.

---

## 2. Results summary

### 2.1 SAM 2.1 Hiera-Tiny (5-session video ONNX, fp32 unless stated)

| Graph / pipeline | Resolution | Provider | Dtype | p50 ms | fps | Parity / mask IoU | Notes |
|---|---|---:|---|---:|---:|---|---|
| `vision_encoder` | 1024² | CPU | fp32 | **774.1** | 1.29 | reference | session create 174 ms; 8 threads: 698.9 ms |
| `vision_encoder` | 1024² | DML | fp32 | **1654.6** | 0.60 | 8.54e-6 | 3 fused DML nodes, no CPU fallback; 2.1× slower than CPU |
| `vision_encoder` | 1024² | DML | fp16 | **1365.8** | 0.73 | 1.31e-2 logits; mask IoU 0.9967 vs fp32 | 17% faster than fp32 DML, still 1.8× slower than CPU fp32 |
| `mask_decoder` | 1024² | CPU / DML / DML-fp16 | — | **48.0 / 58.4 / 37.2** | — | fp32 mask IoU 1.000; fp16 0.9997 vs fp16 CPU | fragmented: 373 DML + 112 CPU graph nodes (Task 0) |
| `memory_encoder` | 1024² | CPU / DML / DML-fp16 | — | **41.3 / 91.4 / 61.9** | — | fp32 ≤2.5e-6 | fully fused |
| `memory_attention` | 28736-token bank | CPU / DML / DML-fp16 | — | **636.7 / 2302.0 / 2585.3** | — | fp32 ≤1.7e-6 | 63 DML nodes; bank is static at 28736 |
| **Full tracking step** | 1024² | CPU | fp32 | **1474.0** | **0.68** | reference | encoder+decoder+memory encoder+memory attention |
| **Full tracking step** | 1024² | DML | fp32 | **4115.8** | **0.24** | mask IoU 1.0 | this is the Task 0 component-sum confirmed end-to-end |
| **Full tracking step** | 1024² | DML | fp16 | **4052.3** | **0.25** | mask IoU 0.9964 vs fp32 CPU | no speed win |
| 768² / 512² / 384² / 256² | — | CPU | — | **unsupported** | — | — | shape inference fails at `/vision_encoder/backbone/Add_1`; graph is baked to 1024² |

Mask parity on the real badminton player (point prompt, 1024²):

| Comparison | Mask IoU | Pixels |
|---|---:|---:|
| fp32 CPU vs fp32 DML | **1.00000** | 3,890 / 3,890 |
| fp16 CPU vs fp32 CPU | **0.99666** | 3,879 / 3,890 |
| fp16 DML vs fp32 CPU | **0.99640** | 3,880 / 3,890 |
| fp16 CPU vs fp16 DML | **0.99974** | 3,879 / 3,880 |

### 2.2 EfficientTAM-Ti 512 (fp32 bundle unless stated)

| Graph / pipeline | Resolution | Provider | Dtype | p50 ms | fps | Parity | Notes |
|---|---|---:|---|---:|---:|---|---|
| `image_encoder` | 512² | CPU / DML | fp32 | **36.9 / 231.4** | — | 4.82e-6 | 3 fused DML nodes; CPU 6.3× faster |
| `prompt_encoder` | 512² | CPU / DML | fp32 | **1.5 / 1.3** | — | 9.6e-7 | — |
| `mask_decoder` | 512² | CPU / DML | fp32 | **7.6 / 10.2** | — | **3.1e-4** | worst fp32 parity, still ≤1e-3 |
| `memory_encoder` | 512² | CPU / DML | fp32 | **9.7 / 20.3** | — | 6.2e-7 | — |
| `memory_attention` | 7168-token bank | CPU / DML | fp32 | **195.7 / 224.2** | — | 3.94e-5 | **1389 DML + 576 CPU nodes** (fragmented) |
| **Full tracking step** | 512² | CPU | fp32 | **250.0** | **4.00** | reference | encoder+memory attention+decoder+memory encoder |
| **Full tracking step** | 512² | DML | fp32 | **486.1** | **2.06** | ≤1e-3 | CPU wins |
| **Full tracking step** | 512² | DML | fp16 | **414.8** | **2.41** | **2.6e-3 – 2.35e-2 (FAIL)** | faster but numerically unusable |
| **Keyframe step** (encoder+prompt+decoder) | 512² | CPU | fp32 | **45.1** | **22.1** | mask IoU 1.0 CPU vs DML | 0.879 mask IoU vs SAM 2 1024² on the real player |

Task 0 fp16 parity, reproduced and diagnosed (all exceed 1e-3):

| Graph | fp16 max relative diff | fp32 max relative diff |
|---|---:|---:|
| image_encoder | 1.094e-2 | **4.82e-6** |
| prompt_encoder | 8.296e-3 | **9.6e-7** |
| mask_decoder | **2.353e-2** (0.469 absolute) | **3.1e-4** |
| memory_encoder | 2.627e-3 | **6.2e-7** |
| memory_attention | 2.589e-3 | **3.94e-5** |

fp16 mask_decoder parity is not a graph-fusion artifact: `ORT_DISABLE_ALL` 5.9e-3, `ORT_ENABLE_BASIC` 1.11e-2, `disable_metacommands=true` 1.11e-2, `enable_dynamic_graph_fusion=false` 8.5e-3.

### 2.3 MI-GAN 512

| Input / ROI | Provider | p50 ms | p95 ms | fps | Notes |
|---|---:|---:|---:|---:|---|
| 1080p full-frame, 10% mask | DML | **547.3** | 548.5 | 1.83 | includes ~100 ms of 1080p preprocessing |
| 1080p full-frame, 10% mask | CPU | **864.7** | 873.2 | 1.16 | — |
| 1080p full-frame, 5% / 20% mask | DML | 546.0 / 548.3 | 547.6 / 551.6 | 1.83 / 1.82 | ROI size does not change the cost |
| 512² input, 10% / 25% / full | DML | **449.2 / 446.4 / 448.8** | ~450 | 2.23–2.24 | fixed 512² generator |
| 512² input, full | CPU | **712.1** | 736.2 | 1.40 | — |
| 256² / 128² input | DML | **437.1 / 435.1** | ~438 | 2.29 / 2.30 | still a 512² internal canvas |
| DML nodes | — | — | — | — | 2532 DML + 114 CPU node events at 1080p; mostly DML |

- **Mask contract (critical):** the official pipeline expects **255 = known/background, 0 = hole**. The Task 0 probe fed a random sparse mask; with 255 in the hole the generator returns a plausible-looking but wrong image (PSNR ~7). This must be pinned in Task 6.
- **fp16:** no prebuilt fp16 bundle; a naive `onnxconverter_common` or ORT fp16 conversion produces invalid graphs at the `Less`/`Where`/`Equal` shape-comparison nodes. Not available for this study.
- **int8:** not attempted; DML int8 support is not established and the offline budget is already met at 2.3 fps.
- **Quality (real badminton frame, 512² crop, synthetic noise hole, PSNR/SSIM inside the hole):**

| Hole | MI-GAN DML | LaMa fp32 CPU |
|---|---:|---:|
| blue_smooth 60×50 | 19.8 dB / 0.469 | **33.5 dB / 0.749** |
| green_stripe 120×45 | 18.3 dB / 0.668 | **23.3 dB / 0.825** |
| court_line 80×40 | 16.1 dB / 0.515 | **17.3 dB / 0.710** |
| player_small 55×140 | 14.0 dB / 0.452 | **16.5 dB / 0.564** |
| large_banner 220×150 | 16.4 dB / 0.448 | **19.3 dB / 0.681** |

### 2.4 LaMa 512

| Variant (512² only) | Provider | p50 ms | fps | Parity | Notes |
|---|---|---:|---:|---|---|
| **fp32, `allowzero=0`** | CPU | **541.8** | **1.85** | reference | best CPU option |
| fp32, `allowzero=0` | DML | 1328.4 | 0.75 | 1.13e-5 | DML works after patch but is slower |
| fp16, `allowzero=0` | CPU | 705.8 | 1.42 | — | fp16 is slower on CPU |
| fp16, `allowzero=0` | DML | 1128.2 | 0.89 | **8.84e-2** | numerically unusable |
| **int8, `allowzero=0`** | CPU | **532.6** | **1.88** | 1.36e-5 vs its CPU | 62 MB; quality ≈ fp32 (see below) |
| int8, `allowzero=0` | DML | 1422.0 | 0.70 | 1.36e-5 | slower than CPU |
| DFT fp16, `allowzero=0` | CPU / DML | 2450.6 / 1719.1 | 0.41 / 0.58 | 1.22e-5 | 144 DFT ops; not viable |
| unpatched fp16 / int8 / fp32 | DML | — | — | — | **session creation fails: `0x80070057` (E_INVALIDARG)** |

- **Patch validation:** `allowzero=1 → 0` on 72 Reshape nodes is exactly behaviour-preserving on CPU (`maxRel = 0` vs unpatched fp32) and DML fp32 parity is `1.13e-5`.
- **Quality:** LaMa is better than MI-GAN on every hole type (PSNR 16.5–33.5, SSIM 0.56–0.83). int8 vs fp32: PSNR within 0.2 dB, SSIM within 0.01, max pixel difference 16–59/255. **LaMa int8 on CPU is the best size/quality/speed deployment artifact** (62 MB, 1.88 fps).

### 2.5 Optical flow (OpenCV 5.0, DIS medium vs Farneback)

| Resolution | DIS p50 / p95 | DIS fps | Farneback p50 / p95 | Farneback fps |
|---|---:|---:|---:|---:|
| 1080p (1080×1920) | **38.4 / 39.4 ms** | **26.0** | 208.9 / 210.7 ms | 4.8 |
| 720p (720×1280) | **19.0 / 20.1 ms** | **52.7** | 92.6 / 94.2 ms | 10.8 |
| 540p (540×960) | **13.8 / 14.6 ms** | **72.7** | 55.8 / 56.8 ms | 17.9 |
| 360p (360×640) | **7.5 / 7.7 ms** | **133.3** | 24.7 / 25.3 ms | 40.5 |

Mask propagation accuracy against **per-frame SAM 2 masks** on the real badminton sequence (31 frames, articulated player, DIS medium at 720×1280; mean/min IoU over frames 1–30):

| Keyframe interval K | SAM 2 calls / 30 frames | Mean IoU | Min IoU | Direct-from-keyframe mean |
|---:|---:|---:|---:|---:|
| 1 (SAM 2 every frame) | 30 | 1.000 | 1.000 | — |
| 2 | 15 | **0.985** | 0.894 | — |
| 3 | 10 | **0.970** | 0.894 | — |
| 5 | 6 | **0.947** | 0.885 | 0.946 |
| 10 | 3 | **0.913** | 0.852 | 0.907 |
| 15 | 2 | **0.884** | 0.803 | 0.876 |
| 30 | 1 | **0.856** | 0.770 | 0.866 |

Synthetic rigid object at 1080p (distinctive texture, DIS, mean IoU over 30 frames):

| Object speed | K=5 | K=10 | K=15 | K=30 |
|---|---:|---:|---:|---:|
| 3 px/frame | **0.985** | 0.973 | 0.965 | 0.948 |
| 8 px/frame | **0.955** | 0.920 | 0.909 | 0.865 |
| 20 px/frame | **0.873** | 0.808 | 0.756 | 0.684 |

**Interpretation:** DIS is fast enough to be interactive (26 fps at 1080p). Keyframe interval 5 gives 0.947 agreement with per-frame SAM 2 on real articulated motion; interval 10 gives 0.913. Fast motion (20 px/frame) degrades to 0.87 even at K=5, which is the boundary of the M4 "slow-to-medium" scope.

---

## 3. Root causes

### 3.1 SAM 2.1 Tiny: why the original gate fails, and why it is not a correctness bug

1. **The bundle is statically baked to 1024².** Rewriting the input to 512² or 768² fails shape inference at `/vision_encoder/backbone/Add_1`. The graph contains:
   - `precomputed_pos_embed [1,256,256,96]` (patch grid 256×256);
   - attention reshapes with constants `[1,32,32,8,8,-1]` and `[1,256,256,-1]` (32×32 windows at 1024²);
   - a neck position encoding built from `[64,64]` constants;
   - `memory_attention` inputs declared **static** at `[4096,1,256]` and `[28736,1,64]`.
   Resizing requires a real re-export at each resolution, not a shape override. A 512² re-export is the single highest-value engineering change for SAM 2, but it is outside this study.
2. **The iGPU is slower than the CPU for fp32.** The encoder is one fused DML graph yet takes 1655 ms vs 774 ms on CPU; memory attention takes 2302 ms vs 637 ms. The GPU has 2 CUs and shared memory, while the CPU has 32 Zen 5 threads.
3. **fp16 does not rescue DML.** DML fp16 encoder 1366 ms and memory attention 2585 ms; CPU fp16 is slower still (encoder ~1561–1593 ms across runs) because fp16 is emulated. Numerical mask quality is fine (0.996 vs fp32), but there is no speed win.
4. **ORT tuning has no effect.** `disable_metacommands`, `performance_preference=high_performance`, `enable_dynamic_graph_fusion=false`, `ORT_SEQUENTIAL` vs `ORT_PARALLEL`, and graph-optimization levels all leave the encoder at 1654–1662 ms. The bottleneck is the device, not the session configuration.
5. **The memory bank is fixed at 28736 tokens.** Supplying 1-token or 7168-token `memory`/`memory_pos` is rejected (`Invalid rank/dimensions`); all inputs are static. Reducing the memory bank also requires a re-export.
6. **CPU thread tuning is marginal.** 774 ms default vs 759 ms at 16 threads and 699 ms at 8 threads; 8 threads has worse p95. Recommend 16 threads for stability, or leave ORT's default.

**Conclusion:** SAM 2's failure is throughput, not parity. Per-frame tracking is 0.24 fps DML / 0.68 fps CPU. The way to use it on this machine is **keyframes only**, not per-frame propagation.

### 3.2 EfficientTAM-Ti: the parity failure is fp16 compute

- The Task 0 bundle is `fp16` (`bundle.toml: variant = "fp16"`). Its DML outputs differ from the CPU fp16 reference by 2.6e-3 to 2.35e-2 because CPU fp16 and DML fp16 accumulate in different orders/precision.
- The **fp32 bundle from the same repository** (`egordm/efficienttam-ti-512`, `fp32/`) passes every component: worst is `mask_decoder` at **3.1e-4**, all others ≤3.94e-5. This isolates the cause to fp16 arithmetic, not the ONNX export, the bundle, or graph fusion.
- Disabling graph optimizations, metacommands, or dynamic fusion does **not** fix fp16 parity (5.9e-3 – 1.11e-2 on `mask_decoder`). It is intrinsic.
- The fp16 **thresholded mask** is still close (Task 0 random-logit mask IoU ~0.986–0.988), so a mask-IoU gate would pass, but the strict ≤1e-3 logit gate does not. (SAM 2 fp16, by contrast, measured 0.996 mask IoU vs fp32 on a real object; see §2.1.)
- **Fix:** ship the fp32 bundle. It is ~77 MB (vs ~40 MB fp16), still small. The fp32 memory attention is heavily fragmented on DML (1389 DML + 576 CPU nodes) and is faster on CPU (195.7 ms vs 224.2 ms). Run EfficientTAM fp32 on **CPU**: full tracking 250 ms (4.0 fps), keyframe 45 ms (22 fps).

### 3.3 LaMa: the `0x80070057` is an invalid `Reshape allowzero`

- Bisection by extracting graph prefixes found the first DML-failing node at index 40: **`node_view` (Reshape)** in the first FFC block. The same node is the runtime failure in the DFT export (`lama_fp16.onnx`: `Reshape node 'node_view' ... 80070057`).
- The Reshape shape is `[1, -1, 64, 33]` with **`allowzero=1`**. ONNX defines `allowzero=1` together with `-1` as invalid; ORT CPU tolerates it and infers the missing dimension, while the DML EP's operator author returns `E_INVALIDARG`.
- There are **72 such Reshape nodes** in the fixed-512 graph. Patching `allowzero` to 0:
  - is exactly behaviour-preserving on CPU (`maxRel = 0` vs the unpatched fp32 graph);
  - lets the DML session initialise (session creation 588–608 ms);
  - gives DML fp32 parity `1.13e-5`.
- **But DML is slower than CPU** (1328 ms vs 542 ms fp32; 1422 ms int8; 1128 ms fp16 with 8.8% error). The iGPU is not the right device for LaMa's 432 MatMul FFC blocks either.
- **Deployment recommendation:** LaMa **int8 + allowzero=0 on CPU** (532.6 ms, 1.88 fps, 62 MB, quality within 0.2 dB PSNR of fp32). If the fp32 graph is preferred, 541.8 ms / 1.85 fps / 209 MB.

### 3.4 MI-GAN: fixed 512 canvas plus a mask-convention trap

- The generator always processes a **512² crop**. 128², 256² and 512² inputs all cost 435–450 ms DML; the pipeline resizes the cropped region to 512². ROI size does not reduce the floor.
- A 1080p input adds ~100 ms of preprocessing (547 ms vs 449 ms at 512²). Cropping the ROI outside the ONNX graph and feeding a 512² patch removes that overhead.
- **Mask convention:** the official `create_onnx_pipeline.py` prepares `mask - 0.5` and `image * mask`, and composes `image * mask + output * (1 - mask)`. That means **255 = known/background, 0 = hole**. Feeding 255 in the hole makes the model copy/attenuate the wrong region and returns a plausible-but-wrong image. The Task 0 random sparse mask did not distinguish the two conventions. This must be a pinned contract with a pixel test.
- **fp16** is not available as a prebuilt and naive conversion is invalid at the shape/comparison nodes. **int8** was not pursued; the offline budget is already met.
- Quality is materially below LaMa on every tested hole type. MI-GAN's advantage is only ~84 ms/frame over LaMa int8 CPU (449 ms DML vs 533 ms CPU), which is a ~16% time saving for a large quality drop. Keep MI-GAN as the fast/preview path, not the default quality path.

---

## 4. Engineering alternatives

### 4.1 Keyframe tracking + optical-flow interpolation — the main enabler

- Run the segmenter only on keyframes; propagate the mask with **OpenCV DIS medium** on every frame.
- Measured real-sequence accuracy: **0.947 mean IoU at K=5**, **0.913 at K=10** vs per-frame SAM 2; **0.985 at K=2**. Direct flow from the last keyframe (no accumulation) is essentially the same (0.946 / 0.907).
- DIS cost at 1080p: **38.4 ms/frame (26 fps)**; at 720p 19.0 ms (53 fps); at 540p 13.8 ms (73 fps). Farneback is 5.4× slower at 1080p and is not recommended.
- Flow is well-posed on textured objects. It degrades on fast motion (20 px/frame: 0.873 at K=5, 0.808 at K=10), on large disocclusion, and on articulated boundaries. The plan already scopes out fast/complex motion; keep the keyframe-correction UI and confidence band.
- **Effective tracking fps at 1080p** (300 frames):
  - EfficientTAM fp32 keyframes (45 ms) at K=5: 60×0.045 + 300×0.0384 = **14.2 s → 21.1 fps**;
  - SAM 2 fp32 keyframes (855 ms = encoder+decoder) at K=10: 30×0.855 + 11.5 = **37.2 s → 8.1 fps**;
  - Per-frame SAM 2 CPU: 300×1.474 = 442 s → 0.68 fps.

### 4.2 ROI-only repair and external cropping

- MI-GAN's floor is the 512² generator (435–450 ms DML), independent of the hole size. Cropping the ROI **outside** the graph avoids the 1080p preprocessing (~100 ms/frame) and avoids feeding a 1080p tensor to a 512² model.
- For a mask larger than 512², tile with 32–64 px overlap; each tile costs another ~435 ms. A 10%-area object fits one tile.
- LaMa is fixed at 512² as well; the same external crop applies. Zero the hole in the LaMa input (the model card requires it) and use mask 1 = hole; for MI-GAN use 255 = known, 0 = hole.

### 4.3 540p/720p processing, then upscale patches

- Tracking: DIS at 540p is 13.8 ms vs 38.4 ms at 1080p (2.8× faster); at 720p 19.0 ms (2×). Mask propagation quality at 540p is close on synthetic rigid motion (K=5: 0.988 at 540p vs 0.985 at 1080p for slow motion; 0.947 vs 0.955 for 8 px/frame). A 540p mask upscaled to 1080p is a viable speed/quality trade for low-detail objects.
- Repair: MI-GAN/LaMa are fixed at 512², so processing the frame at 540p does not reduce the generator cost. It only reduces decode/crop overhead. Do not expect inpainting speedups from resolution reduction.
- Patch upscale cost (OpenCV, 512→1080p crop) is ~1–3 ms and is negligible.

### 4.4 Frame skipping and temporal blending

- Repair every 2nd frame and blend/warp from neighbours: halves the repair time (MI-GAN 65 s, LaMa 81 s for 300 frames) at the cost of ghosting on fast motion and a more complex stabilizer. Keep as an advanced "fast draft" option, not the default.
- Per-frame image inpainting has no temporal memory; flicker is a real risk even without skipping. The plan's temporal stabilization (flow-warp neighbouring repaired frames + blend + optional temporal median in the hole) remains required. Budget ~10–12 s per 300 frames (estimate, 30–40 ms/frame).

### 4.5 MobileSAM (image-only) + flow

- Not measured. It would replace SAM 2/EfficientTAM at keyframes, but EfficientTAM fp32 already runs a keyframe in **45 ms (22 fps)** on CPU, so MobileSAM's expected advantage is small and it adds another model, license/data review, and a weaker mask. **Not needed** for the reduced scope. If EfficientTAM's 0.879 IoU vs SAM 2 proves insufficient on the M4 corpus, try SAM 2 at 1024² on keyframes first (855 ms) before adding MobileSAM.

### 4.6 CPU/offline acceptance

- The measured CPU/offline path is the practical one on this machine. The original interactive gates (SAM 2 ≥5 fps per-frame, EfficientTAM ≥8 fps, MI-GAN ≥10 fps, LaMa ≥2 fps) are not reachable on the iGPU or CPU. The offline acceptance target (1080p30 10 s ≤10 min) is reachable with margin (2.6–3.5 min). This is the scope change the data supports.

---

## 5. Best viable pipeline for the current AMD machine

### 5.1 Pipeline

```
User mask on a reference frame (brush/box)
        │
        ├─ Default keyframe segmenter: EfficientTAM-Ti 512 fp32, CPU (45 ms/keyframe, 22 fps)
        │     mask IoU vs SAM 2 1024²: 0.879 on the real test object
        │
        ├─ Quality fallback: SAM 2.1 Tiny fp32, CPU (855 ms/keyframe)
        │     mask IoU vs per-frame SAM 2: 1.0 by construction
        │
        ├─ Propagation: OpenCV DIS medium at processing resolution
        │     1080p 38.4 ms/frame (26 fps), 720p 19.0 ms, 540p 13.8 ms
        │     keyframe interval K=5 (accuracy 0.947) or K=10 (0.913)
        │
        ├─ Repair (offline, external 512² ROI crop, patch pasted back with feather):
        │     fast path: MI-GAN 512, DML, 449 ms/frame (2.23 fps)
        │     quality path: LaMa 512 int8, CPU, 533 ms/frame (1.88 fps)
        │
        ├─ Temporal stabilization: DIS-warp neighbour repairs, blend, optional median
        │     ~30–40 ms/frame (estimate)
        │
        └─ Cache / manifest / export preflight (unchanged M4 architecture)
```

### 5.2 Expected performance

| Scenario (1080p30, 10 s = 300 frames, one object ≤10% area) | Tracking | Repair | Stabilization (est.) | Total | Classification |
|---|---:|---:|---:|---:|---|
| Static/rigid object, MI-GAN fast path | 0 s | 130.5 s | 12 s | **142.5 s (2.4 min)** | offline-acceptable |
| Static/rigid object, LaMa int8 quality path | 0 s | 159.8 s | 12 s | **171.8 s (2.9 min)** | offline-acceptable |
| EfficientTAM K=5 + MI-GAN | 14.2 s | 130.5 s | 12 s | **156.7 s (2.6 min)** | offline-acceptable; tracking ~21 fps |
| EfficientTAM K=5 + LaMa int8 | 14.2 s | 159.8 s | 12 s | **186.0 s (3.1 min)** | offline-acceptable; best quality/time |
| SAM 2 K=10 + MI-GAN | 37.2 s | 130.5 s | 12 s | **179.7 s (3.0 min)** | offline-acceptable; best mask quality |
| SAM 2 K=10 + LaMa int8 | 37.2 s | 159.8 s | 12 s | **209.0 s (3.5 min)** | offline-acceptable |
| Per-frame SAM 2 CPU + MI-GAN | 442.2 s | 130.5 s | 12 s | **584.7 s (9.7 min)** | borderline; tracking 0.68 fps |
| Per-frame SAM 2 DML + MI-GAN DML | 1234.8 s | 130.5 s | 12 s | **1377.3 s (23.0 min)** | unusable |

Notes:
- Repair time assumes one 512² tile per frame. A large object needing four tiles multiplies the repair time by 4 (e.g., 8.9 min for MI-GAN), still within the 10-minute target only for the fast path.
- Stabilization is an estimate; it was not implemented as a product stage in this study. Including or excluding it does not change the classification.
- Tracking fps is the effective throughput of the segmenter + flow, not the interactive display rate. The mask overlay can update at DIS speed (26 fps at 1080p); EfficientTAM keyframes are 45 ms, SAM 2 keyframes are 855 ms.

### 5.3 Quality expectations

- Mask: EfficientTAM fp32 at 512² is 0.879 IoU vs SAM 2 1024² on a small real object; SAM 2 keyframes are the quality fallback. Flow propagation adds 0.913–0.947 agreement with per-frame SAM 2 at K=10/K=5 on real articulated motion.
- Repair: LaMa int8 is within 0.2 dB PSNR of fp32; MI-GAN is 3–14 dB lower on structured holes. Use LaMa for quality, MI-GAN for draft/preview.
- Temporal: per-frame inpainting flicker is not measured here; the stabilizer is still required and the "slow/medium target" limitation copy remains honest.

---

## 6. Verdict per model

| Model | Session / runtime | Numerical parity | Speed | Verdict |
|---|---|---|---|---|
| **SAM 2.1 Hiera-Tiny** | 5/5 DML and CPU | fp32 mask IoU 1.0; fp16 mask 0.996 | 0.24 fps DML, 0.68 fps CPU per-frame; 8.1 fps effective with K=10 + flow | **Usable offline / keyframe-quality path.** Not usable interactive per-frame. |
| **EfficientTAM-Ti 512 fp32** | 5/5 DML and CPU | **passes** (worst 3.1e-4); fp16 fails (2.6e-3–2.35e-2) | 4.0 fps CPU full step; **22 fps keyframe CPU**; 2.06 fps DML fp32 | **Usable interactive-ish for keyframes; usable offline for per-frame.** fp16 unusable. |
| **MI-GAN 512** | DML + CPU | exact / 1 LSB | 2.23 fps DML; 435–450 ms fixed | **Usable offline. Unusable interactive** (≥10 fps unreachable). |
| **LaMa 512 fp32/int8** | CPU; DML after allowzero patch | 1.13e-5 / 1.36e-5; fp16 DML 8.8% (unusable) | **1.88 fps CPU int8**; DML 0.70–0.75 fps | **Usable offline on CPU.** DML feasible but not faster. |
| **MobileSAM** | not measured | — | — | **Not needed** while EfficientTAM keyframes are 45 ms. |

---

## 7. Recommendation

**Continue M4 with a reduced technical scope** on the current AMD machine. Concretely:

1. **Reframe M4 as an offline generation feature**, not real-time repair. Keep the existing AI subprocess, content-addressed cache, preview compare, and export preflight architecture. Show progress/ETA; do not promise live inpainting.
2. **Make EfficientTAM-Ti 512 fp32 on CPU the default keyframe segmenter** (45 ms/keyframe, 22 fps, 0.879 IoU vs SAM 2 on the test object). Keep **SAM 2.1 Tiny fp32 CPU** as the quality fallback (855 ms/keyframe) for hard objects or an explicit "high quality" mode. Reject the fp16 EfficientTAM bundle for production.
3. **Use DIS medium + keyframe interval 5 by default** (0.947 agreement with per-frame SAM 2), with interval 10 as a faster option (0.913). Expose the interval in the advanced panel. Keep the confidence band, low-confidence stop, and keyframe correction. Do not attempt per-frame SAM 2.
4. **Repair path:** LaMa 512 int8 (`allowzero=0`) on CPU is the quality default (533 ms/frame, 1.88 fps, 62 MB, quality ≈ fp32). MI-GAN 512 on DML is the fast/draft path (449 ms/frame, 2.23 fps). MI-GAN remains useful for small logos/watermarks on smooth backgrounds and preview, but LaMa int8 is only ~19% slower and much better on structured holes.
5. **Pin the integration contracts:** MI-GAN mask 255 = known / 0 = hole; LaMa mask 1 = hole and zero the hole; EfficientTAM prompt encoder expects 3 sparse slots (pass the real point plus a padding point with label −1); SAM 2 points are in 1024² coordinates (scaled from the source image).
6. **Crop the ROI outside the ONNX graph** and feed a 512² patch; paste back with feathering. This removes ~100 ms/frame of 1080p preprocessing and keeps the cost fixed.
7. **Keep the temporal stabilizer**; per-frame inpainting has no temporal memory. Budget ~10–12 s per 300 frames.
8. **Update the model manifest before implementation:** add the EfficientTAM fp32 bundle files and the LaMa `allowzero=0` graph (new hashes below), keep `commercialApproved=false`, `legalSignOff` empty, `releaseBlocked=true`, `technicalGate` unchanged until legal and reference-hardware sign-off. Do not change the approval as part of this research.

### 7.1 When to choose the alternatives instead

- **Change hardware** if the product requires interactive per-frame SAM 2-class tracking (≥5 fps) or MI-GAN ≥10 fps on the target machine. The iGPU is 2-CU-class and DML is 2.1× slower than CPU for SAM 2; a discrete RDNA3/RDNA4 or RTX 3060-class GPU is the only route to the original gates. Re-run the same harness there.
- **Change models** only if interactive repair is non-negotiable: the 512² MI-GAN/LaMa floor cannot exceed 2.3 fps on this iGPU. A smaller inpainting export (e.g., a 256² generator) or a distilled model is the path; re-export and re-measure. A 512² SAM 2 re-export would also help tracking, but it is an engineering task, not a silver bullet.
- **Keep M4 paused** if legal cannot clear the training-data terms (SA-1B, Places2, YouTube-VOS). The technical work does not change `commercialApproved=false`.

---

## 8. Updated thresholds and risks

### 8.1 Proposed threshold changes

| Original gate | Measured result | Proposed replacement |
|---|---|---|
| SAM 2 Tiny ≥5 fps per-frame at 1080p | 0.24 fps DML / 0.68 fps CPU | **Effective tracking ≥5 fps via keyframe+flow**; keyframe segmenter ≥10 fps; mask agreement vs per-frame SAM 2 ≥**0.90 at K=10** / ≥**0.94 at K=5** on the M4 corpus |
| EfficientTAM ≥8 fps | 2.06 fps DML fp32 / 4.0 fps CPU full; 22 fps keyframe | **Keyframe ≥10 fps** and **full step ≥2 fps** on CPU fp32; fp16 is rejected |
| MI-GAN ≥10 fps | 2.23 fps DML | **Offline ≤3 min per 300 frames, one 512² tile**; ≤6 min up to 4 tiles; no interactive gate |
| LaMa ≥2 fps | 1.85 fps CPU fp32 / 1.88 fps int8; DML 0.75 fps | **CPU ≥1.8 fps; offline ≤4 min per 300 frames**; DML optional, not required |
| SAM 2 DML/CPU mask IoU ≥0.98 | fp32 1.0; fp16 0.996 | Keep **0.98 for mask IoU**; do not use the raw-logit ≤1e-3 gate for fp16 models (fp16 logits fail, masks pass) |
| VRAM ≤4 GB | 3.6 GB measured on the iGPU (Task 0) | Keep as a target, but measure on a discrete GPU with `QueryVideoMemoryInfo`; the iGPU shares system memory |

### 8.2 Integration contracts to add

1. **MI-GAN mask:** 255 = known/background, 0 = hole. Add a pixel test that fails if the convention is inverted.
2. **LaMa mask:** 1 = hole, 0 = known; zero the image inside the hole before inference; input RGB 0–1, fixed 512², output RGB 0–1.
3. **EfficientTAM prompt:** point coordinates in 512² coordinates; pass the real point plus a padding point `(0,0)` with label `-1` so the sparse prompt has 3 slots. Passing a single point yields 2 slots and a bad mask.
4. **SAM 2 prompt:** coordinates in 1024² image space (source coordinates × 1024/width, × 1024/height). The prompt encoder divides by 1024 internally.
5. **LaMa ONNX patch:** `allowzero=1 → 0` on the 72 Reshape nodes, or an equivalent exporter setting. Validate CPU output is bit-identical before shipping.
6. **DML provider assertion:** always check `DmlExecutionProvider` is in `get_providers()`; do not treat a CPU-only session as GPU acceleration.

### 8.3 Risks

| Risk | Evidence | Mitigation |
|---|---|---|
| Flow drift on articulated/fast motion | 0.856 at K=30; 0.873 at 20 px/frame even at K=5 | Keyframe interval 5; confidence band; keyframe correction; scope out fast/complex motion |
| Per-frame inpainting flicker | No temporal memory in MI-GAN/LaMa; not measured as a product stage | Keep the temporal stabilizer; verify VFID/E_warp on the M4 corpus |
| iGPU thermal/shared-memory variance | 2-CU iGPU, 31 GiB shared; no discrete VRAM | Re-measure on reference hardware; keep the fallback ladder; no VRAM claims |
| fp16 numerical traps | EfficientTAM fp16 fails parity; LaMa fp16 DML 8.8% error | Ship fp32/int8; gate fp16 on mask IoU, not logits |
| Model hash / manifest drift | New fp32 EfficientTAM and patched LaMa artifacts are not in the manifest | Add hashes before Task 5/6; keep fail-closed |
| MI-GAN mask convention | Wrong convention returns plausible-but-wrong images | Pixel test + documented contract |
| Licensing unresolved | `TECHNICAL_EVALUATION_ONLY` | Legal sign-off remains the release gate; do not describe the shortlist as commercially clean |
| Re-export dependency | SAM 2 is fixed at 1024²; memory bank fixed at 28736 | Treat a 512² SAM 2 re-export as a separate engineering task with its own parity/quality gate |
| Quality of EfficientTAM at 512 on small objects | 0.879 IoU vs SAM 2 on a small player | Use SAM 2 keyframes for small/hard objects; add a quality mode |

---

## 9. Reproduction

### 9.1 Key commands (spike venv, models outside the repo)

```bash
# CPU vs DML, one graph
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/bench.py \
  --model C:/m4-spike/models/sam2_1_tiny_video_onnx/onnx/vision_encoder.onnx \
  --provider both --runs 5 --json

# SAM 2 full tracking step on a real frame
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/sam2_pipeline.py \
  --image C:/m4-spike/frames/badminton_f0.png --point 395,520 --runs 2 --with-memory

# EfficientTAM fp32 keyframe and mask IoU vs SAM 2
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/efficienttam_pipeline.py \
  --image C:/m4-spike/frames/badminton_f0.png --point 395,520 \
  --sam2-mask C:/m4-spike/results/opt/sam2_f0_mask_cpu.npy

# LaMa DML after the allowzero patch (build the variant first)
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/bench.py \
  --model C:/m4-spike/models_opt/lama/lama512_int8_allowzero0.onnx \
  --provider cpu --runs 6 --shape input=1,4,512,512 --input-spec input=noise

# MI-GAN at 512 with a localized mask
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/bench.py \
  --model C:/m4-spike/models/migan_512/migan_pipeline_v2.onnx \
  --provider both --runs 6 --shape image=1,3,512,512 --shape mask=1,1,512,512 \
  --input-spec image=image --input-spec mask=mask_bbox:0.45,0.4,0.1,0.1

# Flow timing and keyframe+flow accuracy
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/flow_eval.py --mode timing
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/flow_vs_sam2.py

# Inpainting quality matrix
C:/m4-spike/venv/Scripts/python.exe -I C:/m4-spike/scripts/opt/inpaint_matrix.py
```

### 9.2 Artifacts referenced by this report

All are outside git under `C:/m4-spike`:

| Artifact | SHA-256 |
|---|---|
| EfficientTAM fp32 `image_encoder.onnx` | `174545be260d1946e1a547216e6cd5174431ae8a6c34c33cb53f84e5902af1f7` |
| EfficientTAM fp32 `mask_decoder.onnx` | `70fb9990d612161efeacbc6bd834d5ec2cee7a98f4f000dc1a2d4620ac95c1a2` |
| EfficientTAM fp32 `memory_attention.onnx` | `e94c24ee72ed05ec97bf6c26f250d91f5a56cc49ffd51e5fed5ee4119d242308` |
| EfficientTAM fp32 `memory_encoder.onnx` | `7f635bbfffcffbc0862a2fb4ef30edcbd2d717ebf93362388211d6b18c9e8420` |
| EfficientTAM fp32 `prompt_encoder.onnx` | `6391ae753283ec36c73904dff41eea0c35d57f142dcb73e512a785b8994d6517` |
| EfficientTAM fp32 `bundle.toml` | `fef4423f1791650f38a8982398e99b50e8331b8e29328f7641b525b0b5fb0a92` |
| EfficientTAM fp32 `maskmem_tpos_enc.npy` | `eb408427700661f19dca81119ad71b19d088dacdb27c824cf3a407365579be57` |
| EfficientTAM fp32 `no_obj_ptr.npy` | `189c46a46ecd991ab8ee797c05ef019555c23fab2cba40b658222e5ece0ed7b9` |
| LaMa 512 **int8 + allowzero=0** (recommended CPU artifact) | `2a875d42de7b31779de731cb390903cf3ba29b7615a2b23f5f6976c293e21407` |
| LaMa 512 fp32 + allowzero=0 (evaluation) | `793c6b49f47e192abebfdf52a17def9e630895c37241aa7bb731f84e812e18d6` |
| MI-GAN pipeline (unchanged) | `6f1f3530a1a2324b19752018ce756088b07973cda8d7d890034ace5c8a48c40b` |

The existing Task 0 hashes remain valid for the original SAM 2 and EfficientTAM fp16 bundles. The fp32 EfficientTAM and patched LaMa hashes above must be added to `assets/models/models.manifest.json` before Task 5/6, with the same fail-closed flags.

### 9.3 Verification performed

- All timings are from real ORT sessions on the listed files; synthetic imagery is used only for flow/quality accuracy tests and is labelled as such.
- Session provider lists were asserted; a DML session without `DmlExecutionProvider` is a failure.
- LaMa `allowzero` patch validated bit-identical on CPU (`maxRel = 0`).
- SAM 2 fp32 CPU vs DML mask IoU 1.0 on a real object; fp16 0.996.
- EfficientTAM fp32 CPU vs DML mask IoU 1.0; fp32 component parity ≤3.1e-4.
- MI-GAN/LaMa output conventions cross-checked against the official MI-GAN pipeline source and the LaMa model card.
- No `src/` change; no model binary in git; manifest approval unchanged.

---

## 10. Bottom line

- **The original interactive M4 gates are not reachable on this machine.** The iGPU is slower than the CPU for SAM 2, EfficientTAM and LaMa; MI-GAN cannot exceed 2.3 fps.
- **A reduced offline M4 is reachable now:** 2.6–3.5 min for a 10 s 1080p single-object removal, with 8–21 fps effective tracking via keyframe+flow and 0.91–0.95 mask agreement with per-frame SAM 2.
- **The highest-value engineering changes are** (a) EfficientTAM fp32 on CPU as the default keyframe segmenter, (b) LaMa int8 CPU as the quality repair path with MI-GAN as the fast path, (c) the LaMa `allowzero` fix, and (d) the MI-GAN mask-convention contract.
- **The highest-value future engineering change is a 512² SAM 2 re-export** with a reduced memory bank; it is the only way to make SAM 2-class per-frame tracking competitive on this hardware.
- **Licensing remains the release blocker.** Technical viability does not imply commercial approval.
