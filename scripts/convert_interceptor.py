# -*- coding: utf-8 -*-
import os, sys, subprocess, struct, time

# Set utf-8 encoding for stdout
sys.stdout.reconfigure(encoding='utf-8')

downloads_dir = r"C:\Users\Reyansh\Downloads"
target_name = None
for f in os.listdir(downloads_dir):
    if "Tornadotok animation" in f and f.endswith(".mp4"):
        target_name = os.path.join(downloads_dir, f)
        break

if not target_name or not os.path.exists(target_name):
    print("ERROR: Target input video not found!")
    sys.exit(1)

staging_dir = r"d:\Atlas\assets\InterceptorEdit"
os.makedirs(staging_dir, exist_ok=True)

out_wav = os.path.join(staging_dir, "InterceptorEdit.wav")
out_mjpeg = os.path.join(staging_dir, "InterceptorEdit.mjpeg")

print(f"Found input video: {target_name}")
print(f"Staging to: {staging_dir}")

# 1. Extract Audio -> InterceptorEdit.wav (44.1kHz 16-bit stereo PCM)
print(f"Extracting audio to {out_wav}...")
t0 = time.time()
res = subprocess.run([
    'ffmpeg', '-y', '-v', 'error',
    '-i', target_name,
    '-vn', '-acodec', 'pcm_s16le',
    '-ar', '44100', '-ac', '2',
    out_wav
], capture_output=True, text=True)
if res.returncode != 0:
    print(f"Audio extraction failed: {res.stderr}")
    sys.exit(1)
print(f"Audio extracted in {time.time()-t0:.1f}s ({os.path.getsize(out_wav)/(1024*1024):.2f} MB)")

# 2. Extract and Pack MJPEG Stream -> InterceptorEdit.mjpeg (320x180 @ 30 FPS, size-prefixed)
print(f"Extracting and packing video frames to {out_mjpeg}...")
t0 = time.time()
cmd = [
    'ffmpeg', '-v', 'error',
    '-i', target_name,
    '-vf', 'scale=320:180:force_original_aspect_ratio=decrease,setsar=1',
    '-r', '30',
    '-qscale:v', '4',
    '-pix_fmt', 'yuvj420p',
    '-f', 'image2pipe', '-vcodec', 'mjpeg', '-'
]

proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=65536)
f_out = open(out_mjpeg, 'wb')

frame_count = 0
buf = bytearray()
chunk_size = 32768

while True:
    data = proc.stdout.read(chunk_size)
    if not data:
        break
    buf.extend(data)
    
    # Process all complete JPEGs in buffer
    while True:
        soi = buf.find(b'\xff\xd8')
        if soi == -1:
            buf.clear()
            break
        if soi > 0:
            del buf[:soi]
        
        eoi = buf.find(b'\xff\xd9', 2)
        if eoi == -1:
            break
        
        frame_len = eoi + 2
        frame_bytes = buf[:frame_len]
        del buf[:frame_len]
        
        f_out.write(struct.pack('<I', len(frame_bytes)))
        f_out.write(frame_bytes)
        frame_count += 1

f_out.close()
proc.wait()

print(f"Done! Packed {frame_count} frames in {time.time()-t0:.1f}s ({os.path.getsize(out_mjpeg)/(1024*1024):.2f} MB)")
