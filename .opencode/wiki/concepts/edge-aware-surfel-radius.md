---
title: Hard-edge surfel fit and crease bridges
tags: [rendering, surfels, hard-edges, curvature, live-editing, performance]
sourceRefs: [src/voxel/surfelize.hpp, src/voxel/surfelize.cpp, src/voxel/live_editor.hpp, src/voxel/live_editor.cpp, src/render/splat_pass.hpp, src/render/splat_pass.cpp, src/app/main.cpp, tests/test_surfelize.cpp, tests/test_store.cpp, docs/rendering.md, AGENTS.md]
lastReviewed: 2026-09-25
---

# Hard-edge surfel fit and crease bridges

Gaussian surfels are wider than a 10 cm voxel so neighbouring disks overlap and
keep the surface watertight. The old edge response combined normal
disagreement with exposed lattice axes and could therefore tighten smooth or
thin geometry twice (`rChaos` plus `edgeShrink`). That widened rather than
clarified silhouettes and opened coverage holes.

## Current rule

`SurfelParams::edgeShrink` now affects an opaque object parent only when the
cell exposes at least two non-opposite lattice faces. Opposite faces of a thin
plate do not qualify. Normal disagreement can still influence anisotropy, but
it never reduces object coverage. Terrain, foliage, emissive materials, and
explicit thin footprints keep their watertight radius.

A qualifying hard-edge parent becomes isotropic and its diameter is reduced by
`edgeShrink`. Hardened parents do not use the normal-field anisotropy stretch;
the derived bridge carries that elongation.

## Crease interpolation

Each exposed face pair `f0/f1` emits a small bridge:

```text
axis  = normalize(cross(f0, f1))
normal = normalize(f0 + f1)
centre = cellCentre + 0.5 * VOXEL * (f0 + f1)
```

The bridge is elongated along `axis`, narrow across the crease, and inherits
its parent's material, per-cell texture override, baked AO/shadow, bent normal,
and packed owner ID. It performs no additional shadow or AO march. A two-face
edge emits one bridge; a trihedral corner can emit up to three.

## Chunk and live-edit layout

Each chunk is assembled as:

```text
[base parents | edge bridges]
```

The edge segment remains inside the always-on opaque range, so it introduces no
extra draw call. (`VF_MICRO_DIST` and the former third `material micro-surfels`
segment left with the micro removal 2026-10-08 — see
[[concepts/detail-pipeline]] §3.) `SurfelSet::edgeStart`
and `LiveEditor::edgeCountOf()` preserve the CPU split for GPU-seeded live
patches. `ChunkStore` recomputes parents and bridges in a region refresh. Object bridges also ride
along unmerged in LOD1/LOD2.

## Controls

- **Rendering → Sharp-edge fit**: baked strength, `0.00–0.80`, default `0.35`.
- **Rendering → Interpolate crease splats**: bridge emission, default on.
- `VF_EDGE_SHRINK=0..1`: launch strength override.
- `VF_EDGE_FILL=0`: disable bridges while retaining hard-edge fit.
- Either GUI change requests a world/surfel reload.

## Verification and cost

`tests/test_surfelize.cpp` proves that flat interiors and terrain keep their
radius, hard edges tighten, bridge geometry is smaller/tangent-aligned, and
the full stream remains deterministic. `tests/test_store.cpp` pins live segment
refresh and placement outside the bridge range.

On the current hamlet (`VF_EDGE_SHRINK=0.35`, fill on), the bake emitted
20,252 bridges for 20,252 edge parents. The opaque/LOD stream rose from
3,366,598 to 3,427,354 records (+1.8% including both object LOD copies), and
the house-view pre-cull workload rose from 601,162 to 614,178 quads (+2.2%).
Bake time was unchanged within run-to-run noise (about 2.34–2.46 s). The full
screenshot A/B changed 2.0% of pixels by more than 3/255 versus shrink-only.

See also [[concepts/detail-pipeline]], [[concepts/surfel-holes]], and
[[entities/live-edit-brush]].
