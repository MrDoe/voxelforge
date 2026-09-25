---
title: Detail pipeline (bake + live path)
tags: [rendering, surfels, micro-detail, live-editing, lod]
sourceRefs: [src/voxel/surfelize.cpp, src/voxel/surfelize.hpp, src/voxel/live_editor.cpp, src/voxel/live_editor.hpp, src/render/splat_pass.cpp, src/render/splat_pass.hpp, shaders/splat.frag, shaders/splat.vert, tests/live_edit_check.py]
lastReviewed: 2026-09-26
---

# Detail pipeline

How much detail the world shows is decided by four stacked layers. They trade
off differently, so keep them separate when tuning.

## 1. Lattice (10 cm)

`VOXEL = 0.1` (`src/voxel/common.hpp`) is the hard geometric limit: terrain
columns and objects live on a 1024³ lattice, chunks are 64³ cells (6.4 m,
`GRID_N=16`). The heightmap source PNG is 2048² (5 cm/texel) but record tops
are stored per 10 cm column (`int16` in `VoxelField`), so sub-column relief
never reaches geometry. See [[entities/svo-render]] for the chunk/brick data
layout.

## 2. Base surfels (soft, ~14 cm)

One isotropic Gaussian disk per outer surface cell, `baseRadius = 1.4·VOXEL`,
σ²=0.5, opacity 0.9 (`SurfelParams`, `splat.frag`). The 1.4-cell footprint is
what makes the surface watertight under source-over; it is also why silhouettes
read soft. Since phase 4 the bake also stretches disks up to 1.6× along the
local crease and stores the in-plane tangent in the 5th surfel vec4
(`tan_aspect`); the vertex shader orthonormalizes it against the normal and
derives a frame when it is zero (isotropic). The optional
`edgeShrink`/`VF_EDGE_SHRINK` control tightens only occupancy-proven hard-edge
object parents; each exposed face pair also emits a smaller tangent-aligned
bridge in the always-on base+edge segment. Bridges are not material micros and
are not distance-culled; see [[concepts/edge-aware-surfel-radius]].

## 3. Micro detail (hash-driven child disks)

`emitMicroSurfelsForCell(x, y, z, baseSurfel, out)` in `surfelize.cpp` is the
single source of truth: 0–3 children per base cell (2–6 cm apparent), spawn
rules per material (meadow grain, pebbles, bark, roof moss/underside seals,
canopy leaflets), offsets/lift/normal-tilt from a `sin`-hash of the lattice
cell. Children inherit the base cell's material and baked shadow/AO/bent, so
they cost no extra marches. Materials 9–15 (emissive) are skipped by design.

- Bake: `SurfelSet.microStart` splits each chunk into base + micro; the renderer
  skips the micro range beyond `VF_MICRO_DIST` (default 20 m ≈ sub-pixel).
- Live path: `LiveEditor` caches base surfels per chunk and `chunkRun()`
  regenerates the micro tail with `buildMicroSurfels(keys, base)` (the same
  emitter). `SplatPass::patchChunkSurfels(..., microOffset)` updates the
  chunk's `m_microStart`, so a brush stamp never loses micro geometry.
  `tests/test_store.cpp` pins determinism and parent proximity;
  `tests/live_edit_check.py` asserts the patched run grows substantially with
  `VF_MICRO` on vs off.
- Cost: stamp latency unchanged (16–21 ms) vs micros off; the extra GPU patch
  time (~2–4 ms) scales with the run size.

## 4. LOD rings + shading detail

Terrain-only merged rings (2×2×2 at 30 m, 4×4×4 at 90 m; objects ride along
unmerged) are baked from the base set and replace the whole chunk draw at
distance; a patched chunk drops its ring until the next full upload. Shading
detail on top: `detailAlbedo` (albedo-only procedural mottling), two-scale
heightfield normals, grass cards near field, TAA. Since 2026-09-19 there is
also a **fragment-level detail normal** derived from the material texture's
luminance (render-flag bit 7, key B) — see [[concepts/detail-normals]] — so
surfaces beyond the micro cull are no longer flat-lit.

## Knobs

`VF_MICRO=0` (launch) / the **`M`** key (runtime — micros are baked into the
surfel stream, so the toggle re-runs the surfelizer via the world-reload path
and stalls briefly; 3.40M -> 2.13M surfels), `VF_MICRO_DIST` (default 20 m),
`VF_LOD=0`, `VF_LOD1`/`VF_LOD2`
(30/90 m), `VF_SPLAT_{SIGMA,OPACITY,RADIUS,EXTENT}`, `VF_SURFEL_{SMOOTH,HFBLEND}`.
See `docs/rendering.md` for the full table.

## Roadmap (decided 2026-09-16)

1. Phase 1 (done): micros survive live edits — shared emitter + `chunkRun` +
   `microOffset`.
2. Phase 2 (done): per-chunk object mask → wider micro distance for object
   chunks (`VF_MICRO_DIST_OBJ` 35 m; hero +1.6 ms).
3. Phase 3 (done): LOD rings retuned to 30/90 m, material-split blocks
   (`VF_LOD_SPLIT=0` restores the majority disk; +0.16/+0.36 % instances,
   1.6 % hero / 2.1 % overview pixels change). A third 8×8×8 ring was tried
   and dropped: the 102 m world has nothing far enough for it to pay off.
4. Phase 4 (done): anisotropic footprints (80 B surfel, 5th vec4
   `tan_aspect`); the bake stretches disks up to 1.6× along the crease
   (structure of the neighbour normals), the fragment already consumes
   `rU`/`rV`; `VF_ANISO=0` disables. Store path shares the rule.
5. Phase 5 (**assessed 2026-09-19 — fails the gate, stays pending**): 5 cm
   lattice (Option A: `CHUNK_N=64` → 3.2 m chunks, `GRID_N=32`). Measured on
   the full hamlet at 10 cm: 3.31 M surfels (265 MB + 13 MB compaction), SVO
   144.9 MB, load **15.1 s** (`voxel_field` 13.7 s + `svo` 14.4 s + surfelize
   1.6 s). Surface cell count scales as 1/cell², so 5 cm multiplies
   everything by ~4: surfels ~1.11 GB, SVO ~0.58 GB → **VRAM +1.5–1.8 GB
   (gate: +1 GB)** and **load ~120 s vs 15 s**. The load regression alone
   makes the test suite unusable (`live_edit_check` already runs ~40
   renders). **Prerequisite: land the component-batching load-time fix**
   ([[concepts/load-time-field-build]]) before revisiting; keep 10 cm as the
   default. Note also that the authored `hamlet_*` objects are 10 cm shells —
   a finer lattice would not add detail to them without re-authoring.
6. Phase 6 (done 2026-09-19): texture detail normals — a Sobel of the material
   albedo's luminance perturbs the shading normal (render-flag bit 7, key B,
   `kDetailRelief` 0.08 ≈ +40 % HF energy). See [[concepts/detail-normals]].
   Also landed the same day: the texture atlas 256² → 512²
   ([[concepts/texture-resolution]] — real but ~10× smaller effect than the
   normals), the [[concepts/texture-conformance]] drop-in gate, water
   [[concepts/water-caustics]], and shoreline [[concepts/world-detail-content]].

## The per-chunk surfel layout contract (three parallel index arrays)

Each chunk's run in the surfel stream is `[base parents | hard-edge bridges |
material micros]`, described by three `GRID_N^3 + 1` arrays. Verified by
reading the producer (`surfelize.cpp`, the two interleave blocks) rather than
inferred from a comment:

- `chunkRange[c]` — start of chunk `c`'s run; `chunkRange[GRID_N^3]` is the
  **total count**. Empty chunks satisfy `range[c] == range[c+1]`.
- `edgeStart[c]` — where the always-on bridges begin (= the base-parent range
  end). Bridges stay in the opaque range, so they cost no extra draw call and
  no extra shadow/AO march; only the micro tail behind them is culled.
- `microStart[c]` — where the micro tail begins; `microStart[GRID_N^3]` is the
  total count. Empty when micro detail is off.

The ordering that actually holds, and that any assertion may rely on:

```
chunkRange[c] <= edgeStart[c] <= microStart[c] <= chunkRange[c+1]
```

with `edgeStart[GRID_N^3] == microStart[GRID_N^3] == total`. A chunk with no
bridges has `edgeStart[c] == chunkRange[c+1]`, so **`edgeStart[c] <=
chunkRange[c]` is false for every chunk that has bridges** — asserting both
`>=` and `<=` against the same value is satisfiable only on equality, which is
the shape of a test bug that reads like a renderer bug. Likewise `edgeStart`
and `microStart` are `+1` arrays whose **last entry is the total, not
`chunkRange[i+1]`**, so a loop over every entry must bound the final index by
the total count or it reads one past the end.

The consumer is deliberately tolerant, which is why a bad producer can hide:
`splat_pass.cpp` clamps the bridge offset into `[start, end]` of the chunk's
slot run before use, so it will not crash on a violated bound — it will just
render the wrong sub-range. See [[concepts/measurement-discipline]] for the
test that pinned this. Cross-refs: [[entities/live-edit-brush]] (live
seed/splice uses `edgeStart`), [[concepts/edge-aware-surfel-radius]] (what
the bridges are).

## Current default world

The scene is now runtime-authored (`hamlet_*` layers, see
[[entities/hamlet-scene]]); the baker emits only the terrain shell and
preserves foreign manifest entries across regens.

Cross-refs: [[entities/live-edit-brush]] (patch path), [[entities/svo-render]]
(load-time geometry), [[concepts/shading-model]] (shading inputs).
