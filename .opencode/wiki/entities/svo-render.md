---
name: svo-render
title: Chunked-SVO raymarch (single path)
sourceRefs: [shaders/svo_raymarch.comp, src/render/svo_pass.cpp, src/voxel/voxel_field.cpp]
---

# Chunked-SVO Raymarch

- `shaders/svo_raymarch.comp` — the only render path: chunk grid (16³) → per-chunk octree → 8³ bricks
- All geometry is data-derived from `.vxw` records via `VoxelField`:
  - terrain: `uHeight` rg32f texture (R = column top Y, G = material), bilinear
  - objects: brick SDF + material byte (bit 7 = object flag); shadows march `uObjVol` (r8_snorm 256³ object-only SDF)
- No analytic scene constants in GLSL. Water plane y=-0.9, fog 0.0012, ACES.
- Push block `RaymarchPush` (128 B) lives in `src/render/svo_pass.hpp`.
