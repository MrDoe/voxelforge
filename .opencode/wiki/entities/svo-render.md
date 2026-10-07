---
title: Chunked-SVO raymarch
tags: [rendering, svo, raymarch, reference-backend, vulkan, voxel]
sourceRefs: [shaders/svo_raymarch.comp, shaders/common_svo.glsl, shaders/post.comp, src/render/svo_pass.cpp, src/render/svo_pass.hpp, src/voxel/voxel_field.cpp]
lastReviewed: 2026-10-07
---

# Chunked-SVO Raymarch

> Current role: the SVO raymarcher is the **pixel reference** (`--mode svo`,
> `F` toggles it); the default backend is the Gaussian-surfel rasterizer
> (`--mode splat`). It earns that role partly by being the simpler of the two:
> per-surfel baking, the depth prepass, and the whole opaque base/band split
> exist only on the splat path, so the two backends agreeing is real evidence
> and not a tautology.

- `shaders/svo_raymarch.comp` — the compute entry point; the traversal itself
  lives in `shaders/common_svo.glsl` (`exactSVOHit`, `traverseSVONode`,
  `softShadow`, `shadeTerrain`)
- Chunk grid (16³) → per-chunk octree → 8³ bricks. Handles are **chunk-local**:
  every access adds the owning chunk's base from `GpuWorld::chunkInfo`
  (`uvec4` = nodeBase/childBase/brickBase, binding 11). Never offset-adjust a
  handle during the merge in `layered_world.cpp`.
- All geometry is data-derived from `.vxw` records via `VoxelField`:
  - terrain: `uHeight` rg32f texture (R = column top Y, G = material), bilinear
  - objects: brick SDF + material byte (bit 7 = object flag)
- No analytic scene constants in GLSL. Water plane y=-0.9, fog 0.0012.
- Push block `RaymarchPush` (128 B) lives in `src/render/svo_pass.hpp`.
  **`misc.y` is `animTime_s`** — the struct's own comment claims `misc.x`, and
  the shader plus `record_interactive.cpp` both read `misc.y`.

## Two claims this page used to make that are no longer true

Both survived a month of being "reviewed" because the schema bug below meant
nothing surfaced them. They are recorded so nobody re-adds them.

**It is not ACES.** The raymarcher outputs **linear HDR scene radiance
(pre-tonemap)** and the post pass applies exposure, **AgX** tonemapping, bloom
and the selection outline. The `aces()` function in `common_base.glsl` is dead
code on this path — kept only as a helper. See [[concepts/shading-model]].

**SVO shadows do not march `uObjVol`.** `softShadow` is a **16-tap PCF over
`exactSVOHit`** — exact DDA traversal of the sparse octree, giving a penumbra
proportional to distance from the shadow boundary. Terrain uses
`softShadowTerrain`. The coarse `r8_snorm` 256³ object volume is only marched on
the GPU for the **splat water path** now; on the splat side, surface sun
shadows are **baked per-surfel on the CPU** (`shadowMarch` over
`VoxelField::sample`) so both backends reach the same verdict.

## Day / night

The SVO path reads the same single source of truth as the splat path:
`kSunDir` off the push constant, with `applyNight` appended to both
`skyColor` and `skyColorFast`, plus `moonLight(n, ao)` using **AO only, never
the baked sun shadow** (the bake marches the sun, so applying it to
light from the opposite side would put moonlit faces into shadow). The
per-backend parity numbers are on [[concepts/sun-direction-pipeline]].

Cross-links: [[entities/hud-sidebar]], [[concepts/ssao-gbuffer]],
[[concepts/enclosed-space-lighting]].
