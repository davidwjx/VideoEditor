# Generates the small 640x360 / 5 s / 30 fps removal evaluation fixtures.
#
# These are test data only and never enter an installer. They are produced with
# the FFmpeg CLI using the LGPL-safe mpeg4 encoder; point VIDEOEDITOR_FFMPEG at
# a specific binary if ffmpeg is not on PATH.
param()

$ff = $env:VIDEOEDITOR_FFMPEG
if (-not $ff) { $ff = "ffmpeg" }
$out = $PSScriptRoot
$size = "640x360"
$rate = 30
$dur = 5

New-Item -ItemType Directory -Force -Path $out | Out-Null

# 1) Static logo: testsrc2 background plus a red block in the top-right corner.
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x=520:y=30:w=80:h=50:color=red@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\static_logo.mp4"

# 2) Slow moving target: a yellow block moving left to right.
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x='80+60*t':y=140:w=70:h=120:color=yellow@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\slow_target.mp4"

# 3) Clean background (ground truth): the same background without a target.
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -c:v mpeg4 -q:v 3 "$out\clean_background.mp4"

# 4) Occlusion sample: the target passes behind a black vertical bar.
& $ff -y -f lavfi -i "testsrc2=size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x='80+60*t':y=140:w=70:h=120:color=yellow@1:t=fill,drawbox=x=300:y=0:w=30:h=360:color=black@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\occlusion.mp4"

# 5) Ground-truth masks: white target on black, same motion as the samples.
& $ff -y -f lavfi -i "color=c=black:size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x='80+60*t':y=140:w=70:h=120:color=white@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\slow_target_mask.mp4"

& $ff -y -f lavfi -i "color=c=black:size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x=520:y=30:w=80:h=50:color=white@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\static_logo_mask.mp4"

# 6) Smooth-gradient set: a fair PSNR reference. A smooth fill can plausibly
#    reconstruct a gradient, unlike the high-frequency testsrc2 plate. The
#    seed and endpoints must be pinned: the gradients filter randomizes its
#    geometry otherwise and the two clips would not share a background.
$grad = "gradients=size=$size:rate=$rate:duration=$dur:c0=0x1E3A8A:c1=0xF59E0B:speed=0:seed=12345:x0=0:y0=0:x1=640:y1=360"
& $ff -y -f lavfi -i "$grad" `
  -c:v mpeg4 -q:v 3 "$out\clean_gradient.mp4"
& $ff -y -f lavfi -i "$grad" `
  -vf "drawbox=x=520:y=30:w=80:h=50:color=red@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\static_logo_smooth.mp4"
& $ff -y -f lavfi -i "color=c=black:size=$size:rate=$rate:duration=$dur" `
  -vf "drawbox=x=520:y=30:w=80:h=50:color=white@1:t=fill" `
  -c:v mpeg4 -q:v 3 "$out\static_logo_smooth_mask.mp4"

Write-Host "Removal fixtures written to $out"
