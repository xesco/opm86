#!/bin/sh
# Records the README's animation, docs/demo.webp: 20 s of a song in the app,
# from 0:40, 10 frames a second, as an animated WebP 960 px wide. Needs the
# app built (make) and Pillow. Usage: scripts/demo.sh song.vgz [start seconds]
set -e
song="$1"
start="${2:-40}"
[ -f "$song" ] || { echo "demo: give a .vgz file, as in scripts/demo.sh \"01 Magical Sound Shower.vgz\"" >&2; exit 1; }
frames=$(mktemp -d)
"./OPM86 Player.app/Contents/MacOS/OPM86 Player" --record "$song" "$frames" "$start" 20 10 >/dev/null
python3 - "$frames" <<'PY'
import sys
from PIL import Image
folder = sys.argv[1]
times = [float(t) for t in open(f"{folder}/times.txt")]
# Each frame lasts until the next one's song time, so the animation keeps the song's pace.
durations = [round((b - a) * 1000) for a, b in zip(times, times[1:])] + [100]
frames = [Image.open(f"{folder}/{i:03d}.png").convert("RGB").resize((960, 562), Image.LANCZOS) for i in range(len(times))]
frames[0].save("docs/demo.webp", save_all=True, append_images=frames[1:], duration=durations, loop=0, quality=45, method=6)
PY
rm -rf "$frames"
echo "wrote docs/demo.webp"
