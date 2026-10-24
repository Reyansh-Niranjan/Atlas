#!/usr/bin/env bash
# scripts/convert.sh
# ──────────────────────────────────────────────────────────────────────────────
# ATLAS Handheld Terminal — Music Video Converter (16:9 @ 60 FPS)
# Converts an input video (MP4) into synchronized audio & video assets:
#
#   1. Audio: dracula.wav   (44.1 kHz, 16-bit, stereo PCM WAV)
#   2. Video: dracula.mjpeg (320x180 16:9 Widescreen @ 60fps MJPEG stream)
#
# Target SD Card Destination:
#   /sdcard/assets/DraculaSong/dracula.wav
#   /sdcard/assets/DraculaSong/dracula.mjpeg
#
# Usage:
#   ./scripts/convert.sh [input_video] [output_directory]
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

INPUT="${1:-assets/DraculaSong/input.mp4}"
OUT_DIR="${2:-assets/DraculaSong}"

mkdir -p "$OUT_DIR"

OUT_WAV="${OUT_DIR}/dracula.wav"
OUT_MJPEG="${OUT_DIR}/dracula.mjpeg"
FRAMES_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t 'atlas_frames')"

VIDEO_FPS="${VIDEO_FPS:-60}"
VIDEO_QSCALE="${VIDEO_QSCALE:-5}"     # 2=highest quality, 5=recommended balance, 8=compact
AUDIO_RATE="44100"
WIDTH=320
HEIGHT=180                            # 16:9 widescreen (320x180) for 320x240 screen

echo "=========================================================="
echo " ATLAS AV Converter: 16:9 Widescreen @ ${VIDEO_FPS} FPS"
echo " Input Video : ${INPUT}"
echo " Output Dir  : ${OUT_DIR}"
echo " Target Res  : ${WIDTH}x${HEIGHT} (16:9) @ ${VIDEO_FPS} fps"
echo " Audio Format: ${AUDIO_RATE} Hz 16-bit Stereo PCM"
echo "=========================================================="

# 1. Extract 44.1kHz 16-bit Stereo PCM Audio
echo "▶ [1/3] Extracting Audio -> ${OUT_WAV}"
ffmpeg -y -i "$INPUT" \
    -vn \
    -acodec pcm_s16le \
    -ar "$AUDIO_RATE" \
    -ac 2 \
    "$OUT_WAV"

# 2. Extract Scaled Baseline JPEG Frames (Native 16:9 320x180 - Zero Pad Waste)
echo "▶ [2/3] Extracting Video Frames -> ${FRAMES_DIR}/"
ffmpeg -y -i "$INPUT" \
    -vf "scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=decrease,setsar=1" \
    -r "$VIDEO_FPS" \
    -qscale:v "$VIDEO_QSCALE" \
    -f image2 \
    "${FRAMES_DIR}/frame_%06d.jpg"

FRAME_COUNT=$(ls -1 "${FRAMES_DIR}"/frame_*.jpg 2>/dev/null | wc -l)
echo "   Extracted ${FRAME_COUNT} frames."

# 3. Pack Frames with 4-Byte Little-Endian Size Header into .mjpeg
echo "▶ [3/3] Packing Size-Prefixed MJPEG Stream -> ${OUT_MJPEG}"
python3 - <<PYEOF
import struct, os, glob, sys

frames_dir = "${FRAMES_DIR}"
out_path   = "${OUT_MJPEG}"

files = sorted(glob.glob(os.path.join(frames_dir, "frame_*.jpg")))
if not files:
    print("ERROR: No frames found in " + frames_dir, file=sys.stderr)
    sys.exit(1)

total = 0
with open(out_path, "wb") as out:
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        # 4-byte little-endian length prefix: [uint32_le size][jpeg data...]
        out.write(struct.pack("<I", len(data)))
        out.write(data)
        total += 1

size_mb = os.path.getsize(out_path) / (1024 * 1024)
print(f"   Successfully packed {total} frames into {out_path} ({size_mb:.2f} MB)")
PYEOF

rm -rf "$FRAMES_DIR"

echo "=========================================================="
echo "✅ Conversion Complete!"
echo "Place files on your FAT32 MicroSD card under:"
echo "   /assets/DraculaSong/dracula.wav"
echo "   /assets/DraculaSong/dracula.mjpeg"
echo "=========================================================="
