---
title: Water caustics and the absorption-alpha ceiling
tags: [water, rendering, caustics, shading, splat, svo]
sourceRefs: [shaders/common_base.glsl, shaders/common_splat.glsl, shaders/svo_raymarch.comp, tests/live_edit_check.py]
lastReviewed: 2026-09-19
---

# Water caustics

`causticAt(xz, t)` (`shaders/common_base.glsl`) is the refracted-sunlight cell
web: two crossed sine grids drifting at different rates interfere, and a
`pow(..., 4.0)` sharpens the bright cells the way a real caustic focuses
light. Pure function of (world xz, time), so both backends and every water
path agree.

## The absorption-alpha ceiling (why it is composited at the surface)

The physically-correct place for caustics is the submerged bed. That does not
work here: `shadeWaterSplat` sets

```
outA = clamp(0.35 + depth * 0.9, 0.0, 0.97)
```

so the water is **97 % opaque by ~0.7 m depth** and the bed is hidden. A
bed-only caustic was measured at **10× strength** and still changed only 1.7 %
of the water frame. Moving the same term to the water **surface** (same light
path, just composited at the wrong depth) changed **10.6 % of pixels at 0.85
strength**, +1.98 % mean luma, no clipping (0.001 %, same as baseline).

The bed term is kept at a small 0.25 strength for genuinely shallow water;
the surface term carries the visible effect and fades with
`exp(-depth * 0.45)` so deep water stays dark. It animates (13.5 % of pixels
change over 3 s of `animtime`).

## Interaction with `live_edit_check`

`live_edit_check` carves a channel at world (0.05, 3.05) and asserts the
newly-exposed water matches the open water colour within 12/255 per channel.
The caustic web is spatially varying, but its **region means are unbiased**
(measured bias 0.001 over the channel vs the open river), so it does not
break that guard. Shoreline **vegetation** there does — see
[[concepts/world-detail-content]].

Cross-refs: [[concepts/water-plane]] (the fixed-level plane and the
height-texture sync), [[concepts/detail-normals]] (the other shading-only
detail layer).
