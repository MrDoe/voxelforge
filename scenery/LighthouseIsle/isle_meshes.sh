#!/usr/bin/env bash
# isle_meshes.sh — import the CC0 hero meshes for LighthouseIsle.
#
# Sources (all CC0): Quaternius — Ships, Buildings, Farm Buildings packs
# (public Google Drive folders linked from quaternius.com/packs/*.html).
# Staged by tools/fetch_isle_meshes.py into assets/models/ as isle_*.{obj,mtl}.
#
# INTERIORS: house / barn / windmill import --shell (1-voxel skin, hollow
# interior). An enclosed shell would be flood-filled solid by the field
# builder — the OPEN DOOR cut (isle_interiors.py via isle_opendoor) is what
# keeps the interior air connected to the outside and therefore real. Doors
# cannot be articulated yet, so every door is authored OPEN (user directive).
# Boats are shell too (open hulls). Run AFTER tools/isle_nature.py (terrain
# must exist; placement coordinates mirror isle_gen.cpp / isle_nature.py).
set -euo pipefail
cd "$(dirname "$0")/../.."          # repo root from scenery/LighthouseIsle/
export VF_SCENERY=LighthouseIsle
V=build/vf_mesh2vox

$V models/isle_house.obj     --out isle_house     --fit 7.6  --ground -12,-6    --rot-y 18  --shell
$V models/isle_barn.obj      --out isle_barn      --fit 8.0  --ground -8.5,-2.5 --rot-y 6   --shell
$V models/isle_windmill.obj  --out isle_windmill  --fit 8.5  --ground 18,-2     --rot-y 200 --shell
$V models/isle_sailing.obj   --out isle_sailing   --fit 15   --at 35,-1.3,-13   --rot-y 55  --shell
$V models/isle_rowboat.obj   --out isle_rowboat   --fit 3.2  --at -0.7,-1.15,-23.6 --rot-y 25 --shell

# hollow the buildings + author the furniture around the real bounds, then
# cut the open doorways (south faces):
python3 tools/isle_interiors.py

# EMISSIVE BUDGET: mesh Kd colours that paletteNearest mapped onto an
# EMISSIVE palette entry (mat 10 ember) glow whole roofs and burn the
# derived-light budget; remap to untextured mat 16 keeps the colour, kills
# the glow. Mat 22 (wet soil — a palette-version artifact of the same
# nearest-match, never meant as a roofing material) remaps to mat 7 shingles
# so roofs sample the tile photo. Safety net — check --stats first: a "0
# records" remap line means the mapping MOVED, not that it worked.
O=scenery/LighthouseIsle/bin/isle_opendoor
S=scenery/LighthouseIsle
for f in isle_house isle_barn isle_windmill isle_sailing isle_rowboat; do
    $O $S/$f.vxw --remap 10,16 --remap 22,7
done

echo "mesh layers + interiors imported into $(pwd)/scenery/LighthouseIsle"
