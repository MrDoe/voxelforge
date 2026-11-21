---
title: "Code review: stale/unused functions and undocumented parts"
tags: [code-review, dead-code, documentation, maintenance]
sourceRefs: [src/voxel/common.hpp, src/voxel/chunk_index.hpp, src/voxel/surfelize.hpp, src/voxel/world.hpp, src/render/environment_pass.cpp, src/render/environment_pass.hpp, src/app/rhi/surface.cpp, docs/architecture.md, tools/heightmap_gen.cpp]
lastReviewed: 2026-10-25
---

# Code review findings

## Stale / unused functions

### Analytic shape family in `src/voxel/common.hpp`
The baker/test-fixture analytic SDFs are the only remaining "analytic runtime
geometry" (the renderer itself is records-only). Most of that family is now
dead — only `fenceAt` and `alpacaAt` still have callers (in
`tests/test_authoring.cpp`):

- `houseAt`, `treesAt` (+ its helper `treeAt`), `rocksAt`, `bridgeAt`,
  `forestAt`, `docksideAt`, `shoreAt`, `fernsAt` — **zero callers**.
- `bushesAt` and `coniferAt` are only called by `forestAt`, so they die with
  it.
- `inPaddock` is only called by `forestAt`/`bushesAt`.

`fenceAt` and `alpacaAt` are still test fixtures and must stay.

### Unused helpers
- `src/voxel/chunk_index.hpp`: `chunkLocalCoord` — never called (chunk-local
  indexing uses `chunkIndexOf` + `% CHUNK_N` inline).
- `src/voxel/surfelize.hpp`: `surfelPackKey` — never called; only
  `surfelUnpackKey` is used.
- `src/voxel/world.hpp`: `handleIsNode` and `brickIndexOf` — never called;
  `handleIsBrick` and `nodeIndexOf` are the used halves of the same handle
  helpers.

### Dead render pass: `EnvironmentPass` (`src/render/environment_pass.*`)
Initialised in `App::init` (surface.cpp) and given a descriptor pool/pipeline,
but `updateDescriptors`/`record` are never called anywhere and
`App::destroy` does not call `m_envPass.destroy()`. It wraps `env_map.comp`
and reuses `RaymarchPush.misc` lanes (x=face, y=mip, z=res) in a way that
would clobber `misc.y = animTime`. Effectively a stale IBL-cubemap generator
left over from before the records-only rework. Candidate for removal.

### Pass `destroy()` not wired in `App::destroy`
`App::destroy` calls `destroy()` only for `m_svoPass`, `m_splatPass`,
`m_taaPass`, `m_postPass`. The other initialised passes — `m_ssaoPass`,
`m_ssrPass`, `m_volFogPass`, `m_motionBlurPass`, `m_dofPass`,
`m_envPass`, `m_furPass`, `m_microSvoPass` — are never destroyed (minor
Vulkan resource leak on shutdown; OS reclaims on process exit). Not fatal but
should be fixed for cleanliness.

### Misleading default in `tools/heightmap_gen.cpp`
Default `worldOut = "scenery/LakesideHamlet/world.vxw"` is used only to derive the output
directory; the actual manifest is `scenery/LakesideHamlet/world.json`. The default name is a
stale leftover from the removed `world.vxw` merged cache.

## Undocumented parts (now documented)

### Render passes missing from `docs/architecture.md`
The module tour previously listed only `svo_pass` and `taa_pass`. Added rows
for: `splat_pass`, `post_pass`, `ssao_pass`, `ssr_pass`,
`volumetric_fog_pass`, `motion_blur_pass`, `dof_pass`, `micro_svo_pass`,
`fur_pass`, `environment_pass`. See `docs/architecture.md`.

### Known stale comment
`svo_pass.hpp` `RaymarchPush::misc` comment claims `misc.x = animTime`, but
the shader and `App::recordInteractiveFrame` use `misc.y`. See AGENTS.md.

## Suggested follow-ups
- Remove the eight dead analytic shapes (and their only callers) from
  `common.hpp`, or move them to a clearly-labelled `legacy_shapes` header so
  the baker/test fixtures that remain are obvious.
- Delete `EnvironmentPass` + `env_map.comp` + the `env_map.comp.spv` target,
  or wire it up; leaving it half-alive is the worst of both.
- Wire the missing `destroy()` calls in `App::destroy`.
- Correct the `heightmap_gen` default to `scenery/LakesideHamlet/world.json`.
- Document `chunkLocalCoord`/`surfelPackKey`/`handleIsNode`/`brickIndexOf`
  callers or delete them.
---

*Filed by code-review pass 2026-10-25.*