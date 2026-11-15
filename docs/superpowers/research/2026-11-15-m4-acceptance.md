# M4 AI removal acceptance — technical evaluation

- Date: 2026-11-16
- Branch: `feature/m4-ai-removal`
- Base commit: `6b9eeb3`; the Task 9 fixes are added by the accompanying commit
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
| `static_logo.mp4` / `_mask` / `clean_background.mp4` | 640x360, 30 fps, 5 s | committed | legacy static fallback + keyframe IoU |
| `slow_target.mp4` / `slow_target_mask.mp4` | 640x360, 30 fps, 5 s | committed | K=5 DIS propagation IoU |
| `occlusion.mp4` | 640x360, 30 fps, 5 s | committed | decode corpus (not model-scored) |
| `static_logo_smooth.mp4` / `_mask` / `clean_gradient.mp4` | 640x360, 30 fps, 5 s | committed | smooth fallback (fair GT) |
| `static_logo_1080p_10s.mp4` / `_mask` / `clean_background_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | `C:/m4-spike/acceptance/` | legacy/high-frequency acceptance |
| `static_logo_smooth_1080p_10s.mp4` / `_mask` / `clean_gradient_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | `C:/m4-spike/acceptance/` | smooth acceptance (fair GT) |

Coverage gaps: only 30 fps was exercised; 24/60 fps, 4 GB/6 GB discrete VRAM
variants and a full CPU-only end-to-end run were **not** performed. The
Chinese-path project round-trip is a mask-store unit test, not a full model run.

Smooth-set note: the initially supplied smooth plate and its clean GT were
generated with different random `gradients` geometry (outside-hole source-vs-GT
PSNR was 7.02 dB, so it was not a valid GT). The two 1080p smooth videos were
regenerated with pinned endpoints and seed
(`gradients=...:seed=12345:x0=0:y0=0:x1=1920:y1=1080`), giving an outside-hole
PSNR of 66.25 dB. The committed 640x360 smooth set uses the same pinned seed.

## 3. Method — `VideoEditor.exe --selftest-removal`

1. Select a fixture set. Default preference: smooth 1080p → legacy 1080p →
   repo smooth 640x360 → repo legacy 640x360. `VIDEOEDITOR_M4_ACCEPTANCE_SET`
   forces `smooth`, `legacy` or `repo` so both acceptance rows are
   reproducible. The selected set is printed
   (`REMOVAL_SELFTEST_FIXTURE set=...`) and echoed in the result line.
2. Probe the clip, build a `Project` with one video track/clip and a
   `RemovalLayer` (`sourceIn=0`, `sourceOut=duration`, Fast/MI-GAN, one mask
   keyframe at 0).
3. Decode mask frame 0, threshold luma > 127, write it through
   `MaskSidecarStore`, and set `clip.removalLayerId`.
4. Derive the cache key with `RemovalPreflight::keyInputFor()` +
   `computeRemovalCacheKey()` so it matches export preflight exactly.
5. Run the real `AiJobSpec` on a `QEventLoop` with a 600 s timeout.
6. Require `state()==Complete`, `verify()`, and
   `result.completedFrames == manifest.totalFrames`. Then require
   `framesWithPatch == totalFrames`: every frame of this static-mask clip must
   contribute at least one alpha>0 repair pixel. Clean frames must also decode
   at the exact display size before any pixel comparison.
7. Compute hole PSNR: every patch pixel with alpha > 0 in the patch crop is
   compared with the clean-GT frame at the same global frame/crop. The timer
   starts immediately before the job and excludes probe/mask/key setup, so
   `seconds=` is **job time**, not full end-to-end wall time.
8. Print one line and exit 0/4. Without a verified model bundle the command
   prints `REMOVAL_SELFTEST_SKIP` and exits 4.

The engine zeroes RGB under alpha == 0 after the final colour match and before
the cache write; alpha is kept straight and the renderer still uses SRC_ALPHA.

## 4. Selftest results (real 1080p10s runs, exit 4)

### 4.1 Smooth acceptance set (fair GT; preferred)

```
REMOVAL_SELFTEST_FAIL threshold set=smooth-1080p
  file=C:/m4-spike/acceptance/static_logo_smooth_1080p_10s.mp4
  psnr=11.15 psnrThreshold=28.00 seconds=172.7 timingThreshold=300
  cacheBytes=11848664 cacheBudget=1840644 sourceBytes=920322
  completedFrames=300 frames=300 provider=tracker=cpu;inpaint=dml
```

### 4.2 Legacy high-frequency acceptance set

```
REMOVAL_SELFTEST_FAIL threshold set=legacy-1080p
  file=C:/m4-spike/acceptance/static_logo_1080p_10s.mp4
  psnr=8.31 psnrThreshold=28.00 seconds=211.6 timingThreshold=300
  cacheBytes=19582160 cacheBudget=36015140 sourceBytes=18007570
  completedFrames=300 frames=300 provider=tracker=cpu;inpaint=dml
```

| Row | PSNR | Job time | Cache / source | Cache verdict | PSNR verdict |
| --- | --- | --- | --- | --- | --- |
| Smooth 1080p10s | **11.15 dB** | **172.7 s** | 11,848,664 / 920,322 = **12.87x** | **FAIL** (12.87x > 2x) | **FAIL** |
| Legacy 1080p10s | **8.31 dB** | **211.6 s** | 19,582,160 / 18,007,570 = **1.09x** | **PASS** (≤2x) | **FAIL** |

Both jobs completed all 300 frames with a complete, hash-verified cache. Job
time is inside the 300 s offline budget but above the 2.6–3.1 min target.

## 5. Why hole PSNR fails (reproducible evidence)

Analysis script:
`tools/m4_spike/hole_psnr_baseline.py` (numpy + opencv).

```
C:/m4-spike/venv/Scripts/python.exe -I tools/m4_spike/hole_psnr_baseline.py \
  --video C:/m4-spike/acceptance/static_logo_1080p_10s.mp4 \
  --mask  C:/m4-spike/acceptance/static_logo_mask_1080p_10s.mp4 \
  --clean C:/m4-spike/acceptance/clean_background_1080p_10s.mp4 --frame 0
```

For the smooth set, replace the three paths with the `*_smooth_*` /
`clean_gradient_*` files.

Frame 0 baselines (mask bbox x1560..1799, y90..239, 36,000 px on both sets):

| Baseline | Legacy high-frequency | Smooth gradient |
| --- | --- | --- |
| Source vs clean **outside** the hole (frame 0) | 70.33 dB | 66.25 dB |
| Mean-colour fill inside the hole | **10.35 dB** | **33.45 dB** |
| Ideal smooth fill (51 px Gaussian blur of GT) | **22.85 dB** | **51.08 dB** |
| Hole BGR mean / std | B 254.7/0.7, G 212.5/95.0, R 42.3/94.7 | B 33.0/4.6, G 138.6/3.6, R 204.9/7.4 |

- Legacy: the GT is high-frequency synthetic `testsrc2`; a smooth fill can
  never reproduce the pattern phase, so 28 dB is unattainable (cap 22.85 dB).
  This fixture is **pathological** and is not a model-quality signal.
- Smooth: the GT is fair (mean fill 33.45 dB, smooth cap 51.08 dB), yet MI-GAN
  measures 11.15 dB. Preserving the known region is exact (outside-hole mean
  channel difference 0.033 on the contract crop; 18.2 with R/B swapped), so
  the cache, channel order and preprocessing are correct. The failure is the
  **generative fill itself**: on the 640x360 smooth fixture the patch's visible
  mean RGB is (255,244,56) while the GT is (243,157,11); on the 128x128 smooth
  contract crop the hole centre is (57,97,144) versus an expected (120,140,120)
  (zeroing the hole content before inference gives 38,49,62, i.e. worse).
- Legacy PSNR also varied 8.31–10.29 dB between runs, indicating DirectML
  inference nondeterminism in addition to model quality.

Conclusion: PSNR **FAIL** is real in both rows; the smooth row isolates it as a
model-quality limitation rather than a metric artifact.

## 6. Repair-cache optimization (implemented)

- `src/core/ImageAlpha.h::clearRgbWhereTransparent()` clears RGB only where
  alpha == 0, keeping straight alpha and the existing SRC_ALPHA blend; it does
  **not** premultiply. `InpaintPipeline` applies it to every patch, and the
  engine applies it again after the stabilizer/second colour match (which can
  write RGB back under alpha 0) immediately before `writeChunkFrames`.
- Cache-format test writes a representative feathered patch (96x96 textured
  hole, 4 px feather, 192x192 crop) through `RemovalCache`:
  alpha survives exactly, transparent pixels read back as (0,0,0,0), and the
  stored PNG is **33,192 B vs 130,080 B** for the old full-RGB crop (25.5%).
- Measured effect on the real acceptance run: legacy cache fell from
  110,131,993 B (6.12x) to **19,582,160 B (1.09x)** — a 5.6x reduction and a
  **PASS** on the 2x budget. The smooth cache is 11,848,664 B (12.87x) because
  the source itself is tiny (920 KB) and the model's noisy fill does not
  compress; that row is a measured **FAIL**, not an assertion.

## 7. Model / acceptance-gate measurements (real models, technical env)

Targeted run: 29 passed + 2 failed = 31 tests (AiEngineEndToEnd 8,
TrackingPipeline 9 + 2 new, InpaintModelContract 4 + 1 new, RemovalEval 2,
RemovalRobustness 4, FlowPropagator new test).

| Measurement | Value | Verdict |
| --- | --- | --- |
| EfficientTAM fp32 keyframe segmentation | 68–72 ms (320x180 and 640x360) | PASS (≤100 ms) |
| EfficientTAM keyframe IoU on `static_logo` frame 0 | **0.674** | **FAIL** (≥0.85) |
| DIS 1080p propagation, 12 warm pairs | p50 **58.87 ms = 16.99 fps**, p95 59.85 ms (earlier run 54.36 ms = 18.4 fps) | **FAIL** (≥20 fps) |
| DIS K=5 propagation, `slow_target` 150 frames | mean IoU **0.9687**, min 0.8007 | PASS (≥0.90) |
| MI-GAN 128x128 DML, cold / warm p50 (3+5 runs) | **512 ms / 443.9 ms = 2.25 fps** | PASS on warm (≥2 fps); cold 1.95 fps |
| MI-GAN 130x98 padded DML | 504 ms | PASS (pipeline test) |
| LaMa 512 int8 CPU | **594 ms = 1.68 fps** | PASS (≥1.5 fps) |
| MI-GAN forced CPU one frame (`allowDml=false`) | **696 ms**, provider `cpu` | PASS |
| Engine repair E2E (4 frames, MI-GAN) | 2999 ms | informational |
| TrackingPipeline 12-frame model run | 165 ms | informational |
| Fixture 30-frame model pipeline | 625 ms | informational |
| DIS-only 12-frame pipeline | 23 ms | informational |
| Cache-format patch size (zeroed vs full RGB) | 33,192 B vs 130,080 B | PASS |
| VRAM peak | not measured in this task (see §9) | **SKIP** |

## 8. Threshold table

| # | Threshold | Target | Measured on this machine | Verdict |
| --- | --- | --- | --- | --- |
| 1 | 1080p30 10 s job time | ≤300 s (target 156–186 s) | 211.6 s legacy / 172.7 s smooth (excludes probe/mask/key) | **PASS** (above target) |
| 2 | EfficientTAM fp32 keyframe segmentation | ≤100 ms | 68–72 ms | **PASS** |
| 3 | DIS propagation at 1080p | ≥20 fps | p50 58.87 ms = 16.99 fps (range 17.0–18.4) | **FAIL** |
| 4 | MI-GAN DML | ≥2 fps (~500 ms/tile) | warm p50 443.9 ms = 2.25 fps; cold 512 ms = 1.95 fps | **PASS** on warm p50 |
| 5 | LaMa int8 CPU | ≥1.5 fps | 594 ms = 1.68 fps | **PASS** |
| 6 | VRAM peak | ≤4 GB | not measured reliably in this task (Task 0 spike 3597.8 MB is historical) | **SKIP** |
| 7 | CPU fallback one frame | completes | MI-GAN `allowDml=false` 696 ms on `cpu` | **PASS** |
| 8 | Mask keyframe IoU | ≥0.85 | 0.674 | **FAIL** |
| 9 | Propagated mean IoU at K=5 | ≥0.90 | 0.9687 | **PASS** |
| 10 | Hole PSNR, smooth GT | ≥28 dB | 11.15 dB | **FAIL** |
| 11 | Hole PSNR, legacy GT | ≥28 dB | 8.31 dB (smooth-fill cap 22.85) | **FAIL** (pathological GT) |
| 12 | Repair cache, legacy 1080p | ≤2x source | 1.09x | **PASS** |
| 13 | Repair cache, smooth 1080p | ≤2x source | 12.87x | **FAIL** |
| 14 | Export only from complete hash-verified cache, no inference | required | selftest reached Complete+verify; export tests pass | **PASS** |
| 15 | Licensing | `TECHNICAL_EVALUATION_ONLY`, `commercialApproved=false`, `releaseBlocked=true`, no legal sign-off | manifest unchanged | **BLOCKED by design** |
| 16 | Subjective review | 3 reviewers, 1–5 scores | **not performed** | **NOT PERFORMED** |

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
| `ctest --preset windows-x64-release` (no model env) | **643/644 passed, 1 failed, 20 skipped**, 82.98 s |
| with `VIDEOEDITOR_M4_MODELS` + `VIDEOEDITOR_M4_TECHNICAL` | **642/644 passed, 2 failed, 1 skipped**, 151.41 s |

Failures are the acceptance gates that miss their thresholds:
`FlowPropagator.Sustains1080pDisThroughput` (both runs) and
`TrackingPipeline.EfficientTamMatchesAuthoredMaskOnFixture` (technical run).
The skips without the env are the model/GPU cases (the same set as before plus
the new EfficientTAM-IoU and CPU-MI-GAN model tests); with the technical env
only `MediaProbe.ProbesOptionalVc1Sample` remains skipped.

## 11. Known limitations

- **Model quality is the dominant failure.** On a fair smooth GT, MI-GAN's
  hole fill misses the local colour even though it preserves the known region
  exactly; the ONNX mirror's generative quality must be re-evaluated before any
  product claim.
- **DIS at 1080p is below the 20 fps gate** (17.0–18.4 fps p50).
- **EfficientTAM keyframe IoU on the fixture is 0.674**, below 0.85.
- Fast motion, large occlusion, reflections and moving-camera content were not
  scored with the real model; only the static logo was run end-to-end.
- One object / one removal layer per clip; 1080p SDR only; 24/60 fps and VFR
  inputs are outside this evaluation.
- Per-layer duration and the fixed 30-frame repair chunk model were not varied.
- The legacy testsrc2 plate is a pathological PSNR GT (cap 22.85 dB) and must
  not be used as a model-quality signal.
- The 2x cache budget is sensitive to source bitrate: the legacy 18 MB source
  passes (1.09x), while the 920 KB smooth source fails (12.87x) even after the
  zero-RGB fix, because the repair patches are lossless RGBA PNGs.
- MI-GAN DirectML shows run-to-run variation (legacy PSNR 8.31–10.29 dB).
- **VRAM was not measured reliably in this task.** No discrete GPU was
  available; the integrated Radeon reports shared-memory counters. The Task 0
  DXGI spike (3597.8 MB peak) is historical and is not re-measured here, so
  the row is **SKIP**, not PASS.
- **Licensing: release blocked.** `commercialApproved=false`,
  `releaseBlocked=true`, `legalSignOff` empty; dataset terms and ONNX mirror
  provenance are unresolved.
- **The 3-reviewer subjective review was not performed.**

## 12. Conclusion

**Technical feasibility.** The M4 pipeline runs end-to-end on this machine:
real Fast/MI-GAN jobs tracked and repaired all 300 frames of a 1080p30 10 s
clip, wrote complete hash-verified caches, and the selftest measured them
deterministically. Job time is inside the 300 s budget, LaMa CPU, MI-GAN CPU
fallback, MI-GAN warm throughput, K=5 propagation IoU, export-from-verified-
cache and the robustness mapping tests pass. The zero-RGB cache fix cut the
legacy cache from 6.12x to 1.09x of source.

**Failed gates.** DIS 1080p throughput (17.0–18.4 fps vs 20), EfficientTAM
keyframe IoU (0.674 vs 0.85), hole PSNR on both the pathological legacy GT
(8.31 dB, cap 22.85) and the fair smooth GT (11.15 dB vs a mean-fill baseline
of 33.45), and the smooth-asset cache budget (12.87x vs 2x). VRAM is SKIP.

**Commercial readiness.** Not established. This is internal technical evidence
only. The feature remains `TECHNICAL_EVALUATION_ONLY`, `commercialApproved=false`
and `releaseBlocked=true`; there is no legal sign-off, no three-reviewer
subjective review, and no discrete-GPU or multi-hardware validation. No
commercial approval is claimed or implied.

## Appendix — reproduction

```bash
# Fixtures (test data only)
pwsh tests/fixtures/removal/generate_fixtures.ps1

# Build and full suites (expect the two acceptance-gate failures)
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  ctest --preset windows-x64-release

# Real metrics (smooth preferred; force each row with the set variable)
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  VIDEOEDITOR_M4_ACCEPTANCE_SET=smooth VideoEditor.exe --selftest-removal
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  VIDEOEDITOR_M4_ACCEPTANCE_SET=legacy VideoEditor.exe --selftest-removal

# Reproduce the PSNR baselines
C:/m4-spike/venv/Scripts/python.exe -I tools/m4_spike/hole_psnr_baseline.py \
  --video C:/m4-spike/acceptance/static_logo_1080p_10s.mp4 \
  --mask  C:/m4-spike/acceptance/static_logo_mask_1080p_10s.mp4 \
  --clean C:/m4-spike/acceptance/clean_background_1080p_10s.mp4 --frame 0

# Real model measurement lines
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  ai_tests.exe --gtest_filter='InpaintModelContract.*:TrackingPipeline.*:AiEngineEndToEnd.*'
```

Related records: `docs/superpowers/research/2026-11-15-m4-dml-spike.md`,
`docs/superpowers/research/2026-11-15-m4-license-decision.md`.
