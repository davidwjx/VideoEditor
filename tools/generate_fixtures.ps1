$ErrorActionPreference = "Stop"
$ffmpeg = if ($env:VIDEOEDITOR_FFMPEG) { $env:VIDEOEDITOR_FFMPEG } else { "ffmpeg" }
$out = Join-Path $PSScriptRoot "..\tests\fixtures"
New-Item -ItemType Directory -Force $out | Out-Null

$src = "testsrc2=size=640x360:rate=30:duration=2"
$audio = "sine=frequency=440:sample_rate=48000:duration=2"

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v mpeg4 -q:v 3 -c:a aac -shortest (Join-Path $out "mpeg4.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest (Join-Path $out "h264.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx265 -pix_fmt yuv420p -tag:v hvc1 -c:a aac -shortest (Join-Path $out "h265.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v libx265 -pix_fmt yuv420p10le -tag:v hvc1 -c:a aac -shortest (Join-Path $out "h265_10bit.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i $src -f lavfi -i $audio `
  -c:v wmv2 -c:a wmav2 -shortest (Join-Path $out "wmv2.wmv")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=red:size=64x64:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p (Join-Path $out "red.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p -an (Join-Path $out "noaudio.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=30:duration=2" `
  -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" `
  -map 0:v -map 1:v -c:v libx264 -pix_fmt yuv420p (Join-Path $out "twovideo.mp4")

# FFmpeg 7.x ignores the legacy "rotate" metadata; apply the display matrix on a
# remux pass instead. -display_rotation is an input option, so it cannot be
# combined with the lavfi source directly.
$rotatedBase = Join-Path $out "rotated.tmp.mp4"
& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p $rotatedBase
& $ffmpeg -y -hide_banner -loglevel error -display_rotation:v:0 270 -i $rotatedBase `
  -c copy (Join-Path $out "rotated.mp4")
Remove-Item $rotatedBase

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x240:rate=30:duration=3" `
  -vf "select='if(lt(mod(n,3),2),1,0)'" -fps_mode vfr -c:v libx264 -pix_fmt yuv420p `
  (Join-Path $out "vfr.mp4")

& $ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=2560x1440:rate=30:duration=1" `
  -c:v libx264 -pix_fmt yuv420p (Join-Path $out "highres.mp4")

Copy-Item (Join-Path $out "h264.mp4") (Join-Path $out "中文 文件名.mp4") -Force
Set-Content -Path (Join-Path $out "corrupt.mp4") -Value "this is not a video" -Encoding Ascii

Write-Host "Fixtures written to $out"
