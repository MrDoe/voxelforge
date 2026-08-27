---
title: Voxelforge PBR shading model
tags: [shading, pbr, ggx, ao, gi, fog, grade]
sourceRefs: [shaders/svo_raymarch.comp, docs/rendering.md]
lastReviewed: 2026-08-26
---

# PBR Shading Model

The lighting stack implemented 2026-08-24 in the single chunked-SVO shader
(`shaders/svo_raymarch.comp`; see [[entities/svo-render]] and `docs/rendering.md`):

## Direct light
- **GGX BRDF**: Fresnel-Schlick `F = f0 + (1-f0)(1-V·H)^5` with
  `f0 = 0.04 + 0.7*refl` (refl from 8-bit brick/`kMatRefl`); Trowbridge-Reitz
  D (`ggxD`) + Smith height-correlated visibility (`smithV`, folded G/(4·N·L·N·V));
  roughness = brick rough clamped to [0.05, 1], `a2 = rough^4`.
- Energy conservation: diffuse scaled by `(1 − avg(F))`; **Albedo-Scale multi-scatter**
  compensation added as `spec * alb * 0.30` (cheap second-bounce surrogacy).

## Ambient / indirect
- **Multi-scale SDF AO** (`sdfAO`): 3 rings (0.14 / 0.45 / 1.25 m) × 4 azimuths on a
  ~0.85*n cone, smoothstep falloff, weight `1/(1+fall²*4)`, accumulated into a **bent normal**
  that drives the ambient (occluded directions drop out of `bent`). Replaced the old 4-dir
  constant-radius `voxelAO`. Cost ≈ +0.5 ms/frame.
- **Sky-driven ambient** (`skyIrradiance`): 3 taps of the analytic `skyColor` model
  (n-tilted-to-up, n-tilted-to-sun, near-zenith), so ambient follows sun azimuth and sky
  gradient instead of constant `mix(kHorizon, kZenith)`.
- **Ground bounce**: `(alb*0.65 + 0.09) * fold * (0.3+0.7*ao)` — albedo-tinted, occlusion-aware.

## Atmosphere
- **Aerial perspective** (`fogColor`): `skyColor(rd)*0.88 + sun-warm pow(cosG,3)*0.10`,
  altitude-attenuated `clamp(0.60 + 0.40*(p.y+1.5)*0.08)`. Kept density 0.0012.
- Water gets the same fog color (previously constant horizon color).

## Micro detail & foliage
- **Two-scale heightfield normals**: fine ε=0.10 m blended 45 % with wide ε=0.35 m — grass
  bumps read up close without schmears.
- **Flora field (2026-08-24, two-layer, LOD 9→26 m via hit distance passed into
  `shadeTerrain`)**: (1) blade-scale micro-grass texture — fbm-warped `vnoise` @ 33 m⁻¹,
  wind-animated with `pc.misc.y`, ~±14 % luminance + normal jitter — fills the gaps between
  tufts and makes mid-fields read as grass; (2) structured tufts (`floraField`, deterministic
  9-cell lookup): grass (matId 0/1) = 4–7 tapered leaning blade segments (0.12–0.40 m,
  ~1.4 cm half-width at base tapering to tip, per-blade azimuth/lean/color hash, dry straw-tip
  blend, dark-root/light-tip height AO via tip² smoothstep); canopy/bushes (matId 8) =
  irregular leaf balls (0.5 m cells, 10–20 cm) with **two-sided** random facet normals
  (flipped toward the viewer) + hue jitter. Per-blade color comes from the blade's own cell
  hash — per-pixel hash alone reads as mush, NOT blades (hard-won lesson).
- **Grass sprite cards (2026-08-24, replaces the blade-tuft layer)**: alpha-tested
  crossed vertical quads, ray-intersected in `shadeTerrain` (`grassCard`/`cardPass`):
  two decorrelated grids — coarse meadow tufts (0.42 m cells, 0.50–0.88 m cards) and
  fine filler (0.17 m cells, 0.14–0.30 m). `tuftAlpha(uv)` = analytic 9-branch pattern
  (tapered wavy blades, wispy tips) = resolution-independent sprite texture; per-card
  hash albedo with dark-root→lit-tip gradient, two-sided up-biased normals + fbm wrinkle,
  cards occlude the ground because `tc < dist`.
- **LOD distance = `length(p - ro)`** (2026-08-24 fix): `t - t0` from `rayAABB` is wrong
  when the camera is inside the world volume (t0 < 0, +~70 m inflation) — the whole grass
  layer used to be silently disabled in every shot. Grass LOD: `smoothstep(12, 40, dist)`.
- **Blade streaks (near-field fill)**: anisotropic world-space noise `vnoise(vec2(p.x*26
  + p.z*13 + wind, p.y*2.9))` (~9:1 vertical stretch, 3–4 cm wide blades, wind-sheared)
  + height-graded AO (dark roots, lit tips) + blade-core brightening `(0.72+0.55*alpha)`.
  The blade pattern is ALSO sampled at the ground-hit point (hit-space) because short
  cards can't occlude steep near-field rays. Isotropic per-pixel noise = mush (hard-won).
- **Specular fireflies**: clamped `smithV ≤ 6.0` (grazing-angle blowup on blades).
- **Grass detail (far fallback)**: 3-octave `fbm` field + `vnoise` blade map shaping albedo
  (0.74..1.19×) with slight tangent-normal jitter (replaces single-hash jitter).
- **Foliage translucency** (matId 8): backlit `pow(1-N·L,2)` × thickness proxy from the SDF
  (`0.5 − sceneMap(p+n·0.3)*2`) — shadow-side canopy glows warm instead of going flat.

## Post
ACES → vibrance (`mix(lum, col, 1 + 0.85*(1 − sat))`) → cool shadow lift + warm split tone →
gamma 2.2. Tuning knobs are plain GLSL constants (push constant is exactly 128 B, no free slots).

## Priorities for the next realism step
Per `docs/history/ImplementationPlan.md`: real 1-bounce GI (half-res temporal), TAA reprojection,
leaf-cluster foliage warp. The SDF AO + bent normal is already the cheapest leg of that.
