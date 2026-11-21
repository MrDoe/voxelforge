---
title: Terrain "slits" seen from a buried camera
tags: [splat, terrain, holes, water, camera, collision, measurement, debugging]
sourceRefs: [src/app/app.hpp, src/app/frame/record_interactive.cpp, src/app/frame/record_headless.cpp, src/render/splat_pass.cpp, shaders/splat.vert, shaders/splat.frag, src/voxel/surfelize.cpp]
lastReviewed: 2026-11-20
---

# Terrain "slits" seen from a buried camera

A low-angle view can show bright, checkered water/sky streaks in apparently
solid grass. The 2026-11-20 investigation established that the usual cause is
**not** splat coverage: the camera is inside the heightfield.

## The measurement that settled it

Repro camera from the user's live app: position `(1.3, 2.1, 10.4)`, heading
268° W, LighthouseIsle.

- `vf_mcp ground` reported terrain **2.93 m** at the camera's own xz, while the
  camera was at y=2.1 — embedded by 0.83 m.
- Sampling the eye ray along its whole length (t=1..9 m): ground 2.70 → 1.70 m
  while the ray's y falls 1.70 → −1.46 m. The ray **never exits the solid**.
- Therefore no terrain top disk lies along the ray; the world-wide water plane
  at y=−0.9 is still drawn and passes the depth test. Its 0.2 m coplanar surfel
  grid is what the "slit" texture actually is.
- Lifting only the camera to y=3.2 above the same spot removed the artifact:
  bright lower-half pixels **26,778 → 8**.

## What does NOT fix it (all measured on that camera)

| lever | result |
|---|---|
| `VF_SPLAT_RADIUS` 1.0 → 1.3 → 1.6 → 2.0 | bright-pixel count falls, but the **water on/off diff mask stays ~100 % water** in the slit region — the water merely shades darker, terrain never covers it |
| `VF_LOD=0`, `VF_ANISO=0`, `VF_LOD_SPLIT=0` | byte-identical in the slit region |
| `VF_SPLAT_NOCULL`, `VF_NO_GPU_CULL`, `VF_NO_OCCL` | byte-identical in the slit region |
| `VF_TERRAIN_ANALYTIC=0` | byte-identical in the slit region |
| emitting a riser face disk on every exposed terrain step | 0 px improvement in the slit region; adds horizontal banding elsewhere |
| tilting + growing terrain disks while buried | 0 px improvement in the slit region; flattens the grass |
| suppressing all water draws while buried | slit becomes sky, and the whole lake disappears — not a fix |

The general lesson: **a flat disk cannot cover a surface that was never
drawn**, and the terrain surfel set has no underside. No radius or normal
change can seal a view through a heightfield from inside it. Also see
[[concepts/splat-edge-fade-measurement]] for the water-mask trap: a falling
"bright pixel" count is not evidence of coverage — always re-render with
`VF_SPLAT_NOWATER=1` and compare the water-diff mask.

## The fix that landed

`App::updateBuriedProbe(bool ejectEmbedded)` (in `src/app/app.hpp`) now lifts
an embedded camera out of the heightfield before the frame is recorded:

- terrain-only, using `VoxelField::colTops()` at the camera's xz (so a camera
  inside a building, which has no column top, is untouched);
- only when **more than 0.30 m** below the terrain top, so a camera a few cm
  under a rough surface is left alone;
- target height is the column top's world face + **0.12 m** (a larger 0.35 m
  margin lifted the view enough to expose the 10 cm heightfield staircase;
  measured 0.05 m above the top already seals the slit, and y = top + 0.12 m
  rendered 0 bright pixels at the repro camera);
- gated by `m_collisionEnabled` and `VF_NO_CAMERA_EJECT`;
- called with `ejectEmbedded=true` **only from `recordInteractiveFrame`**.
  Headless `--cam` placement is intentionally uncollided and every gate relies
  on exact cameras — applying the eject there broke `live_edit_check`
  (black-in-silhouette 35 %, sky probe); it stays off in `recordHeadlessFrame`.

## A second class of "holes" — real water past a stepped silhouette

At a low-angle view above the 11.8 m mountain (`-4,12.3,14 → 0,11.8,4`) the
water-diff mask found 3,129 enclosed bright pixels in a contour band. Ray/ground
sampling showed those rays fly **over** a 6 m cliff into a real valley — the
water is legitimately visible between the voxel steps of the silhouette, not
through a missing surface. A radius sweep made it worse on both sides
(0.7/0.85/1.1/1.3 → 8,344/5,931/5,105/7,928 water px vs 3,572 at the default
1.0); LOD, cull and analytic-terrain knobs did not help. The shipped radius is
already the optimum. The "stairs" read is the same 10 cm heightfield: the
per-fragment heightfield normal (bit 9) smooths the cell-scale metric 7.34 →
6.78 but darkens the ground badly (mean 83.9 → 46.5, near-black 4.6k → 64.8k),
and widening the bake's heightfield taps (e 0.35 → 0.8, e2 0.10 → 0.30) was a
no-op after a forced rebuild. Both were rejected; removing the staircase needs
a heightfield-projected terrain surfel, not a knob.

## Detection traps

- `VF_SPLAT_DEBUG=16` paints water dark blue as well as terrain. Set
  `VF_SPLAT_NOWATER=1` when using the ownership mask to look for holes, or the
  water itself will read as "terrain present".
- A debug view skips the normal-mode backface collapse and alpha handling, so
  a hole seen in the shaded frame can be invisible in a debug frame.
- Query the terrain height at the camera xz **first**. If the camera is below
  it, stop tuning splats.

See also [[concepts/water-plane]], [[concepts/terrain-surfel-interpolation]],
[[concepts/collision]] and [[concepts/splat-edge-fade-measurement]].
