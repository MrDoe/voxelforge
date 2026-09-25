---
title: Water plane (one fixed level)
tags: [water, rendering, splat, svo, live-edit, shading, gotcha]
sourceRefs: [src/voxel/surfelize.cpp, src/voxel/surfelize.hpp, src/app/main.cpp, src/render/splat_pass.cpp, shaders/splat.frag, shaders/common_splat.glsl, shaders/common_svo.glsl, tests/live_edit_check.py]
lastReviewed: 2026-09-17
---

# Water plane (one fixed level)

The water is **one analytic plane at `WATER_LEVEL = -0.9`**. There is no
per-column water state, no flow, no flood — a place is wet exactly when the
terrain surface there lies below the plane. Both backends shade the same
plane:

- **Splat backend** — the plane is rasterized as a **world-wide 0.2 m grid of
  coplanar surfels** (513² ≈ 263k `makeWaterSurfel` cells, ~5% of the surfel
  count). The grid is *pure coverage*: `splat.frag`'s water branch intersects
  the analytic plane per fragment (`tW = (kWaterLevel - ro.y) / rd.y`) and
  shades *there*, so every water fragment on the plane is identical regardless
  of which cell covered it. The depth test against the opaque prepass does the
  clipping — cells standing over dry land/objects never shade. Cost ≈
  0.2–0.45 ms at 640×360 (measured with `--smoke` vs `VF_SPLAT_NOWATER=1`).
- **SVO backend** — the ray march intersects the same plane analytically.

Consequences:

- A dig below the level is water **automatically**; there is no flood pass and
  no water-buffer patching (the 2026-09-17 rework deleted `floodNewlyDug`,
  `SplatPass::patchWaterSurfels` and the water headroom).
- The Carve scoop still has to *open* the ground it starts at
  (`EditableWorld::kCarveTopMargin` = 0.2 m above the pick + the
  `d > VOXEL*1e-3` inclusive boundary — see [[entities/live-edit-brush]]): a
  flat cap left a one-cell roof over the ±1 cell of relief and the channel
  stayed dry-looking.
- `VF_SPLAT_NOWATER=1` skips the plane draw (A/B knob for the tests).

## The bed: everything the water reads must follow edits

`shadeWaterSplat` needs the **bed height** for the shoreline foam, the
absorption alpha and the reflected-bed march — it reads `heightAt()` /
`heightMatNearest()` from the terrain **height texture** (`uHeight`, rg32f =
top world Y + material). That texture is baked once, so after a live edit the
edited columns used to stay stale: the dug channel still read "land above the
plane" → foam 1, absorption 0.35 → dark foam-washed water instead of the
river's. `App::patchHeightTexture` now re-derives the edited columns from the
runtime store (top solid cell + material, same rule as `storeTerrainTopY`) and
re-uploads just that sub-rect (`vf::uploadSubImage3D`) after every stamp and
after an overlay restore. The same texture feeds the splat shadow/AO marches
(`softShadowSplat`, `splatAO`, `splatSceneDist`) and the SVO's submerged-bed
tint, so they follow edits too.

Verification (`tests/live_edit_check.py`): a channel carved at the shore must
match the open river's water colour on screen — measured mean (91,113,125) vs
(93,113,124) in the splat backend and (87,111,122) vs (89,112,123) in SVO.

## History / why this is a gotcha

The old design built water splats only where the *baked* terrain was below the
plane and patched them per edit (`floodNewlyDug` with a "covered column stays
dry" cap test). Two bugs compounded there: `glm::rotation(up, axisDir)` is
degenerate for an antiparallel axis (the carve kept a one-cell roof) and the
flood's cap test used `.solid` (`raw <= 0`), which the bake's `int(d/VOXEL)`
truncation makes true for the surface *skin* cell right above a carve — so
carved channels never flooded. The plane removes the whole class of problem:
coverage is global and static, and only the *shading inputs* need to follow
edits.

See [[entities/live-edit-brush]] for the brush store/GPU path and
[[entities/svo-render]] for the two backends.
