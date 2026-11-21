---
title: Grass and foliage — the two-tier system and the path to photoreal
tags: [grass, foliage, flora, vegetation, rendering, photorealism, wind, transmission, textures]
sourceRefs: [shaders/common_base.glsl, shaders/common_splat.glsl, shaders/common_svo.glsl, tools/hamlet_nature.py, tools/gen_textures.py, src/voxel/surfelize.cpp, assets/textures/grass_light.png, assets/textures/grass_dark.png, assets/textures/foliage.png, assets/textures/proc_moss.png]
lastReviewed: 2026-11-18
---

# Grass and foliage

The renderer carries vegetation at **two independent scales**: real voxel
geometry baked into the field (trees, bushes, ferns, grass tufts, flowers,
mushrooms, fallen logs) and a **shading-level micro layer** (`applyFlora` in
`common_base.glsl`, shared by the splat and SVO backends) that turns a flat
grass/foliage surface into blades, tufts and leaf facets without adding
geometry. The two are composited at shade time, so a single grass cell can
carry hundreds of blades.

## Tier 1 — baked geometry (`tools/hamlet_nature.py`)

Authored into `ai_edits`/object layers as `FOLIAGE` (material 8) and
`WOOD`/`SOIL` voxels. Includes:

- **Trees** — pine (layered conifer tiers) and deciduous (stacked canopy
  blobs, branches, tapering trunk, soil mound so slopes never float a trunk),
  plus a dead-wood variant.
- **Shrubs** — `bush` (3 offset blobs + optional berries), `fern` (7
  fronds), `flower_patch`, `mushroom`.
- **Grass tufts** — `grass_tuft`: 3 small columns of 2–3 cells.
- **Fallen logs** with moss on top.

These are real voxels, so they participate in the SVO/splat pipelines, cast
binary sun shadows, and collide. Material 8 (`foliage`) is the marker the
shader uses to trigger the micro layer.

## Tier 2 — shading micro layer (`applyFlora`)

Runs per shaded hit when `gRenderFlags & 4` (detail) is set. Two parts:

### A. Vegetation micro detail (material-agnostic, green detection)

Gated on `alb.g > alb.r * 1.15`. Applies `grassDetail()` (per-pixel noise
perturbation) and a second sun-shadow evaluation, so grass reads as textured
rather than flat. The shadow is `min(sh, shR + 0.15)`.

### B. Flora field (materials ≤1 grass, or 8 foliage)

`floraField()` deterministically hashes a 0.20 m (grass) / 0.50 m (leaves)
grid into tufts and leaf clusters:

- **Grass tufts** — 4–7 blades per tuft, each a tapered segment with a
  hashed azimuth, height (0.12–0.40 m), lean, wind sway (`pc.misc.y`), and a
  normal perpendicular to the blade axis. Width is kept most of the way to
  the tip (a blade that narrows to a point reads as a spike). Roots dark,
  tips lit via `ao *= mix(0.68, 1.08, tip²)`.
- **Leaf clusters** (material 8) — irregular balls with random facet normals
  (two-sided, facing the viewer), height-graded occlusion.
- **Grass cards** — crossed alpha-tested quads per tuft (`tuftAlpha`), ray-
  intersected against the camera ray so cards occlude terrain. Blades arch
  and keep width; tips fade over the last fifth.

`floraColor()` adds per-blade hue/luminance variance (dry straw tips on
grass, hue jitter on leaves).

### Transmission (render-flag bit 11, default OFF)

For materials ≤1, 8, 21: when the surface normal faces away from the sun and
the camera looks toward it, a warm tint `vec3(0.30,0.20,0.08)` is added to
the albedo. This is the "leaves glow when backlit" cue. It is off by default
so shipped gates stay bit-exact. Toggle via the **Rendering → EFFECTS →
Transmission (foliage)** checkbox, the `VF_TRANSMISSION` env override, or
`VF_RENDER_FLAGS` bit 11 (2048).

### Textures

`grass_light` / `grass_dark` / `foliage` / `moss` atlas layers (512²,
triplanar, `gen_grass`/`gen_foliage` in `gen_textures.py`), with detail
normals (bit 7) adding a Sobel-of-luminance bump.

## Levers for "more real" (ranked)

1. **Enable bit 11 (transmission)** — biggest single photographic cue for
   grass/foliage; already implemented, just off by default. A/B with
   `VF_RENDER_FLAGS`.
2. **Roughness variation from the texture** — sample `foliage.png`/`grass`
   luminance into `vMat.z` instead of a constant per material (see
   [[concepts/photoreal-neural-postprocess]] item 3).
3. **Subsurface scattering on leaves** — replace the backlit tint with a real
   wrap-diffuse + thickness integral.
4. **Better grass cards** — photo alpha atlases + LOD cross-fade (cards
   currently fade by `smoothstep(12,40,m)`).
5. **Per-blade self-shadowing within a tuft** — baked sun shadow is binary;
   grass reads flat without intra-tuft occlusion.
6. **Stronger root darkening / ground contact** — `ao *= mix(0.55,1.05,hgt)`
   already does this; can be pushed.

## Cross-refs

[[concepts/photoreal-neural-postprocess]] (transmission + GI + roughness
ranking), [[concepts/detail-normals]] (Sobel bump), [[concepts/texture-atlas]]
(binding), [[concepts/texture-resolution]] (how much resolution buys),
[[concepts/enclosed-space-lighting]] (cave/interior lighting of foliage).

