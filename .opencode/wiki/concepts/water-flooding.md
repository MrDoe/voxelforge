---
title: Water flooding (removed) — the fixed-level plane replaced it
tags: [water, live-edit, history, gotcha]
sourceRefs: [src/voxel/surfelize.cpp, shaders/common_splat.glsl, src/voxel/editable_world.cpp]
lastReviewed: 2026-11-24
---

There is no flood-fill simulation in VoxelForge. The older per-column water
bookkeeping was removed in the 2026-09-17 water rework; this page exists to
close the historic `[[concepts/water-flooding]]` link and to say why the missing
feature is deliberate rather than an oversight.

The water is **one fixed-level plane** at `WATER_LEVEL = -0.9`, represented in
the splat backend by a world-wide 0.2 m grid of coplanar surfels
(`buildWaterSurfels`). The shader intersects the analytic plane per fragment;
the grid is coverage only. A depth test against the opaque prepass hides the
plane over dry land and objects, so carving a pit below the level exposes water
immediately with no per-cell state to update. The SVO reference uses the same
analytic plane.

What *must* follow an edit is the height texture the water shading reads for
bed depth, shore foam and absorption: `App::patchHeightTexture` re-derives the
edited columns and re-uploads that sub-rect after every stamp or overlay
restore. Without it, a freshly dug channel shades as thin foam-washed land
instead of water — see [[concepts/water-plane]] for the two measured bugs and
[[entities/live-edit-brush]] for the stamp/patch path.

Consequence for authoring: a "flooded cave" is a cave whose floor sits below
`-0.9`, not a region marked wet. A dry interior needs its floor above the
plane; sealed air is solid in the bake, so an enclosed pocket cannot silently
fill.
