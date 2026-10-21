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

### Bridges only where the parent was actually shrunk (2026-10-09)

A bridge's only job is to fill the hole that reduction opened, so it must not
be emitted where no hole exists. The emission gates therefore key off the
**same predicate as the shrink**, not off exposed faces alone:

- bake (`buildSurfels`): records a per-index `tightened[]` flag from the
  `hardEdge` decision and skips bridges unless it is set;
- live/store (`appendCandidateEdgeBridges`): skips unless
  `cd.thinTall <= 0.0f`, which is that path's form of the same test.

Before this gate, every object cell with two non-opposite exposed faces
bridged — including cells whose parent was *never* reduced (thin footprints,
smooth curvature, foliage). Those parents already covered the crease, so the
bridge was a redundant disk whose 45° crease normal shades darker than the
face it lands on. On a thin beam the result was a vertical **"diamond chain"**
of dark disks.

Measured (2026-10-21, re-verified on the current tree): hamlet bridges
**63,537 → 44,610** records, with `VF_ANISO=0` as the pre-fix control that
restores 63,537 and the default as the fixed path. Cabin 2–3-cell corner posts
(real hard edges) still bridge and still carry ~0.54 L² / 1.14 % px >12 — that
part is load-bearing sealing, not an artifact.

**Measurement trap — the "0 px artifact view" figure previously on this page
was wrong.** It was taken without setting `VF_EDGE_FILL=1`, and `edgeFill`
now defaults **off**, so it compared bridges-off against bridges-off and
reported noise (23 px is the same-binary rerun floor). With fill explicitly
forced on, the same artifact view changes **3.02 %** of pixels (480×270) and a
cabin-wall close-up changes **4.49 %** (960×540). Those deltas are dominated
by a few large contiguous regions rather than a chain of small blobs, and the
frame mean luma is flat, so they read as coverage being restored rather than
dark disks being added — but they are **not zero**, and any future fill A/B
must set `VF_EDGE_FILL=1`/`VF_CORNER_FILL=1` explicitly or it measures
nothing. The geometry-level record count is the reliable claim; the pixel
claims on this page prior to 2026-10-21 should not be quoted.

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
- **Rendering → Interpolate crease splats**: bridge emission, default **off**.
- **Rendering → Crease splat size**: `edgeBridgeSize`, `0.00–2.00`, default `1.00`.
- **Rendering → Fill corner splats**: `cornerFill`, default **off**.
- `VF_EDGE_SHRINK=0..1`: launch strength override.
- `VF_EDGE_FILL=1`: turn crease bridges **on** (they are off by default);
  works independently of the GUI checkbox.
- `VF_EDGE_SIZE=<float>`: bridge/corner size override.
- `VF_CORNER_FILL=1`: turn corner caps **on** (off by default).
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

Two fixtures pin the tightened-only bridge gate: a thick 2×2×2 block emits
8 bridges (still a genuine hard edge), while a thin 2×2 × 8 post emits **0**
with thin classification on — and, run again with `anisotropy=false`, emits 32
again, which proves the suppression is the thin rule and not an object-class
gate.

On the current hamlet (`VF_EDGE_SHRINK=0.35`, fill on), the bake emitted
20,252 bridges for 20,252 edge parents. The opaque/LOD stream rose from
3,366,598 to 3,427,354 records (+1.8% including both object LOD copies), and
the house-view pre-cull workload rose from 601,162 to 614,178 quads (+2.2%).
Bake time was unchanged within run-to-run noise (about 2.34–2.46 s). The full
screenshot A/B changed 2.0% of pixels by more than 3/255 versus shrink-only.

## Why both fills are off by default (2026-10-09)

A 2×2 A/B at 960×540 on the hero/house/water views (`VF_EDGE_FILL` ×
`VF_CORNER_FILL`), with a same-settings rerun as the noise floor:

| comparison | hero | house | water | noise floor (house) |
|---|---|---|---|---|
| `cornerFill` off only | 0.0076 | 0.0076 | 0.024 | 0.0088 |
| `edgeFill` off only | 0.128 | 0.575 | 0.059 | 0.0088 |

Mean L2 difference per pixel, in 8-bit codes, against the full-ON frame.
Corner caps are a **genuine no-op on the shipped hamlet** — the coverage
guard leaves ~557 caps among ~3.4 M surfels, and ON-vs-OFF stays inside the
same-binary rerun noise floor (max 6 codes vs 5 for two identical runs). The
guard is live, not dead code: a genuine thin rod emits 8 caps (unit-test
pinned, [[concepts/surfel-holes]]), so the hamlet reading is a property of its
content.

Edge bridges are **not** free, and this is the load-bearing distinction:
turning them off moves 1.24 % of house pixels by >12 codes (mean 0.575 L2,
~60× the noise floor), and the same magnitude at a cabin-corner-post
close-up (0.54 L2 / 1.14 % px >12 / max 132). Those posts are 2–3 cells
across, so the hard-edge shrink *opens a real hole* and the bridge seals it.
Interior thin posts (one-cell cross-section) read 0 px instead — their
parents keep their full footprint and are not shrunk, so bridging them was
pure double coverage. That split is exactly what the tightened-only bridge
gate added the same day produces: bridges seal genuine hard edges, and stop
re-covering thin shells.

All bridge figures above come from measurements taken on the tightened-only
bridge gate, i.e. the post-fix set, not the pre-fix one. The record-count comparison (44,610 hamlet-derived, down from 63,537 pre-gate) is a peer
session's number, not part of this A/B: `./build/voxelforge --selftest --width
320 --height 180` with `VF_NO_OVERLAY=1`, no `--sun`, no `VF_ANISO` override
(so anisotropy on, lodRings off). It is **not** comparable to a run with
lodRings on — this page's pixel deltas were all taken under the app's
default params.

See also [[concepts/detail-pipeline]], [[concepts/surfel-holes]], and
[[entities/live-edit-brush]].
