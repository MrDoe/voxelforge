#!/usr/bin/env bash
# shots.sh — render the LighthouseIsle verification/gallery set.
# Headless gates' rules apply: VF_NO_OVERLAY=1 so nothing can touch a
# session overlay; PNGs go to tests/screenshots/ (the gallery), PPMs stay
# under /tmp/opencode. Run this ONCE per GPU window; ~4 app loads (~15 s
# each of EDT is unavoidable for now).
set -euo pipefail
cd "$(dirname "$0")/../.."
export VF_SCENERY=LighthouseIsle VF_NO_OVERLAY=1
OUT=/tmp/opencode/isle_shots
GAL=tests/screenshots
mkdir -p "$OUT" "$GAL"
SH="$GAL/../shotlist.txt"; : > "$SH"

# v3 cameras, one load: hero wide (beach+jetty+house+headland), beach flat,
# lighthouse from the sea (its rock plinth is part of the tower layer now),
# the jetty head with lamp + moored rowboat, the cottage on NATURAL ground
# with its own footing, the volcanic cone, and the house INTERIOR seen
# through the open south doorway.
# --shotlist line format: PATH X Y Z TX TY TZ (space separated, no flags).
cat > "$OUT/list_splat.txt" <<'LST'
/tmp/opencode/isle_shots/wide.ppm 14 7.5 -62 -4 2.5 -2
/tmp/opencode/isle_shots/beach.ppm 2 1.8 -27 10 4 -4
/tmp/opencode/isle_shots/lh.ppm 12 7.5 -14 27 12 -5
/tmp/opencode/isle_shots/jetty.ppm -2 2.0 -18.6 -2 0.6 -25.5
/tmp/opencode/isle_shots/cottage.ppm -4.5 3.6 -11.5 -12 2.6 -6
/tmp/opencode/isle_shots/cone.ppm -8 3.2 -4 -6 10 16
/tmp/opencode/isle_shots/interior.ppm -12 3.9 -10.8 -12 3.3 -5.5
LST

VF_RENDER_FLAGS=2559 VF_AUTOEXPOSE=1 VF_LINEAR_FX=1 \
  ./build/voxelforge --shotlist "$OUT/list_splat.txt" --width 1280 --height 720

# dusk: low sun, lantern + cottage/jetty lamps lit, fog on for sea depth
VF_RENDER_FLAGS=2559 VF_AUTOEXPOSE=1 VF_LINEAR_FX=1 VF_VOLFOG=1 \
  ./build/voxelforge --shot "$OUT/dusk.ppm" --width 1280 --height 720 \
  --cam 16,6,-58 -2,3,0 --sun 3 250

# backend parity: same hero cam, SVO reference (and the 3dgs OIT composite
# where the binary supports it — not part of the committed CLI on every build)
./build/voxelforge --mode svo  --shot "$OUT/wide_svo.ppm"  --width 1280 --height 720 \
  --cam 14,7.5,-62 -4,2.5,-2
VF_RENDER_FLAGS=2559 ./build/voxelforge --mode 3dgs --shot "$OUT/wide_3dgs.ppm" \
  --width 1280 --height 720 --cam 14,7.5,-62 -4,2.5,-2 || echo "3dgs mode unavailable on this binary — skipped"

python3 - <<'PY'
from PIL import Image
import os
for n in ("wide","beach","lh","jetty","cottage","cone","interior","dusk","wide_svo","wide_3dgs"):
    p = f"/tmp/opencode/isle_shots/{n}.ppm"
    if os.path.exists(p):
        Image.open(p).save(f"tests/screenshots/isle_{n}.png")
        print("gallery:", f"isle_{n}.png")
PY
