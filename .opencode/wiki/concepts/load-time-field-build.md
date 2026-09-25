---
title: Load-time field build cost (why tests are slow)
tags: [performance, load-time, tests, voxel-field, startup]
sourceRefs: [src/voxel/voxel_field.cpp, src/app/main.cpp]
lastReviewed: 2026-09-18
---

# Load-time field build cost (why tests are slow)

Every `voxelforge` process pays a **~17.6 s world load** before its first
frame; `--shot`, `visual_check`, `live_edit_check` and the app all repeat it.
Measured breakdown on the hamlet world (16-core machine, 480×270 shot):

| phase | time | note |
|---|---|---|
| `VoxelField::build` (component EDT) | **13.2 s** | 72,002 components / 533k object cells |
| SVO synthesis | ~0.6 s | 4.6M records → 10.5k nodes / 36k bricks |
| `rebuildSurfels` | ~1.9 s | 5.0M surfels (2.1M terrain, 1.3M object, 1.6M LOD) |
| splat upload + render + PPM | ~1.5 s | 5.3M surfels incl. the water grid |
| component grouping | 0.2 s | `groupComponents` |
| merge into hash/objVol | ~0 s | serial, but cheap |

So the field build is ~75 % of the load and the load is ~95 % of a test run.

## Why the EDT is bbox-bound, not content-bound

`VoxelField::build` groups object cells into connected components and runs a
padded-bbox signed-distance transform **per component** (~6 passes over the
*padded* box: occupied/argmat assigns, exterior BFS, two Dijkstra passes,
collect). The padding `kPad` (~8 cells) is load-bearing: the stored field
carries an air band (`kStore * VOXEL`), so it cannot simply shrink.

The problem is amortization: most components are single grass/pebble cells, so
their content is ~7 cells while their padded bbox is `(2·kPad+2)³ ≈ 4900`
cells. Measured totals:

- object cells: 533,151
- padded bbox volume: **361,127,914 cells** (~680× inflation!)
- top-5 bboxes: 5.1M, 0.8M, 0.35M, 0.31M, 0.29M cells — the bulk is spread
  across the many mid-size components, not a few monsters.

361M padded cells × ~6 passes ≈ 2 G cell-ops ⇒ ~13 s. The Dijkstras are
already parallel over components (per-thread `Scratch`), so more threads do not
help; the work is simply oversized.

## Fix direction (next session)

**Batch spatially-near components into one processing group** (union bbox):
72k groups → a few k, amortizing the padding without touching the SDF math.
Keep the deterministic merge in component order (the collision rule is
"solid beats air-band, first-wins for solid-vs-solid"). Alternatives: a
narrow-band/sparse SDF pass (bigger rewrite), or a per-component bbox that
tightens to the stored band instead of a fixed `kPad`.

Do **not** shrink `kPad` (it carries the stored SDF air band) and do not
parallelize renders instead — the load already saturates the cores (3
concurrent `--shot` runs took 48 s vs 53 s sequential).

## Test-time consequences

- Trimming render counts helps only linearly (each render is ~18 s, mostly
  this load); parallel renders do not help (see above).
- `ctest -j4` is the only wall-time lever until the load shrinks (the 4 test
  binaries are independent). `live_edit_check.py` is ~6 min for ~20 renders,
  `visual_check.py` ~1 min for 3.
- The render-level checks cannot be replaced by the (fast) unit tests for the
  GPU-patch paths — see [[entities/live-edit-brush]] for the zero-surfel patch
  regression that no store-level test could see.
