# M4 AI removal acceptance — technical evaluation

- Date: 2026-11-16
- Branch: `feature/m4-ai-removal`
- Base commit: `6b9eeb3` (Task 9 report and tests are added by the accompanying
  commit on this branch)
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

Test environment for the real runs:

```
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models
VIDEOEDITOR_M4_TECHNICAL=1
```

## 2. Fixture matrix

| Fixture | Geometry | Location | Used by |
| --- | --- | --- | --- |
| `static_logo.mp4` | 640x360, 30 fps, 5 s, mpeg4 | `tests/fixtures/removal/` (committed) | fallback selftest, geometry test |
| `static_logo_mask.mp4` | 640x360, 30 fps, 5 s | committed | fallback mask |
| `clean_background.mp4` | 640x360, 30 fps, 5 s | committed | fallback PSNR GT |
| `slow_target.mp4` | 640x360, 30 fps, 5 s | committed | corpus (not run through the model) |
| `slow_target_mask.mp4` | 640x360, 30 fps, 5 s | committed | corpus |
| `occlusion.mp4` | 640x360, 30 fps, 5 s | committed | corpus |
| `static_logo_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | `C:/m4-spike/acceptance/` (not committed) | real selftest |
| `static_logo_mask_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | outside repo | real selftest mask |
| `clean_background_1080p_10s.mp4` | 1920x1080, 30 fps, 10 s | outside repo | real selftest PSNR GT |

`tests/fixtures/removal/generate_fixtures.ps1` regenerates the committed corpus
with the FFmpeg CLI; `README.md` documents that these are test data only and
never enter an installer.

Coverage gaps in the matrix: only 30 fps was exercised; 24/60 fps, 4 GB/6 GB
discrete VRAM variants and a full CPU-only run were **not** performed. The
Chinese-path project round-trip is covered by a unit test, not by a full model
run.

## 3. Method — `VideoEditor.exe --selftest-removal`

1. Resolve the 1080p acceptance clip when `C:/m4-spike/acceptance/` has the
   video, mask and clean GT; otherwise fall back to the committed 640x360
   corpus.
2. `MediaProbe` the clip, build a `Project` with one video track/clip and a
   `RemovalLayer` (`sourceIn=0`, `sourceOut=duration`, Fast/MI-GAN, one mask
   keyframe at source time 0).
3. Decode mask frame 0 with `FrameReader`, threshold luma > 127 into a
   `BinaryMask`, write it through `MaskSidecarStore` to a temporary project
   sidecar root and set `clip.removalLayerId`.
4. Derive the cache key with `RemovalPreflight::keyInputFor()` +
   `computeRemovalCacheKey()` so it is exactly the key export preflight checks.
5. Start an `AiJobSpec` (Fast/MI-GAN, display size, asset fps, full source
   range, technical env/models) and wait on a `QEventLoop` with a 600 s timeout.
6. Require `RemovalCache::state()==Complete` and `verify()`, open the cache via
   `RepairFrameSource`, and compute hole PSNR: every patch pixel with alpha > 0
   in the patch crop is compared with the clean-GT frame at the same global
   frame/crop.
7. Measure wall time, cache bytes and source bytes, then print one line and
   exit 0/4.

The run uses the real inference subprocess (`VisionAiEngine`) with
EfficientTAM-fp32 CPU tracking and MI-GAN DirectML repair. A pass is never
fabricated: without a verified model bundle the command prints
`REMOVAL_SELFTEST_SKIP` (exit 4) instead.

## 4. Measured results

### 4.1 Real 1080p selftest (threshold failure, recorded as-is)

Command:

```
VideoEditor.exe --selftest-removal
```

Output (exit code **4**):

```
REMOVAL_SELFTEST_FAIL threshold file=C:/m4-spike/acceptance/static_logo_1080p_10s.mp4
  psnr=10.29 psnrThreshold=28.00 seconds=218.8 timingThreshold=300
  cacheBytes=110131993 cacheBudget=36015140 sourceBytes=18007570 frames=300
  provider=tracker=cpu;inpaint=dml
```

| Metric | Value |
| --- | --- |
| Decoded/processed frames | 300 (1080p30, 10 s) |
| End-to-end wall time | **218.8 s** (3 min 39 s) |
| Hole PSNR vs clean GT | **10.29 dB** |
| Cache bytes | **110,131,993 B** (~105 MiB) |
| Source bytes | 18,007,570 B |
| Cache / source | **6.12x** |
| Provider | `tracker=cpu;inpaint=dml` |
| Cache state / hash verification | Complete and verified (the run reached PSNR) |

The job itself completed and committed a hash-verified cache of all 300 frames.
The failure is on the hole-PSNR and cache-size thresholds, not on generation
completeness.

### 4.2 Why hole PSNR is 10.29 dB

The static-logo mask at 1080p covers x=1560..1799, y=90..239 (240x150 =
36,000 px). Analysis of the acceptance fixtures:

- Outside the mask, `static_logo_1080p_10s` and `clean_background_1080p_10s`
  are pixel-aligned (PSNR 70.3 dB), so the low score is **not** a temporal or
  geometry offset.
- The clean background inside that ROI is high-frequency `testsrc2` texture:
  per-channel standard deviation is ~95 in G/B.
- Filling the hole with the ROI mean gives **10.35 dB**; an ideal smooth fill
  (51 px Gaussian blur of the true GT) tops out at **22.9 dB**.
- Measured MI-GAN output is **10.29 dB**, i.e. effectively a smooth plausible
  fill. MI-GAN cannot recover the exact phase of the synthetic high-frequency
  pattern, so the 28 dB hole-PSNR gate is **not attainable on this ROI with any
  smooth inpaint model**. The fixture is pathological for this metric; the
  number should not be read as "MI-GAN is broken", and it also must not be read
  as a pass.

Likely causes on a real clip would additionally include compressed-GT error,
ROI margin, and model quality on large holes.

### 4.3 Why the cache is 6.12x the source

- Mask bbox 240x150; `InpaintPipeline` expands it by
  `max(64, w/4, h/4) = 64` px per side and rounds to 8 px, producing a
  **368x280** crop per frame.
- Every one of the 300 frames stores an RGBA PNG of the full crop; the
  transparent (alpha=0) region still carries MI-GAN's RGB texture, which PNG
  cannot compress well. Measured average is ~367 KB/frame.
- The 2x budget is 36,015,140 B; the actual cache is 110,131,993 B.
- Future optimization: zero or omit RGB outside the feathered hole (store only
  the hole-bounded patch), or premultiply alpha so transparent pixels collapse
  to one colour. This is an M5 cache-format change, not a threshold edit.

### 4.4 Model / unit measurements (real models, technical env)

Targeted real-model run: **27/27 passed** (AiEngineEndToEnd 8, TrackingPipeline
9, InpaintModelContract 4, RemovalEval 2, RemovalRobustness 4).

| Measurement | Value | Test |
| --- | --- | --- |
| EfficientTAM fp32 keyframe segmentation | **68 ms** (320x180 synthetic) | `TrackingPipeline.EfficientTamSegmentsSyntheticRectangle` |
| TrackingPipeline 12-frame model run | 185 ms | `TrackingPipeline.RunsWithKeyframesAndWritesArtifacts` |
| Fixture 30-frame model pipeline | 607 ms | `TrackingPipeline.RunsOnFixtureVideoWithModel` |
| DIS-only 12-frame pipeline | 25 ms | `TrackingPipeline.RunsWithoutModelBetweenKeyframes` |
| MI-GAN 128x128 repair (DML) | **509 ms = 1.96 fps** | `InpaintModelContract.MiGanRepairsSyntheticCrop` |
| MI-GAN padded 130x98 repair (DML) | 503 ms | `InpaintModelContract.MiGanPadsNonMultipleOfEightCrop` |
| LaMa tiled 640x360 (CPU) | 1191 ms | `InpaintModelContract.LamaTilesCropLargerThan512` |
| LaMa 512x512 (CPU int8) | **584 ms = 1.71 fps** | `InpaintModelContract.LamaRepairsSyntheticCrop` |
| Engine repair E2E (4 frames, MI-GAN) | 2997 ms | `AiEngineEndToEnd.RepairJobWritesChunkAndManifest` |

## 5. Threshold table

| # | Threshold | Target | Measured on this machine | Verdict |
| --- | --- | --- | --- | --- |
| 1 | 1080p30 10 s end-to-end | ≤300 s (target 156–186 s) | **218.8 s** | **PASS** (above target) |
| 2 | EfficientTAM fp32 keyframe segmentation | ≤100 ms | 68 ms (320x180 synthetic; no 1080p isolation) | **PASS** (resolution caveat) |
| 3 | DIS propagation | ≥20 fps at 1080p | not isolated; the whole tracking stage for 300 frames including setup completed within ~35 s (upper-bound average ≥8.6 fps) | **SKIP** (not directly measured) |
| 4 | MI-GAN DML | ≥2 fps (~500 ms/tile) | **509 ms/tile = 1.96 fps** | **FAIL** (marginal, ~2% over the 500 ms gate) |
| 5 | LaMa int8 CPU | ≥1.5 fps | 584 ms/tile = 1.71 fps | **PASS** |
| 6 | VRAM peak | ≤4 GB | 3597.8 MB (Task 0 spike; not re-measured in Task 9) | **PASS** (Task 0, iGPU shared-memory caveat) |
| 7 | CPU fallback one frame | completes | MI-GAN CPU 685 ms (Task 0); LaMa CPU 584 ms now | **PASS** |
| 8 | Mask keyframe IoU vs reference | ≥0.85 | not measured against the fixture reference | **SKIP** |
| 9 | Propagated mean IoU at K=5 | ≥0.90 | not measured | **SKIP** |
| 10 | Hole PSNR | ≥28 dB | **10.29 dB** | **FAIL** |
| 11 | Repair cache size | ≤2x source | **6.12x** (110,131,993 / 18,007,570 B) | **FAIL** |
| 12 | Export only from complete, hash-verified cache; no inference during export | required | selftest reached Complete+verify; preflight/export tests pass | **PASS** |
| 13 | Licensing | `TECHNICAL_EVALUATION_ONLY`, `commercialApproved=false`, `releaseBlocked=true`, no legal sign-off | manifest unchanged | **BLOCKED by design** |
| 14 | Subjective review | 3 reviewers, 1–5 scores | **not performed** | **NOT PERFORMED** |

## 6. Robustness results

`tests/ai/RemovalRobustnessTest.cpp` (fake transports; no real process kill or
disk fill), all **PASS**:

| Case | Result |
| --- | --- |
| Engine crash (`CrashExit`) marks the job failed and keeps completed chunks | PASS — `errorCode=crash`, message states completed segments are retained |
| `disk_full` error stops the job without losing completed chunks | PASS — `errorCode=disk_full` |
| Model SHA-256 mismatch disables the feature gate | PASS — `featureEnabled()` false, reason contains 校验失败 |
| Chinese project path mask round-trip | PASS — bytes identical through `MaskSidecarStore` |

Not exercised by an automated test: real process kill, actual disk exhaustion,
GPU driver reset/device loss, and save-as relocation with an in-flight job.

## 7. Full regression

| Command | Result |
| --- | --- |
| `ctest --preset windows-x64-release` (no model env) | **639/639 passed, 0 failed, 18 skipped**, 86.23 s |
| `ctest --preset windows-x64-release` with `VIDEOEDITOR_M4_MODELS` + `VIDEOEDITOR_M4_TECHNICAL` | **639/639 passed, 0 failed, 1 skipped**, 139.86 s |

Without the env the skips are the model/GPU cases: `MediaProbe.ProbesOptionalVc1Sample`,
`RemovalTechnicalE2E.*`, 8 `AiEngineEndToEnd.*`, 3 model `TrackingPipeline.*`,
`OnnxExecutor.RejectsWhenNoExecutionProviderAllowed`, 4
`InpaintModelContract.*`. With the technical env only
`MediaProbe.ProbesOptionalVc1Sample` remains skipped; every real-model test
runs and passes.

## 8. Known limitations

- **Fast motion, large occlusion, reflections, and reflections over moving
  content were not evaluated with the real model.** Only the static-logo clip
  was run end-to-end; the slow-target and occlusion fixtures prove decode
  geometry but were not scored.
- One object / one removal layer per clip.
- 1080p SDR only; 24/60 fps and VFR inputs are not part of this evaluation
  (VFR is rejected by the surrounding pipeline).
- Per-layer duration and the fixed 30-frame repair chunk model were not varied.
- Hole PSNR is measured against a compressed synthetic GT and is not
  representative of natural footage; on this ROI even an ideal smooth fill caps
  at 22.9 dB.
- Cache format stores the whole crop RGBA and exceeds the 2x budget on a small
  static logo (6.12x).
- MI-GAN DML is at the 2 fps boundary (1.96 fps measured here).
- DIS throughput at 1080p and both IoU gates were not isolated/measured.
- No discrete AMD or NVIDIA GPU was available; no 4 GB vs 6 GB VRAM comparison;
  no full CPU-only end-to-end run.
- **Licensing: release blocked.** `commercialApproved=false`, `releaseBlocked=true`,
  `legalSignOff` empty. Dataset terms (SA-1B, Places2, YouTube-VOS, etc.) are
  unresolved and the ONNX mirror provenance still needs legal review.
- **The 3-reviewer subjective review was not performed.** No subjective quality
  score exists.

## 9. Conclusion

**Technical feasibility.** The M4 pipeline works end-to-end on this machine:
a real Fast/MI-GAN job tracked and repaired all 300 frames of a 1080p30 10 s
clip, committed a complete hash-verified cache, and the selftest measured it
deterministically. End-to-end time (218.8 s) is inside the 300 s offline
budget, and LaMa int8 CPU, CPU fallback, export-from-verified-cache and the
robustness cases pass. Selected thresholds nevertheless **fail**: hole PSNR
(10.29 dB vs 28 dB), cache budget (6.12x vs 2x) and MI-GAN throughput
(1.96 fps vs 2 fps); DIS-at-1080p and both IoU gates are not measured.

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

# Real metrics
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  VideoEditor.exe --selftest-removal

# Real model measurement lines
VIDEOEDITOR_M4_MODELS=C:/m4-spike/models VIDEOEDITOR_M4_TECHNICAL=1 \
  ai_tests.exe --gtest_filter='InpaintModelContract.*:TrackingPipeline.*:AiEngineEndToEnd.*'
```

Related records: `docs/superpowers/research/2026-11-15-m4-dml-spike.md`,
`docs/superpowers/research/2026-11-15-m4-license-decision.md`.
