---
title: Voxel object authoring workflow
tags: [authoring, sdf, workflow, tools, verification]
sourceRefs: [src/voxel/common.hpp, tools/scene_slice.cpp, tests/test_authoring.cpp, .opencode/skills/voxel-object/SKILL.md]
lastReviewed: 2026-08-24
---

# Voxel object authoring

How detailed world objects get built in voxelforge: **SDF-in-code, one layer
at a time, CPU-checked per layer, render-checked once at the end.** The
operational checklist lives in the `voxel-object` skill
(`.opencode/skills/voxel-object/SKILL.md`); this page records the why and the
tool landscape.

## Why SDF-in-code (no mesh import)

- Materials are first-class: every primitive returns `ObjHit{d, mat}`, which
  flows into brick packing, `kMaterialReflection`, and shading. STL/OBJ
  has no materials - an importer would need a sidecar convention.
- Rendered detail caps at `VOXEL = 0.1 m`; imported high-poly would just buy
  aliasing.
- Parametric repetition, hash variation, ground-hugging placement, and
  deterministic diffs are natural in code; binary assets are none of those.
- The bake sweeps these shapes into `.vxw` record layers, and everything
  downstream (`VoxelField`, SVO synthesis, probe, tests) consumes records.

A converter was considered and rejected for now; if organic hero assets ever
demand Blender sculpting, the right shape is an *offline* mesh->stamp-table
generator emitting C++ `StampCell` arrays back into `common.hpp`, keeping the
bake sweeps the single source of truth.

A converter was considered and rejected for now; if organic hero assets ever
demand Blender sculpting, the right shape is an *offline* mesh->stamp-table
generator emitting C++ `StampCell` arrays back into `common.hpp`, keeping
`scene()` the single source of truth.

## Verification ladder (cheap first)

| Stage | Tool | Cost |
|---|---|---|
| Per layer | `vf_slice` ASCII cross-sections of the baked field (`tools/scene_slice.cpp`) + `--probe` point queries | seconds; no GPU/window |
| Object done | 2-3 `--shot` renders judged via `ascii_view.py` glyph maps/stats (bundled in skill `scripts/`; never vision models) | needs bake + display |
| Done | `ctest --test-dir build` | ~30 s |

Key insight: `--probe` exits before Vulkan init (see `src/app/main.cpp`,
`App::run`) and `vf_slice` reads the baked field directly, so the whole layer
iteration loop runs without any render pass. Renders stay reserved for what
only they can catch: shader-side integration bugs (the LOD t0 incident hid
entire grass layers while CPU truth was fine), palette/shading under sun+fog+
ACES, and visual_check regression thresholds.

## Building blocks added 2026-08-24

`src/voxel/common.hpp`: `sdCapsule`, `sdEllipsoid` (IQ approximation with an
exact-at-center guard returning `-min(r)`), `sdConeY`, `smin`, and the
`StampCell`/`stampAt` voxel-stamp helper (dense bucket index cached per cells
pointer; exact cube distance near cells, conservative underestimate outside
the AABB, `+VOXEL` in interior pockets). Unit coverage:
`tests/test_authoring.cpp`.

Bake-band constraint: tall objects placed outside existing radii must be
added to `nearObject()` in `tools/heightmap_gen.cpp`, or their above-ground
parts never reach the baked voxel records (and thus never reach the SVO).
See skill.

## Cross-references

- Render side of the contract: [[entities/svo-render]]
- How object surfaces shade: [[concepts/shading-model]]
