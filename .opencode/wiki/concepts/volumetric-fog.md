---
title: Volumetric fog (J) — the ray-bounded rewrite
tags: [rendering, fog, atmosphere, post-processing, testing]
sourceRefs: [shaders/volumetric_fog.comp, src/render/volumetric_fog_pass.cpp, src/app/main.cpp, tests/fog_check.py]
lastReviewed: 2026-09-19
---

# Volumetric fog (J)

Low-altitude Rayleigh + Mie in-scatter along the view ray. Toggle **J** /
`VF_VOLFOG`; **off by default**. It composites on the post-tonemap LDR
`m_offscreen` like the other photo passes and keeps a **3-binding** layout
(scene / gpos / out) — deliberately no `common_base.glsl` include, because it
needs no sky model (the sky is already in the scene), no heightfield and no
object volume.

## The three bugs it replaced

1. **`heightAt()` was stubbed to `return 0.0`** and the march ran a *fixed*
   `ro + rd·i·0.5` for 32 steps — 16 m from the camera **regardless of where
   the surface was**. It accumulated extinction through nearby geometry and
   darkened ~42 % of the hero frame below luma 20.
   **Fix**: march only to the G-buffer hit distance. `m_gpos` *is* the
   occluder, so no heightfield is needed — the surface in front of a pixel
   terminates its own ray. Sky pixels march a fixed 140 m for the horizon
   gradient.
2. **`sunDot = max(dot(-rd, sunDir), 0)`** — sign-inverted for forward
   scatter. `pc.sunDir` points **toward** the sun, so it is `dot(rd, sunDir)`.
3. `skyColor` was duplicated dead code (declared, never called) and
   `gRenderFlags` was read but never used. Deleted.

## Model

- **Density**: `kFogBase · exp(-(y - WATER_LEVEL)/kFogHeight)` — valley fog
  pools low — times 3-octave world-locked fbm patchiness with slow wind drift
  from `pc.misc.y`. Never camera-relative, or it swims.
- **In-scatter**: `kHazeCol · kAmbient · mix(0.85, 1.15, phR)` (sky-lit haze,
  Rayleigh-weighted so it cools away from the sun) plus
  `vec3(1.0,0.90,0.72) · phM · kSunGlow` (forward Mie lobe). The **ambient
  term is what makes it aerial perspective instead of a dimming filter**: as
  optical depth grows the in-scatter approaches sky radiance, so distant
  geometry fades *toward* the sky and a dark tree at range lifts.
- **Extinction**: bounded Beer-Lambert, `tr *= exp(-dens·stepLen)`, pure
  scattering so extinction == density.
- 28 steps with a per-pixel dithered first step; `stepLen = tEnd/28` is
  adaptive, so near geometry gets fine steps for free.

Constants live at the top of the shader (`kFogBase` 0.0045 /m ≈ 20 % haze at
50 m, `kFogHeight` 9 m, `kMieG` 0.62, `kAmbient`, `kSunGlow`).

## Measured (hero, 1280×720, vs fog off)

- mean luminance **+3.1 %**; pixels below luma 20 **0.00 % → 0.00 %**
  (the historical regression is gone)
- distant dark geometry **55.3 → 61.7** (lifts — aerial perspective)
- looking toward the sun **+6.1 %** vs away **+1.5 %** (Mie phase, ×4.2)

## Gate

`tests/fog_check.py` (CTest `fog_check`): the three canonical shots × both
backends, asserting the shift is within **−1..+12 %**, **no new pixels below
luma 20**, distant dark geometry **lifts**, and toward-sun in-scatter is
**≥ 1.5×** away. Constants are still `m_volFogEnabled = false` by default —
enable only with the gate green.

Cross-refs: [[concepts/detail-pipeline]] (the other quality layers),
[[concepts/shading-model]] (the forward aerial-perspective term this
complements), [[concepts/ssao-gbuffer]] (the photo-pass chain it sits in).
