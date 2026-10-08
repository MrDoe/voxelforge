---
title: Hard-edge surfel fit and crease bridges
tags: [rendering, surfels, hard-edges, curvature, live-editing, performance]
sourceRefs: [src/voxel/surfelize.hpp, src/voxel/surfelize.cpp, src/voxel/live_editor.hpp, src/voxel/live_editor.cpp, src/render/splat_pass.hpp, src/render/splat_pass.cpp, src/app/main.cpp, tests/test_surfelize.cpp, tests/test_store.cpp, docs/rendering.md, AGENTS.md]
lastReviewed: 2026-10-08
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

## Corner fill (coverage-gated)

`SurfelParams::cornerFill` (default **ON**) adds small isotropic caps at
tri-face corner vertices (three mutually orthogonal exposed faces;
`edgeInfoFromMask` detects up to 8 corners per cell).

Caps are **coverage-gated, not angle-gated**: a cap fires only when the corner
vertex projects **outside** the parent disk footprint — the parent is a flat
Gaussian at `centre + n*0.5*VOXEL` with in-plane radius `max(rU, rV)`, and the
corner sits 0.037–0.13 m out along the diagonal. A normal 0.14 m parent covers
that in every direction, so **convex and multi-exposed cells emit nothing**;
only thin-shell / narrow-disk parents (which genuinely miss the corner) and
concave corners get caps.

Why not an angle guard (`dot(parentNormal, cornerDir) > 0.99`): it is a proxy
that leaks. On the hamlet it kept 17,705 caps on covered multi-exposed cells
whose diagonal normal has worse ndl than the wall they land on — 401 px
darkened >20 luma (dark specks on bright walls). The coverage guard dropped
caps to **557** and darkened>20 to **17** (measured 960×540, hero view; frame
mean luma unchanged), with the ~37 brightened px being the intended fill.

Caps inherit parent shading/owner and ride in the `[base | edge]` region
alongside bridges (no extra draw call). On healthy content the count is ~0.

## Size control

`SurfelParams::edgeBridgeSize` scales both bridge and corner radii
(`rV`, `rU` multiplied then **re-floored** so a size < 1 cannot breach the
pinhole threshold). `1.0` preserves the historical footprint. Measured
house-view A/B `1.0` vs `2.0`: 14.9% of bytes changed, mean 1.32/255 — a real
but modest silhouette effect.

## Controls

- **Rendering → Sharp-edge fit**: baked strength, `0.00–0.80`, default `0.35`.
- **Rendering → Interpolate crease splats**: bridge emission, default on.
- **Rendering → Crease splat size**: `edgeBridgeSize`, `0.00–2.00`, default `1.00`.
- **Rendering → Fill corner splats**: `cornerFill`, default on.
- `VF_EDGE_SHRINK=0..1`: launch strength override.
- `VF_EDGE_FILL=0`: disable bridges while retaining hard-edge fit.
- `VF_EDGE_SIZE=<float>`: bridge/corner size override.
- `VF_CORNER_FILL=0`: disable corner caps.
- Either GUI change requests a world/surfel reload.

## Verification and cost

`tests/test_surfelize.cpp` proves that flat interiors and terrain keep their
radius, hard edges tighten, bridge geometry is smaller/tangent-aligned, and
the full stream remains deterministic. The 5% always-on edge-geometry gate is
scoped to **bridges** (`cornerFill=false`) so it stays exact; a separate
`cornerFill=true` bound keeps the combined edge geometry under 10%. A
lone-voxel + `edgeShrink=0.8` fixture proves caps fire (shrunk face disks miss
their own corners: 1 bridge → 9 splats) while a convex `cubeField` stays at 0
caps. `tests/test_store.cpp` pins live segment refresh and placement outside
the bridge range.

On the current hamlet (`VF_EDGE_SHRINK=0.35`, fill on), the bake emitted
20,252 bridges for 20,252 edge parents. The opaque/LOD stream rose from
3,366,598 to 3,427,354 records (+1.8% including both object LOD copies), and
the house-view pre-cull workload rose from 601,162 to 614,178 quads (+2.2%).
Bake time was unchanged within run-to-run noise (about 2.34–2.46 s). The full
screenshot A/B changed 2.0% of pixels by more than 3/255 versus shrink-only.

See also [[concepts/detail-pipeline]], [[concepts/surfel-holes]], and
[[entities/live-edit-brush]].
