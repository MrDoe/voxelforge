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

## Why SDF-in-code is the primary path

Mesh import **does** exist — STL/OBJ via the sidebar's **Mesh** section, the
`vf_mesh2vox` CLI and the `import_mesh` MCP tool
([[entities/mesh-to-voxel]]). It is a *secondary* path, and the reasons below are
why SDF-in-code stays primary rather than why import is unavailable:

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
demand Blender sculpting, the right shape is an *offline* mesh→stamp-table
generator emitting C++ `StampCell` arrays back into `common.hpp`.

> **Correction (2026-10-08): `scene()` no longer exists.** This page previously
> said the generator would keep `scene()` as the single source of truth. That is
> stale — the dense reference raymarcher and **all analytic runtime geometry are
> gone**, and geometry derives *solely* from `.vxw` records via `VoxelField`
> (see `docs/history/rework.md`). Verified: the only remaining mentions of
> `scene()` in `src/` and `tools/` are comments recording its removal.
>
> The corrected framing: `common.hpp` is the **authoring-side** SDF vocabulary,
> not a runtime source of truth. The analytic `houseAt` / `treesAt` /
> `alpacaAt` / `fenceAt` shapes still exist there (19 references) but are
> **test fixtures only** — they are not in the baker, whose object sweeps were
> removed. Runtime placement is manifest-driven; see
> [[concepts/layer-placement]].

The SDF building blocks below were verified still present on 2026-10-08
(`sdCapsule`, `sdEllipsoid`, `sdConeY`, `smin`, `StampCell`/`stampAt`), so the
rest of this page's tooling guidance stands.

## Verification ladder (cheap first)

| Stage | Tool | Cost |
|---|---|---|
| Per layer | `vf_slice` ASCII cross-sections of the baked field (`tools/scene_slice.cpp`) + `--probe` point queries | seconds; no GPU/window |
| Object done | 2-3 `--shot` renders judged via `ascii_view.py` glyph maps/stats (bundled in skill `scripts/`; never vision models) | needs bake + display |
| Done | `ctest --test-dir build` | ~30 s |


**Reading `vf_slice` output — the row order is the trap.** The grid prints
**row 0 as the top of the span and rows descend**; columns ascend. The header
line states this (`cols: … asc, rows: … desc`) and the loop repeats it in a
comment (`tools/scene_slice.cpp:85`). Trimming the output with
`sed -n 'A,Bp'` therefore selects rows from the **top** of the span, which above
the terrain is **empty** — so a perfectly good cross-section reads as a blank
plane. Only ~half the rows of a full-span slice carry content. Read the whole
output, or pass a `--center`/`--span` that brackets the feature, before
concluding the tool is broken. Related: [[concepts/measurement-discipline]].

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
