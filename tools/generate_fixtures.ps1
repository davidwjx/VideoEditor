$ErrorActionPreference = "Stop"
$ffmpeg = if ($env:VIDEOEDITOR_FFMPEG) { $env:VIDEOEDITOR_FFMPEG } else { "ffmpeg" }
$out = Join-Path $PSScriptRoot "..\tests\fixtures"
New-Item -ItemType Directory -Force $out | Out-Null

$src = "testsrc2=size=640x360:rate=30:duration=2"
$audio = "sine=frequency=440:sample_rate=48000:duration=2"

# $ErrorActionPreference = "Stop" does not turn a native executable's non-zero
# exit into a terminating error in Windows PowerShell 5.1, so every encode is
# followed by an explicit $LASTEXITCODE check.

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v mpeg4 -q:v 3 -c:a aac -shortest (Join-Path $out "mpeg4.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating mpeg4.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest (Join-Path $out "h264.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating h264.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx265 -pix_fmt yuv420p -tag:v hvc1 -c:a aac -shortest (Join-Path $out "h265.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating h265.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx265 -pix_fmt yuv420p10le -tag:v hvc1 -c:a aac -shortest (Join-Path $out "h265_10bit.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating h265_10bit.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v wmv2 -c:a wmav2 -shortest (Join-Path $out "wmv2.wmv")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating wmv2.wmv: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=red:size=64x64:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p (Join-Path $out "red.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating red.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p -an (Join-Path $out "noaudio.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating noaudio.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=30:duration=2" `
  -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" `
  -map 0:v -map 1:v -c:v libx264 -pix_fmt yuv420p (Join-Path $out "twovideo.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating twovideo.mp4: $ffmpeg" }

# FFmpeg 7.x ignores the legacy "rotate" metadata; apply the display matrix on a
# remux pass instead. -display_rotation is an input option, so it cannot be
# combined with the lavfi source directly.
$rotatedBase = Join-Path $out "rotated.tmp.mp4"
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p $rotatedBase
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating rotated.mp4 base: $ffmpeg" }
& $ffmpeg -y -hide_banner -loglevel error -display_rotation:v:0 270 -i $rotatedBase `
  -c copy (Join-Path $out "rotated.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating rotated.mp4: $ffmpeg" }
Remove-Item $rotatedBase

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=30:duration=3" `
  -vf "select='if(lt(mod(n,3),2),1,0)'" -fps_mode vfr -c:v libx264 -pix_fmt yuv420p `
  (Join-Path $out "vfr.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating vfr.mp4: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=2560x1440:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p (Join-Path $out "highres.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating highres.mp4: $ffmpeg" }

# Native 1080p source for the M2 3V/2A/1S performance gate.
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=5" `
  -c:v libx264 -preset medium -crf 32 -pix_fmt yuv420p -movflags +faststart `
  (Join-Path $out "perf1080.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating perf1080.mp4: $ffmpeg" }

Copy-Item (Join-Path $out "h264.mp4") (Join-Path $out "中文 文件名.mp4") -Force
Set-Content -Path (Join-Path $out "corrupt.mp4") -Value "this is not a video" -Encoding Ascii

# M2 audio, sync and performance fixtures.
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" `
  -c:a pcm_s16le (Join-Path $out "tone.wav")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating tone.wav: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" `
  -c:a libmp3lame -b:a 192k (Join-Path $out "bgm.mp3")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating bgm.mp3: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" `
  -c:a flac (Join-Path $out "bgm.flac")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating bgm.flac: $ffmpeg" }

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" `
  -c:a aac -b:a 192k (Join-Path $out "bgm.m4a")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating bgm.m4a: $ffmpeg" }

# sync.mp4: testsrc2 timestamp pattern + a 100 ms 1 kHz beep at every whole second.
& $ffmpeg -y -hide_banner -loglevel error `
  -f lavfi -i "testsrc2=size=640x360:rate=30:duration=10" `
  -f lavfi -i "aevalsrc=0.8*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.1):s=48000:d=10" `
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest (Join-Path $out "sync.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE while generating sync.mp4: $ffmpeg" }

Write-Host "Fixtures written to $out"
