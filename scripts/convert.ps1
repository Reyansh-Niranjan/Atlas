<#
.SYNOPSIS
    ATLAS Handheld Terminal — Music Video Converter (PowerShell for Windows)
    Converts MP4 into 16:9 Widescreen (320x180) @ 60 FPS MJPEG + 44.1kHz WAV.

.EXAMPLE
    .\scripts\convert.ps1 -InputVideo "assets\DraculaSong\input.mp4" -OutputDir "assets\DraculaSong"
#>

param(
    [string]$InputVideo = "assets\DraculaSong\input.mp4",
    [string]$OutputDir  = "assets\DraculaSong",
    [int]$Fps           = 30,
    [int]$Width         = 320,
    [int]$Height        = 180,
    [int]$Quality       = 4
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $InputVideo)) {
    Write-Error "Input video file not found: $InputVideo"
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutWav   = Join-Path $OutputDir "dracula.wav"
$OutMjpeg = Join-Path $OutputDir "dracula.mjpeg"
$FramesDir = Join-Path ([System.IO.Path]::GetTempPath()) ("atlas_frames_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $FramesDir | Out-Null

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " ATLAS AV Converter: 16:9 Widescreen @ $Fps FPS" -ForegroundColor Green
Write-Host " Input Video : $InputVideo"
Write-Host " Output Dir  : $OutputDir"
Write-Host " Target Res  : ${Width}x${Height} (16:9) @ $Fps fps"
Write-Host " Audio Format: 44100 Hz 16-bit Stereo PCM"
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Extract 44.1kHz 16-bit Stereo PCM Audio
Write-Host "▶ [1/3] Extracting Audio -> $OutWav" -ForegroundColor Yellow
& ffmpeg -y -i $InputVideo -vn -acodec pcm_s16le -ar 44100 -ac 2 $OutWav

# 2. Extract Scaled Baseline JPEG Frames (Native 16:9 320x180, 4:2:0 subsampling)
Write-Host "▶ [2/3] Extracting Video Frames -> $FramesDir" -ForegroundColor Yellow
$FramePattern = Join-Path $FramesDir "frame_%06d.jpg"
& ffmpeg -y -i $InputVideo -vf "scale=${Width}:${Height}:force_original_aspect_ratio=decrease,setsar=1" -r $Fps -qscale:v $Quality -pix_fmt yuvj420p -f image2 $FramePattern

# 3. Pack Frames into size-prefixed MJPEG stream
Write-Host "▶ [3/3] Packing Size-Prefixed MJPEG Stream -> $OutMjpeg" -ForegroundColor Yellow

$PyScript = @"
import struct, os, glob, sys

frames_dir = r"$FramesDir"
out_path   = r"$OutMjpeg"

files = sorted(glob.glob(os.path.join(frames_dir, "frame_*.jpg")))
if not files:
    print("ERROR: No frames found in " + frames_dir, file=sys.stderr)
    sys.exit(1)

total = 0
with open(out_path, "wb") as out:
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        out.write(struct.pack("<I", len(data)))
        out.write(data)
        total += 1

size_mb = os.path.getsize(out_path) / (1024 * 1024)
print(f"   Successfully packed {total} frames into {out_path} ({size_mb:.2f} MB)")
"@

python -c $PyScript

Remove-Item -Recurse -Force $FramesDir

Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Conversion Complete!" -ForegroundColor Green
Write-Host "Place files on your FAT32 MicroSD card under:"
Write-Host "   /assets/DraculaSong/dracula.wav"
Write-Host "   /assets/DraculaSong/dracula.mjpeg"
Write-Host "==========================================================" -ForegroundColor Green
