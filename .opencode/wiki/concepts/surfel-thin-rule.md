---
title: Surfel thin-structure rule (and its roof failure mode)
tags: [surfel, splat, coverage, roofs, authoring, gotcha]
sourceRefs: [src/voxel/surfelize.cpp, src/voxel/surfelize.hpp, tools/hamlet_tower_v2.py, tools/cabin_v2.py]
lastReviewed: 2026-10-04
---

# Surfel thin-structure rule

`surfelize.cpp:thinRunsAt`/`thinFootprintAt` decide each surface cell's
footprint. A cell is **thin** when:

1. its longest lattice-axis solid run (probe ±8 cells, `solidRunAlong`) sums to
   **≥ 4** (`kThinMinRun`), and
2. **both** other axes sum to **≤ 3** (`kThinMaxCross`).

Thin cells get an elongated ellipse — `along` ≈ 1.6–2.4 cells, `across` =
0.55 (one-cell stems) or 0.75 cells (faces 2+ cells wide, `kThinAcrossSeal`)
— instead of the round `baseRadius = 1.4 * VOXEL` disk. The rule exists for
grass blades, reed stems, posts, branches and thin plates: it keeps their
disks from inflating the silhouette.

## Failure mode: curved / stepped thin shells (roofs)

A **roof cone or steep roof authored as a 2–3 cell shell** is the pathological
case. At many angles the tangent direction aligns with a lattice axis (long
run), while the radial and vertical runs are both ≤ 3 → every such cell is
classified thin. The narrow `across` radius (5.5–7.5 cm) cannot seal the
~10 cm staircase step pitch, so pinholes line up along every step edge.

- In the **splat** backend this reads as a sparse field of disks / a
  semi-transparent roof (the dark hollow or sky shows through the pinholes).
- In **SVO** (`--mode svo`) the same voxels ray-march solid, which is the
  tell that the voxels are fine and the footprint heuristic is the problem.

Measured on the hamlet tower (`hamlet_tower.vxw`), sampling cone surface
cells at several heights/angles with the same run rule:

| cone build | thin-classified surface cells |
|---|---|
| 2.6-cell shell (first version) | ~50 % |
| solid cone (`r <= r_o(dy)`) | 3 % (apex only, hidden by the finial) |

The cabin's 3-cell roof skin hit the same rule (~43 % of roof-zone cells);
a 5-cell skin (with a plank ceiling on its underside) drops it to 2 %.

## Authoring rule

Author roofs/domes/cones as **solid masses** (fill the volume under the
surface), or at least ≥ 4 cells across the thin direction, so the radial run
exceeds `kThinMaxCross` and the thin rule cannot fire. Solid also keeps the
VoxelField flood-fill happy (a watertight component fills solid; a leaky
shell renders hollow — see the `tree_gen.cpp` note in the skill).

## Diagnostic (CPU-only, no GPU)

Replicate the run rule on the `.vxw` cells with Python:

```
for each surface cell: runs = solidRunAlong(±x, ±y, ±z, cap 8)
best = argmax(run[a-]+run[a+]); thin = sum[best] >= 4 and all other sums <= 3
```

`tools/hamlet_tower_v2.py` / `tools/cabin_v2.py` are the rebuilds that
applied this; both keep their content in the layer's original absolute
lattice box so the manifest placement is preserved.

Cross-refs: [[concepts/surfel-holes]] (the buried-cell sibling bug),
[[concepts/edge-aware-surfel-radius]] (hard-edge shrink + crease bridges),
[[concepts/detail-pipeline]] (the per-chunk layout),
[[entities/hamlet-scene]] (the objects this was measured on).
