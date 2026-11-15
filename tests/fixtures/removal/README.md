# Removal evaluation fixtures

Small, deterministic test assets for the M4 AI-removal evaluation and
robustness tests. They are **test data only** and never ship in an installer.

Regenerate with `generate_fixtures.ps1` (uses the FFmpeg CLI and the
LGPL-safe `mpeg4` encoder; honours `$env:VIDEOEDITOR_FFMPEG`, otherwise falls
back to `ffmpeg` on `PATH`).

| File | Purpose |
| --- | --- |
| `static_logo.mp4` | testsrc2 background + static red logo top-right |
| `static_logo_mask.mp4` | white-on-black ground-truth mask for the logo |
| `slow_target.mp4` | yellow block moving slowly left→right |
| `slow_target_mask.mp4` | matching white-on-black target mask |
| `occlusion.mp4` | moving target passing behind a black bar |
| `clean_background.mp4` | same background with no target (PSNR reference) |
| `static_logo_smooth.mp4` | smooth-gradient background + static red logo |
| `static_logo_smooth_mask.mp4` | matching white-on-black logo mask |
| `clean_gradient.mp4` | smooth gradient with no logo (fair PSNR reference) |

Every sample is 640x360, 30 fps, 5 s, mpeg4 q=3, so the whole corpus stays
small enough to commit and fast enough for CI.

The larger 1080p30 10 s acceptance assets live **outside the repository** at
`C:/m4-spike/acceptance/`:

- `static_logo_1080p_10s.mp4`
- `static_logo_mask_1080p_10s.mp4`
- `clean_background_1080p_10s.mp4`

`VideoEditor.exe --selftest-removal` prefers those files when present and
falls back to the committed 640x360 fixtures otherwise.
