#!/usr/bin/env bash
# fur_ab.sh - the fur cards, A/B, in one command.
#
# Renders the alpaca twice (fur on / fur off) and prints the numeric
# difference. The numbers are the point: a caption cannot judge a 0.1%
# artefact, and the noise floor of the scene (its animated grass) has to be
# measured before a difference means anything.
#
#   tools/fur_ab.sh [outdir] [camera]
#
# The default camera fills the frame with the alpaca (the third micro
# object, in the paddock east of the cabin).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/build/voxelforge"
OUT="${1:-/tmp/vf-fur-ab}"
CAM="${2:-10.95,0.85,11.45,11.75,0.52,12.55}"
W="${W:-900}"
H="${H:-600}"

mkdir -p "$OUT"
# The live-edit overlay is a SHARED file: never let a scratch render write it.
export VF_OVERLAY_PATH="$OUT/overlay.vxw"

echo "camera: $CAM   ($W x $H)"
echo "--- fur OFF (VF_FUR=0)"
VF_FUR=0 "$APP" --shot "$OUT/off.ppm" --cam "$CAM" --width "$W" --height "$H" 2>&1 |
    grep -E "fur:|shot written" || true
echo "--- fur ON"
"$APP" --shot "$OUT/on.ppm" --cam "$CAM" --width "$W" --height "$H" 2>&1 |
    grep -E "fur:|shot written" || true

echo "--- the difference (ON vs OFF)"
python3 "$ROOT/tools/fur_diff.py" "$OUT/on.ppm" "$OUT/off.ppm"
echo "--- the high-frequency noise (the body alone is ~12.8 at this camera)"
python3 "$ROOT/tools/hf_noise.py" "$OUT/on.ppm" 260 100 520 480
python3 "$ROOT/tools/hf_noise.py" "$OUT/off.ppm" 260 100 520 480

for p in on off; do
    if command -v convert >/dev/null 2>&1; then
        convert "$OUT/$p.ppm" -resize 900x "$OUT/$p.png"
        echo "wrote $OUT/$p.png"
    fi
done
