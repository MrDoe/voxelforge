---
title: Wet-surface reflections (stunning pits)
tags: [rendering, reflections, ssr, water, material, photorealism]
sourceRefs:
  - src/voxel/common.hpp
  - shaders/common_base.glsl
  - shaders/common_splat.glsl
  - shaders/ssr.comp
  - src/render/ssr_pass.cpp
  - src/render/texture_atlas.hpp
  - tools/gen_textures.py
  - tests/reflection_check.py
lastReviewed: 2026-11-24
---

# Wet-surface reflections (stunning pits)

A default-off feature set (render-flag **bit 12**, `VF_RENDER_FLAGS=4127` =
31 + 4096) that makes carved pits and damp shorelines read as wet: dark glossy
earth with real reflections. Landed in commit `c0789ed`.

## Why a material, not a water-shader tweak

A material is the unit of texture binding *and* surface response: a textured
material ignores the record colour, so wet earth must be its own slot or a
photo override would darken dry soil too. Wetness is physically a reflectance
change — darkened albedo + low roughness + raised specular — which lives in
the `kMaterialReflection` table, not in the water path.

## What was added

- **Material 22, wet soil** — dark saturated earth (`kPalette[22]` =
  0.24/0.14/0.07), `kMaterialReflection[22]` = refl 150 / rough 55 (soil mat 2
  is 55/225). All mirrored tables bumped 22→23: `kPaletteN` in
  `common.hpp` + `common_base.glsl`, `kPalette`/`kMatRefl`/`matTex` GLSL arrays,
  `TexAtlas::kLayers`, and the `slot < 21` emissive guard → `< 23`. Existing
  material indices are unchanged. `gen_textures.py` gains `TARGETS["wet_soil"]`
  and a `gen_wet_soil` generator so `check_texture.py --material wet_soil`
  validates a drop-in. No `world.json` change is required — with no textures
  entry, mat 22 uses its dark palette colour.
- **Procedural wet terrain** — in `shadeSurfel`, when bit 12 is set, terrain
  (not object/scatter) soil/sand/rock surfels within ~0.55 m above
  `WATER_LEVEL` blend albedo toward wet earth and drop roughness toward mat
  22's. This is shading-only: baked data and the authored scene are untouched.
  It writes the effective roughness into `oGAlb.a`, the guidance lane.
- **Roughness-aware SSR** — `SSRPass` gains a 5th binding for `m_galb` (the
  albedo/roughness lane). `ssr.comp` boosts reflection strength on low-roughness
  (wet) surfaces via a narrow `1 - smoothstep(0.20, 0.32, rough)` gate — narrow
  so sand (~0.75) and rock (~0.59) keep their old reflection, only wet (~0.22)
  moves. Bit 12 off = the historical Fresnel-only amount, bit-exact.
- **Simplified object-volume RT** — `splatReflectObjects` marches the coarse
  256³ `r8_snorm` object volume (`objDist`, ±1.26 m) along the reflected ray,
  returning a dim sky-tinted silhouette on a hit. This is the off-screen
  complement to SSR: a pit shows that the cabin/trees are reflected even when
  they are outside the screen. The coarse volume carries no colour, so it is a
  shape, not a fake detailed reflection. Gated by bit 12.

## Measured (2026-11-19, 640×360, house view)

- default (bit 12 off): **0.000/255** vs committed HEAD — byte-identical, 0 px > 10.
- bit 12 on: mean 15.7/255, effect confined to near-water terrain/water
  (sky top-20% barely changes; it only moves where reflected in water).
- smoke 120 frames: avg 4.23 ms, no errors.

## Depth-aware wall occlusion (2026-11-24)

The original SSR march accepted any G-buffer surface within a fixed 0.3 m of
the reflected point and otherwise returned sky. It had no view-depth test, so
a ray could accept geometry behind an intervening wall, skip a thin wall
between fixed 0.5 m steps, or leak bright sky after leaving the screen.

`shaders/ssr.comp` now starts from a normal-offset origin, skips the source
pixel neighbourhood, and compares reflected-ray view depth with visible
G-buffer view depth at every projected sample. A crossing from in front to
behind is occlusion; the current projected pixel already shows the nearest
occluder and supplies its reflection colour directly, with no refinement
taps. The 32-step march grows from 0.08 m to a 0.5 m cap, matte/face-on
pixels skip SSR, and an off-screen ray keeps the base surface rather than
assuming sky. No extra descriptors or world-space marches were added.

`tests/reflection_check.py` builds a glossy floor, an emissive textured panel
and a two-voxel wall in a temporary scenery. Its floor ROI is strictly in
front of the wall: an earlier full-floor polygon included the wall itself and
misreported legitimate wall SSR as a leak. The gate measures open reflection
vs behind-wall reflection on splat and SVO, checks direct wall occlusion, and
also proves a lights-only manifest edit refreshes the UBO/irradiance volume
without a voxel rebuild.

## Integration notes / gotchas

- The atlas table `matTex[23]` and `slot < 23.0` guard must move together with
  `TexAtlas::kLayers` and the GLSL `kPalette`/`kMatRefl` arrays — a partial bump
  silently reads past the end or drops the new slot (no validation layers here).
- SSR's descriptor layout had to go 4→5 bindings: bump the binding array,
  `bindingCount`, pool size, write loop, and the shader layout together.
- `assets/world.json` was found gutted (157 vs committed 473 lines) by an
  app/test rewrite during this work; restored with `git checkout --`, per
  AGENTS.md. Not part of this feature.
