#!/bin/bash
# Batch generate 2K textures for all low-res materials in voxelforge
# Run with ComfyUI running at http://127.0.0.1:8188

cd /home/christoph/code/voxelforge
PYTHON=/home/christoph/code/bark-venv/bin/python

# Material -> description mapping for low-res textures needing upgrade
declare -A TEXTURES=(
  [0]="dark green grass, dense, natural meadow, top-down view"
  [3]="fine sand, beige, granular, smooth surface"
  [4]="grey rough rock, weathered, natural stone surface"
  [5]="grey stone, rough texture, natural rock"
  [7]="dark wooden planks, aged oak, horizontal grain"
  [9]="molten lava, glowing orange red, volcanic rock"
  [14]="light grey rock, smooth stone surface"
  [15]="small gravel pebbles, grey, natural ground"
  [17]="brown tree bark, rough, vertical ridges"
  [18]="green moss, soft, organic, dense growth"
  [19]="thatch straw, woven, dry golden brown"
  [20]="white plaster, smooth wall surface, subtle texture"
)

for mat in "${!TEXTURES[@]}"; do
    desc="${TEXTURES[$mat]}"
    echo "=== Generating mat $mat: $desc ==="
    $PYTHON tools/gen_texture.py "$desc" --size 2048 --steps 30 --out-dir assets/textures 2>&1 | tail -3
    echo ""
done

echo "All textures generated. Run tools/check_texture.py to verify conformance."
