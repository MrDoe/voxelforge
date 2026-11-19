#!/usr/bin/env bash
# bake.sh — (re)generate the LighthouseIsle scenery from its sources.
#
# Standalone by design (agreed with the scenery-migrator session): the isle
# baker is an author-time tool and does NOT ride the default ninja build,
# exactly like heightmap_gen's `world` target — it just isn't wired into
# CMakeLists. Compile line mirrors the `heightmap_gen` target in CMakeLists.
#
#   ./scenery/LighthouseIsle/bake.sh            # full pipeline
#   ./scenery/LighthouseIsle/bake.sh terrain    # terrain shell only
#
# Stages: terrain -> compile -> bake -> textures -> nature layers -> meshes
set -euo pipefail
cd "$(dirname "$0")/../.."            # repo root
SCENERY=scenery/LighthouseIsle
BIN=build/isle_gen

# Wren's scenery migration is in flight on this tree; keep every headless
# authoring run on an isolated overlay so nothing can touch a session file.
export VF_SCENERY=LighthouseIsle
export VF_OVERLAY_PATH=/tmp/opencode/isle_overlay-$$.vxw

if [[ "${1:-all}" != "terrain" ]]; then
  echo "== compile isle_gen"
  g++ -std=c++20 -O2 -DGLM_ENABLE_EXPERIMENTAL \
      -DVOXELFORGE_ASSET_DIR="\"$(pwd)/assets\"" \
      -Isrc -Iexternal -Ibuild/_deps/glm-src -Ibuild/_deps/spdlog-src/include \
      tools/isle_gen.cpp \
      src/voxel/heightmap.cpp src/voxel/worldfile.cpp src/voxel/scenery.cpp \
      build/_deps/spdlog-build/libspdlog.a \
      -o "$BIN"
fi

echo "== terrain bake"
"$BIN" "$SCENERY/heightmap.png" "$SCENERY/world.vxw"

echo "== textures (cached; needs network first time)"
python3 tools/isle_textures.py

echo "== nature layers (vf_mcp write_object)"
python3 tools/isle_nature.py

echo "== CC0 hero meshes (needs build/vf_mesh2vox from the shared build)"
./"$SCENERY"/isle_meshes.sh

echo "== done. preview:"
echo "   ./build/voxelforge --scenery LighthouseIsle"
