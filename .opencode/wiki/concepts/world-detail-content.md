---
title: World detail content (shoreline reeds) + the test keep-out
tags: [world-content, vf-mcp, reeds, shoreline, testing]
sourceRefs: [assets/hamlet_reeds.vxw, assets/hamlet_reeds_far.vxw, assets/world.json, tests/live_edit_check.py, .opencode/skills/voxel-object/SKILL.md]
lastReviewed: 2026-09-19
---

# World detail content

The hamlet's authored layers (`hamlet_hall`, `_tower`, `_pier`, `_boat`,
`_well`, `_market`, `_garden`, `_pines`, `_props`) are runtime-authored with
`vf_mcp write_object` (see the `voxel-object` skill). 2026-09-19 added two
shoreline layers: **`hamlet_reeds`** (near bank) and **`hamlet_reeds_far`**
(hamlet side), 5 086 voxels total.

## Recipe

Positions are generated from `assets/heightmap.png`: sample the terrain and
keep cells in the band **−1.5 … −0.42 m** (the shallow margin around
`WATER_LEVEL = -0.9`), thinned to one clump per 1.5 m. Each clump is 4–6
`box` shapes of `size [1, 9..14, 1]` (0.9–1.4 m stems that always clear the
surface) at ±3-cell jitter, `mat 8` (foliage), colour ramped from saturated
reed green `(84,116,42)` to dry straw `(140,138,75)`, plus a 2-cell brown
seed head `(92,64,36)` on ~45 % of tall stems.

Both layers are anchored with `ground:[14, 8]` (anchor cell `[651,513,592]`,
terrain +0.10 m); `at` offsets are anchor-relative voxels.

## Two hard-won rules

1. **Colour must be saturated enough to read.** The first pass used an olive
   `(120,130,60)` that blended into the terrain — and in `ascii_view.py` it
   classified as `.` (mixed) rather than `g` (green), which made it
   invisible to the very tool meant to verify it. Saturated green
   (`g > r + 20`) both looks like vegetation and is checkable.
2. **Keep a keep-out zone clear of scenery.** `tests/live_edit_check.py`
   carves at world (0.05, 2.05) and (0.05, 3.05) and asserts the newly
   exposed water matches the open water within 12/255 per channel. Reeds in
   that area shifted the carved-water mean **22 codes in blue** and failed
   the guard. The reed layers exclude **world x −3…3, z −1…6**; with the
   keep-out the guard passes (93,114,125 vs 92,114,124).

## Measured effect (hero view, 960×540, vs the same build without reeds)

- mean |diff| 1.14, **4.11 % of pixels changed** by >3 codes, max 102
- Laplacian HF energy **+2.19 %**
- The sparse first pass (46 clumps, 2–4 stems) moved only 2.66 % of pixels;
  densifying to 87 clumps / 4–6 stems roughly doubled it.

Cross-refs: [[concepts/voxel-object-authoring]] (the authoring loop),
[[concepts/water-caustics]] (the other new water-adjacent detail),
[[entities/hamlet-scene]] (the scene these layers join).
