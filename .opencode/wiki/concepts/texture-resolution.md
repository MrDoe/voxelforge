---
title: How much does texture resolution actually buy?
tags: [textures, atlas, resolution, measurement, quality]
sourceRefs: [src/render/texture_atlas.hpp, src/render/texture_atlas.cpp, assets/world.json, shaders/common_base.glsl]
lastReviewed: 2026-09-19
---

# How much does texture resolution actually buy?

**Question (2026-09-19):** is texture resolution the biggest remaining
display-quality lever? **Measured answer: no — it is a real but modest
sharpness fix; relief and atmosphere move the same metrics ~10× more.**

## The numbers

At 1080p / 60° vertical FOV a pixel footprint is **1.069 mm at 1 m** of view
distance. Tile scales come from `world.json` `"textures"` (metres per tile),
so texel size = `scale / atlasSize`:

| material | scale | texel @256 | texel @512 | 1:1 at (512) |
|---|---|---|---|---|
| grass | 2.6 m | 10.2 mm | 5.08 mm | 4.75 m |
| soil | 1.8 m | 7.0 mm | 3.5 mm | 3.3 m |
| rock | 1.2 m | 4.7 mm | 2.3 mm | 2.2 m |
| wood / shingles | 0.9–1.0 m | 3.5–3.9 mm | 1.8–2.0 mm | ~1.7 m |

So the large-area materials (grass, soil, sand) were **magnified** — visibly
blurry — out to ~5–10 m in the mid-field, which is most of a hero frame. The
shipped ambientCG sources were already 512², and the atlas was 256², so
half the existing art was being thrown away by the box filter on upload.

## What the fix bought

Raising `TexAtlas::kTexSize` 256 → 512 (`src/render/texture_atlas.hpp:34` —
the only hardcode; mip count, staging and blits all derive from it) is free
art-wise and costs 5.9 → 23.8 MB VRAM.

A/B on the hero view, 1280×720, headless (no TAA):

- mean |diff| **0.37 / 255** — a subtle change, not a transformation
- Laplacian HF energy: near meadow **+4.8 %**, house/roof **+4.0 %**,
  mid-field **+1.8 %**

## Why it is not the top lever

The surfaces were not detail-limited, they were **flat-lit**: there is no
fragment-level detail normal, so a crisp albedo on a smooth-shaded surface
still reads painted. Adding [[concepts/detail-normals]] moves the same
Laplacian metric by **+40 %** at its default strength — an order of magnitude
more than the resolution change. Atmosphere ([[concepts/volumetric-fog]])
moves the frame by ~+3 % mean luminance and adds depth cueing that no amount
of texel density provides.

Rule of thumb: resolution fixes *sharpness*; normals fix *relief*; fog fixes
*depth*. Ranked by visual effect per unit of effort the order is
relief ≈ atmosphere > resolution.

Cross-refs: [[concepts/texture-atlas]], [[concepts/texture-conformance]].
