# M4 AI removal acceptance — technical evaluation

- Date: 2026-11-16
- Branch: `feature/m4-ai-removal`
- Base commit: `6b9eeb3`; Task 9 delivery `ab80f7a` / `e480696`, final
  prompt/DIS/corpus round on top (see `git log` on this branch)
- Scope: **internal technical evaluation only**. This document is not a
  commercial-readiness sign-off.

> **Release status: BLOCKED.**
> `assets/models/models.manifest.json` keeps `commercialApproved=false`,
> `releaseBlocked=true` and an empty `legalSignOff`. There is no legal approval
> for the model training data (SA-1B, Places2, YouTube-VOS and related terms).
> The product must not ship AI removal on the basis of this report.
>
> **The planned three-reviewer subjective review (1–5 scores) was NOT
> performed.** Only deterministic automated metrics and one human run of the
> selftest are recorded here.

## 1. Environment

| Item | Value |
| --- | --- |
| OS | Windows 11 Pro 10.0.26300 (build 26300), x64 |
| CPU | AMD Ryzen 9 9950X, 16 cores / 32 threads, 4.3 GHz |
| RAM | 48,282 MB |
| GPU | **AMD Radeon(TM) Graphics** (integrated, PCI `1002:13C0`), driver `32.0.21042.62` |
| GPU memory | 485.7 MiB dedicated (509,358,080 B), 31.15 GiB shared system memory |
| ONNX Runtime | `onnxruntime-directml` 1.22.0 (`DmlExecutionProvider`, `CPUExecutionProvider`) |
| Qt / toolchain | Qt 6.8.3, MSVC 2022, bundled CMake, preset `windows-x64-release` |
| FFmpeg | LGPL shared 7.1 (`BtbN` build), used by decode/fixture generation |
| Models | `C:/m4-spike/models` (outside the repo; binaries not committed) |
| Build | Release (`build/windows-x64-release`) |

Hardware caveat: the only GPU is an **integrated** Radeon with shared memory.
No discrete AMD card and no NVIDIA RTX 3060-class machine were available, so
the plan's discrete-GPU expectation is not measured. VRAM figures on an iGPU
are shared system memory counters, not dedicated VRAM.

Real-run environment:

```
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models
VIDEOEDITOR_M4_TECHNICAL=1
```

## 2. Fixture matrix

| Fixture | Geometry | Location | Use |
| --- | --- | --- | --- |
| `static_logo.mp4` / `_mask` / `clean_background.mp4` | 640x360, 30 fps, 5 s | committed | legacy fallback + prompt/IoU tests |
| `slow_target.mp4` / `slow_target_mask.mp4` | 640x360, 30 fps, 5 s | committed | K=5 DIS propagation IoU |
| `occlusion.mp4` | 640x360, 30 fps, 5 s | committed | decode corpus (not model-scored) |
| `static_logo_smooth.mp4` / `_mask` / `clean_gradient.mp4` | 640x360, 30 fps, 5 s | committed | smooth fallback (fair GT) |
| `static_logo_soft_1080p_10s.mp4` / `_mask` / `clean_soft_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | `C:/m4-spike/acceptance/` | **soft low-frequency acceptance (preferred, fair GT)** |
| `static_logo_smooth_1080p_10s.mp4` / `_mask` / `clean_gradient_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | `C:/m4-spike/acceptance/` | smooth-gradient acceptance (fair GT) |
| `static_logo_1080p_10s.mp4` / `_mask` / `clean_background_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | `C:/m4-spike/acceptance/` | legacy/high-frequency acceptance |

Coverage gaps: only 30 fps was exercised; 24/60 fps, 4 GB/6 GB discrete VRAM
variants and a full CPU-only end-to-end run were **not** performed. The
Chinese-path project round-trip is a mask-store unit test, not a full model run.

Corpus notes:

- The soft set is the preferred fair corpus: outside-hole source-vs-GT PSNR
  70.58 dB, mean-fill 50.43 dB, ideal smooth-fill cap 60.85 dB (frame 0).
- The initially supplied smooth 1080p plate/GT had mismatched random
  `gradients` geometry (outside-hole 7.02 dB, invalid GT); they were regenerated
  with pinned `seed=12345` and endpoints (outside-hole 66.25 dB). The committed
  640x360 smooth fixtures use the same pinned seed.

## 3. Method — `VideoEditor.exe --selftest-removal`

1. Select a fixture set. Default preference: **soft 1080p → smooth 1080p →
   legacy 1080p → repo smooth 640x360 → repo legacy 640x360**.
   `VIDEOEDITOR_M4_ACCEPTANCE_SET=soft|smooth|legacy|repo` forces one set so
   every row is reproducible. The selected set is printed and echoed in the
   result line.
2. Probe the clip, build a `Project` with one video track/clip and a
   `RemovalLayer` (`sourceIn=0`, `sourceOut=duration`, Fast/MI-GAN, one mask
   keyframe at 0).
3. Decode mask frame 0, threshold luma > 127, write it through
   `MaskSidecarStore`, set `clip.removalLayerId`.
4. Derive the cache key with `RemovalPreflight::keyInputFor()` +
   `computeRemovalCacheKey()` so it matches export preflight exactly.
5. Run the real `AiJobSpec` on a `QEventLoop` with a 600 s timeout.
6. Require `state()==Complete`, `verify()`, `result.completedFrames ==
   manifest.totalFrames`, and `framesWithPatch == totalFrames` (every frame of
   this static-mask clip must contribute an alpha>0 repair pixel). Clean frames
   must decode at the exact display size before any pixel comparison.
7. Compute hole PSNR: every patch pixel with alpha > 0 is compared with the
   clean-GT frame at the same global frame/crop. The timer starts immediately
   before the job and excludes probe/mask/key setup, so `seconds=` is **job
   time**, not full end-to-end wall time.
8. Print one line and exit 0/4. Without a verified model bundle it prints
   `REMOVAL_SELFTEST_SKIP` and exits 4.

Tracker prompt: `Sam2Tracker::promptFromMask` uses a **SAM-style 2-point box**
(top-left label 2, bottom-right label 3), no centroid. The prompt encoder emits
exactly three sparse tokens for a 2-point prompt and these are used as-is; a
legacy 3-point prompt (4 tokens) still drops only the trailing padding token.

DIS: `FlowPropagator` uses `PRESET_FAST`, a cached `(x,y)` grid plus one
vectorized `cv::add` (no per-pixel map build), `cv::remap` with the CV_32FC2
map, `cv::compare` + `countNonZero` for the mask, and `cv::magnitude` +
`meanStdDev`/`minMaxLoc` for metrics.

Repair cache: RGB is zeroed where alpha == 0 (straight alpha, SRC_ALPHA, no
premultiply) and each patch is trimmed to its alpha>0 bounding box with its
crop rebased, so the compositor/`RepairFrameSource` mapping stays exact.

## 4. Selftest results (real 1080p10s runs, exit 4)

### 4.1 Soft low-frequency set (preferred)

```
REMOVAL_SELFTEST_FAIL threshold set=soft-1080p
  file=C:/m4-spike/acceptance/static_logo_soft_1080p_10s.mp4
  psnr=8.71 psnrThreshold=28.00 seconds=164.1 timingThreshold=300
  cacheBytes=13226502 cacheBudget=1805744 sourceBytes=902872
  completedFrames=300 frames=300 provider=tracker=cpu;inpaint=dml
```

### 4.2 Smooth-gradient set

```
REMOVAL_SELFTEST_FAIL threshold set=smooth-1080p
  file=C:/m4-spike/acceptance/static_logo_smooth_1080p_10s.mp4
  psnr=13.53 psnrThreshold=28.00 seconds=164.0 timingThreshold=300
  cacheBytes=11705523 cacheBudget=1840644 sourceBytes=920322
  completedFrames=300 frames=300 provider=tracker=cpu;inpaint=dml
```

### 4.3 Legacy high-frequency set

```
REMOVAL_SELFTEST_FAIL threshold set=legacy-1080p
  file=C:/m4-spike/acceptance/static_logo_1080p_10s.mp4
  psnr=8.04 psnrThreshold=28.00 seconds=205.1 timingThreshold=300
  cacheBytes=31174721 cacheBudget=36015140 sourceBytes=18007570
  completedFrames=300 frames=300 provider=tracker=cpu;inpaint=dml
```

| Row | PSNR | Job time | Cache / source | Cache verdict | PSNR verdict |
| --- | --- | --- | --- | --- | --- |
| Soft 1080p10s | **8.71 dB** | **164.1 s** | 13,226,502 / 902,872 = **14.65x** | **FAIL** (>2x) | **FAIL** |
| Smooth 1080p10s | **13.53 dB** | **164.0 s** | 11,705,523 / 920,322 = **12.72x** | **FAIL** (>2x) | **FAIL** |
| Legacy 1080p10s | **8.04 dB** | **205.1 s** | 31,174,721 / 18,007,570 = **1.73x** | **PASS** (≤2x) | **FAIL** |

All three jobs completed 300/300 frames with a complete, hash-verified cache.
Job time is inside the 300 s budget; the soft/smooth runs (164 s) are also inside the 2.6–3.1 min target, while the legacy run (205 s) is above it.

## 5. Why hole PSNR fails (reproducible evidence)

Analysis script: `tools/m4_spike/hole_psnr_baseline.py` (numpy + opencv):

```
C:/m4-spike/venv/Scripts/python.exe -I tools/m4_spike/hole_psnr_baseline.py \
  --video C:/m4-spike/acceptance/static_logo_soft_1080p_10s.mp4 \
  --mask  C:/m4-spike/acceptance/static_logo_soft_mask_1080p_10s.mp4 \
  --clean C:/m4-spike/acceptance/clean_soft_1080p_10s.mp4 --frame 0
```

Frame 0 baselines (hole bbox x1560..1799, y90..239, 36,000 px on all sets):

| Baseline | Soft | Smooth | Legacy |
| --- | --- | --- | --- |
| Source vs clean **outside** the hole | 70.58 dB | 66.25 dB | 70.33 dB |
| Mean-colour fill inside the hole | **50.43 dB** | **33.45 dB** | **10.35 dB** |
| Ideal smooth fill (51 px blur of GT) | **60.85 dB** | **51.08 dB** | **22.85 dB** |
| Hole BGR mean / std | B 91.6/0.6, G 60.0/0.8, R 51.0/0.8 | B 33.0/4.6, G 138.6/3.6, R 204.9/7.4 | B 254.7/0.7, G 212.5/95.0, R 42.3/94.7 |

- Soft and smooth are **fair** GTs: a trivial mean fill already reaches
  50.43 / 33.45 dB, and an ideal smooth fill 60.85 / 51.08 dB. The measured
  MI-GAN fill is 8.71 / 13.53 dB. The model's generative fill is therefore the
  limiting factor, not the metric. Outside-hole preservation is exact on the
  contract crop (mean channel difference 0.033; 18.2 with R/B swapped), so the
  cache, channel order and preprocessing are correct; the in-hole colour is
  wrong (640x360 smooth patch mean RGB 255,244,56 vs GT 243,157,11; 128x128
  contract centre 57,97,144 vs expected 120,140,120; zeroing the hole content
  before inference gives 38,49,62, i.e. worse).
- Legacy is a **pathological** high-frequency `testsrc2` GT: no smooth fill can
  reproduce the pattern phase (cap 22.85 dB), so it is not a model-quality
  signal. It also varied 8.04–10.29 dB between runs (DirectML nondeterminism).
- An independent Python MI-GAN/LaMa reference on the soft hole scores ~16–18 dB
  (plausible texture, not the exact low-frequency gradient). The Qt engine
  measures 8.71 dB, lower than that reference, so the engine's fill quality
  needs investigation in M5. **The 28 dB gate is an exact-reconstruction metric
  and remains FAIL for generative models; it is not changed or hidden.**

## 6. Repair-cache optimization (implemented)

- `src/core/ImageAlpha.h::clearRgbWhereTransparent()` clears RGB only where
  alpha == 0, keeping straight alpha and the existing SRC_ALPHA blend; it does
  **not** premultiply. `InpaintPipeline` applies it to every patch, and the
  engine applies it again after the stabilizer/second colour match (which can
  write RGB back under alpha 0) immediately before the cache write.
- The engine then trims each patch to its alpha>0 bounding box and rebases the
  crop, so `RepairFrameSource`/the compositor keep an exact mapping.
- Effect: the smooth cache fell from 11,848,664 B (pre-trim `e480696`) to
  **11,705,523 B** (1.2%); the all-transparent margin already compressed to
  almost nothing, so trimming is only a small win. The remaining bytes are the
  alpha>0 fill, which is noisy for this model. No premultiplication is used.
- Cache-format test (representative feathered patch, 192x192 crop, 96x96 hole):
  alpha survives exactly, transparent pixels read back (0,0,0,0), stored PNG
  **33,192 B vs 130,080 B** full-RGB (25.5%).
- Legacy cache: 110,131,993 B in the first (`ab80f7a`) run → 31,174,721 B now
  (1.73x source, PASS; the size rose versus `e480696` because the corrected
  keyframe prompt changed the refined tracking masks/fill, not because of the
  trim). Soft/smooth cache ratios FAIL because their sources compress to
  ~0.9 MB while the lossless alpha>0 patches are ~40 KB/frame; this is a
  source-size/format caveat, measured rather than asserted.

## 7. Model / acceptance-gate measurements (real models, technical env)

Targeted run: **31/31 passed** (AiEngineEndToEnd 8, TrackingPipeline 9 + 2 new,
InpaintModelContract 4 + 1 new, RemovalEval 2, RemovalRobustness 4,
FlowPropagator 1 new).

| Measurement | Value | Verdict |
| --- | --- | --- |
| EfficientTAM fp32 keyframe segmentation | 66–72 ms | PASS (≤100 ms) |
| EfficientTAM keyframe IoU, `static_logo_smooth` frame 0 (box prompt) | **0.9447** (was 0.674 with the 3-point prompt) | **PASS** (≥0.85) |
| DIS 1080p, 12 warm pairs, `PRESET_FAST` | p50 **37.54 ms = 26.64 fps**, p95 37.95 ms (range 26.6–28.3 fps) | **PASS** (≥20 fps) |
| DIS K=5 propagation, `slow_target` 150 frames | mean IoU **0.9604**, min 0.8114 (was 0.9687 with MEDIUM) | **PASS** (≥0.90) |
| MI-GAN 128x128 DML, cold / warm p50 (3+5 runs) | **508 ms / 442.1 ms = 2.26 fps** | **PASS** on warm (≥2 fps); cold 1.97 fps |
| MI-GAN 130x98 padded DML | 508 ms | PASS (pipeline test) |
| LaMa 512 int8 CPU | 594 ms = 1.68 fps | PASS (≥1.5 fps) |
| MI-GAN forced CPU one frame (`allowDml=false`) | **694 ms**, provider `cpu` | PASS |
| Engine repair E2E (4 frames, MI-GAN) | 2931 ms | informational |
| TrackingPipeline 12-frame model run | 161 ms | informational |
| Fixture 30-frame model pipeline | 504 ms | informational |
| DIS-only 12-frame pipeline | 15 ms | informational |
| Cache-format patch size | 33,192 B vs 130,080 B | PASS |
| VRAM peak | not measured in this task (see §9) | **SKIP** |

## 8. Threshold table

| # | Threshold | Target | Measured on this machine | Verdict |
| --- | --- | --- | --- | --- |
| 1 | 1080p30 10 s job time | ≤300 s (target 156–186 s) | 164.1 s soft / 164.0 s smooth / 205.1 s legacy (excludes probe/mask/key) | **PASS** (soft/smooth inside target; legacy above target) |
| 2 | EfficientTAM fp32 keyframe segmentation | ≤100 ms | 66–72 ms | **PASS** |
| 3 | DIS propagation at 1080p | ≥20 fps | p50 37.54 ms = 26.64 fps | **PASS** |
| 4 | MI-GAN DML | ≥2 fps (~500 ms/tile) | warm p50 442.1 ms = 2.26 fps; cold 508 ms = 1.97 fps | **PASS** (warm) |
| 5 | LaMa int8 CPU | ≥1.5 fps | 594 ms = 1.68 fps | **PASS** |
| 6 | VRAM peak | ≤4 GB | not measured reliably in this task | **SKIP** |
| 7 | CPU fallback one frame | completes | MI-GAN `allowDml=false` 694 ms on `cpu` | **PASS** |
| 8 | Mask keyframe IoU | ≥0.85 | **0.9447** | **PASS** |
| 9 | Propagated mean IoU at K=5 | ≥0.90 | **0.9604** | **PASS** |
| 10 | Hole PSNR, soft GT | ≥28 dB | **8.71 dB** (mean fill 50.43) | **FAIL** |
| 11 | Hole PSNR, smooth GT | ≥28 dB | **13.53 dB** (mean fill 33.45) | **FAIL** |
| 12 | Hole PSNR, legacy GT | ≥28 dB | **8.04 dB** (smooth cap 22.85) | **FAIL** (pathological GT) |
| 13 | Repair cache, soft 1080p | ≤2x source | **14.65x** | **FAIL** |
| 14 | Repair cache, smooth 1080p | ≤2x source | **12.72x** | **FAIL** |
| 15 | Repair cache, legacy 1080p | ≤2x source | **1.73x** | **PASS** |
| 16 | Export only from complete hash-verified cache, no inference | required | selftest reaches Complete+verify; export tests pass | **PASS** |
| 17 | Licensing | `TECHNICAL_EVALUATION_ONLY`, `commercialApproved=false`, `releaseBlocked=true`, no legal sign-off | manifest unchanged | **BLOCKED by design** |
| 18 | Subjective review | 3 reviewers, 1–5 scores | **not performed** | **NOT PERFORMED** |

## 9. Robustness results

`tests/ai/RemovalRobustnessTest.cpp`, fake transports, all **PASS**. These
tests cover **error mapping only**; the fake transport never touches the cache.
Chunk retention is covered by the `RemovalCache` partial-manifest tests and the
`RemovalController` redo tests, not by these cases.

| Case | Result |
| --- | --- |
| Engine crash (`CrashExit`) reports `crash` failure | PASS — error mapping |
| `disk_full` error reports without overwriting state | PASS — error mapping |
| Model SHA-256 mismatch disables the feature gate | PASS — `校验失败` reason |
| Chinese project path **mask-sidecar** round trip | PASS — mask-store round trip only |

Not exercised: real process kill, actual disk exhaustion, GPU driver reset, and
full project save-as relocation with an in-flight job.

## 10. Full regression (final tree)

| Command | Result |
| --- | --- |
| `ctest --preset windows-x64-release` (no model env) | **644/644 passed, 0 failed, 20 skipped**, 82.20 s |
| with `VIDEOEDITOR_M4_MODELS` + `VIDEOEDITOR_M4_TECHNICAL` | **644/644 passed, 0 failed, 1 skipped**, 146.26 s |

With the corrected box prompt and DIS optimization both acceptance-gate tests
now pass; the only remaining suite skips are the environment-gated model cases
(and `MediaProbe.ProbesOptionalVc1Sample`).

## 11. Known limitations

- **Model quality is the dominant remaining failure.** MI-GAN's exact hole
  reconstruction is far below 28 dB even on fair soft/smooth GTs, and lower
  than an independent Python reference on the soft hole; the ONNX mirror's
  generative quality and the engine's in-hole preprocessing need M5 work.
- **Hole PSNR is an exact-reconstruction metric.** Generative models fill
  plausible texture, not the exact low-frequency background; the gate remains
  FAIL rather than being reinterpreted. A subjective 3-reviewer score was not
  performed.
- **Small-source cache ratios fail.** Lossless alpha>0 patches are ~40 KB/frame
  while the soft/smooth source is ~0.9 MB; 2x cannot be met without a lossy or
  hole-only payload. The legacy 18 MB source passes at 1.73x.
- Fast motion, large occlusion, reflections and moving-camera content were not
  scored with the real model; only the static logo was run end-to-end.
- One object / one removal layer per clip; 1080p SDR only; 24/60 fps and VFR
  inputs are outside this evaluation.
- Per-layer duration and the fixed 30-frame repair chunk model were not varied.
- MI-GAN DirectML shows run-to-run variation (legacy PSNR 8.04–10.29 dB).
- **VRAM was not measured reliably in this task.** No discrete GPU; the iGPU
  reports shared-memory counters. The Task 0 DXGI spike (3597.8 MB peak) is
  historical and is not re-measured, so the row is **SKIP**, not PASS.
- **Licensing: release blocked.** `commercialApproved=false`,
  `releaseBlocked=true`, `legalSignOff` empty; dataset terms and ONNX mirror
  provenance are unresolved.
- **The 3-reviewer subjective review was not performed.**

## 12. Conclusion

**Technical feasibility.** The M4 pipeline runs end-to-end on this machine:
real Fast/MI-GAN jobs tracked and repaired all 300 frames of a 1080p30 10 s
clip, wrote complete hash-verified caches, and met job time (164–205 s), DIS
1080p throughput (26.6 fps), keyframe IoU (0.945), K=5 propagation IoU (0.960),
MI-GAN warm throughput (2.26 fps), LaMa CPU (1.68 fps), CPU fallback, cache
correctness and the robustness mapping tests. The legacy cache is 1.73x source.

**Failed gates.** Hole PSNR on all three fair/unfair GTs (8.71 / 13.53 /
8.04 dB vs 28 dB) and the soft/smooth cache ratios (14.65x / 12.72x vs 2x).
The PSNR failure is a model-quality/metric limitation, reported honestly; the
small-source cache failure is a format/source-size limitation. VRAM is SKIP.

**Commercial readiness.** Not established. This is internal technical evidence
only. The feature remains `TECHNICAL_EVALUATION_ONLY`, `commercialApproved=false`
and `releaseBlocked=true`; there is no legal sign-off, no three-reviewer
subjective review, and no discrete-GPU or multi-hardware validation. No
commercial approval is claimed or implied.

## Appendix — reproduction

```bash
# Fixtures (test data only)
pwsh tests/fixtures/removal/generate_fixtures.ps1

# Build and full suites
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  ctest --preset windows-x64-release

# Real metrics: soft (default preference) then each forced row
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  VideoEditor.exe --selftest-removal
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  VIDEOEDITOR_M4_ACCEPTANCE_SET=smooth VideoEditor.exe --selftest-removal
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  VIDEOEDITOR_M4_ACCEPTANCE_SET=legacy VideoEditor.exe --selftest-removal

# Reproduce the PSNR baselines (swap in the smooth/legacy paths as needed)
C:/m4-spike/venv/Scripts/python.exe -I tools/m4_spike/hole_psnr_baseline.py \
  --video C:/m4-spike/acceptance/static_logo_soft_1080p_10s.mp4 \
  --mask  C:/m4-spike/acceptance/static_logo_soft_mask_1080p_10s.mp4 \
  --clean C:/m4-spike/acceptance/clean_soft_1080p_10s.mp4 --frame 0

# Real model measurement lines
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  ai_tests.exe --gtest_filter='InpaintModelContract.*:TrackingPipeline.*:AiEngineEndToEnd.*'
```

Related records: `docs/superpowers/research/2026-11-15-m4-dml-spike.md`,
`docs/superpowers/research/2026-11-15-m4-license-decision.md`.

## 11. 2026-11-18 GPU re-measurement (RTX 5060 Ti 16 GB)

The reference machine was upgraded from the AMD Radeon integrated GPU to an
**NVIDIA GeForce RTX 5060 Ti 16 GB** (driver 610.88, CUDA UMD 13.3). The DirectML
path in the C++ engine was re-enabled for tracking (EfficientTAM hybrid DML),
per-frame refinement was enabled when the tracker reports DML, and LaMa now
requests DirectML with CPU fallback. These rows supersede the VRAM SKIP and the
interactive-performance SKIP rows above; PSNR and small-source cache rows are
unchanged because they are model/format limits, not speed limits.

### 11.1 Component latency (ONNX Runtime 1.22 DirectML, warm p50)

| Component | CPU | DirectML | Note |
|---|---|---|---|
| MI-GAN 512 | 320 ms | **26 ms (38 fps)** | ~12× faster than the iGPU DML path |
| LaMa 512 int8 allowzero0 | 460 ms | **49 ms (20 fps)** | DML now initialises; CPU fallback retained |
| EfficientTAM image encoder | 35 ms | **4.8 ms** | |
| EfficientTAM prompt encoder | 0.4 ms | 0.9 ms | |
| EfficientTAM mask decoder | 4.2 ms | **1.2 ms** | keyframe segmentation ≈7 ms (≈145 fps) |
| SAM 2 vision encoder | 569 ms | **52 ms** | per-frame SAM 2 becomes viable, but the offline path uses EfficientTAM + DIS |
| SAM 2 mask decoder | 16 ms | **5.5 ms** | |

### 11.2 1080p30/10s selftest (soft corpus, DML)

```
REMOVAL_SELFTEST_FAIL threshold set=soft-1080p
  psnr=8.71 psnrThreshold=28.00 seconds=43.4 timingThreshold=300
  cacheBytes=13227347 cacheBudget=1805744 sourceBytes=902872
  completedFrames=300 frames=300 provider=tracker=dml;inpaint=dml
```

| Metric | iGPU (2026-11-15) | RTX 5060 Ti (2026-11-18) | Verdict |
|---|---|---|---|
| Job time (1080p30 10 s) | 164.1 s | **43.4 s** | PASS (target 156–186 s not met; ≤300 s met) |
| Provider | tracker=cpu;inpaint=dml | **tracker=dml;inpaint=dml** | — |
| Peak VRAM (nvidia-smi sampling, 0.2 s) | not measured | **1,533 MiB** | PASS (≤4 GB) |
| DIS 1080p | 26.6 fps | 26.6 fps (CPU DIS) | PASS |
| MI-GAN warm | 2.26 fps | **38 fps** | PASS (≥10 fps interactive gate) |
| LaMa int8 DML | not run (CPU 1.68 fps) | **20 fps** | PASS (≥2 fps gate) |
| Keyframe segmentation | 66–72 ms | **≈7 ms** | PASS (≤100 ms) |
| Hole PSNR (soft GT) | 8.71 dB | **8.71 dB** | **FAIL** (28 dB), unchanged model quality |
| Cache/source (soft) | 14.65× | **14.65×** | **FAIL** (2×), unchanged format/source-size limit |

VRAM methodology: the selftest ran while `nvidia-smi
--query-gpu=memory.used --format=csv,noheader,nounits` was sampled every 0.2 s;
the maximum observed was 1,533 MiB (baseline desktop usage ≈780 MiB excluded by
taking the peak during the job). This is an external measurement, not an
in-process counter; a product-grade VRAM sampler is still M5 work.

### 11.3 What the upgrade changed

- **Interactive tracking is now real:** per-frame EfficientTAM refinement
  (K=1) plus DIS runs at roughly 24 fps at 1080p (35 ms DIS + 7 ms model), so
  the original "tracking ≥5 fps / EfficientTAM ≥8 fps" gates are satisfied with
  margin. The offline pipeline keeps K=5 on CPU-only machines.
- **Interactive repair is viable:** MI-GAN at 38 fps and LaMa at 20 fps exceed
  the original ≥10/≥2 fps gates. The UI still generates an offline cache rather
  than running inference inside the preview compositor; exposing a live preview
  repair mode is now an M5 UX decision, not a hardware blocker.
- **VRAM is no longer a SKIP:** 1.5 GB peak on a 16 GB card is well inside the
  4 GB gate.
- **Quality did not improve:** PSNR and the small-source cache ratio are
  independent of GPU speed. The generative fill remains the limiting factor;
  licensing remains release-blocked; the 3-reviewer subjective review remains
  not performed.

### 11.4 Reproduce

```bash
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" --build --preset windows-x64-release --target VideoEditor
cd build/windows-x64-release/Release
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
VIDEOEDITOR_M4_ACCEPTANCE_SET=soft QT_ASSUME_STDERR_HAS_CONSOLE=1 \
  ./VideoEditor.exe --selftest-removal
# separately sample VRAM during the run:
nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -l 0.2
```
