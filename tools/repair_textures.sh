#!/bin/bash
cd /home/christoph/code/voxelforge
for f in assets/textures/*_1.png; do
    base=$(basename "$f" .png)
    out="assets/textures/${base}_seamless.png"
    python3 tools/prepare_texture.py "$f" "$out" 2>&1 | tail -1
done
echo "Done repairing all textures"
