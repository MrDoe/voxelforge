---
title: Live-edit surfel parity
tags: [live-edit, surfels, normals, chunk-store, tests]
sourceRefs: [src/voxel/surfelize.cpp, src/voxel/live_editor.cpp, src/voxel/surfelize.hpp, tests/test_store.cpp]
lastReviewed: 2026-09-26
---

# The live/store surfel path must run the bake's normal pipeline, not its own

A live stamp does not edit one surfel. `LiveEditor::stamp` re-derives the whole
edit AABB plus a margin (`kStampMargin = 3`, `kExactStampMargin = 12` for
Smooth) through `buildChunkSurfelsRange`, and `LiveEditor::refreshRegion` drops
every cached parent inside that box and splices the fresh ones back in. So
**every cell in the box is re-derived, including the ones the user did not
touch** — and their new normal comes from the store path's rule, while the
cached neighbours kept the *bake's* rule.

Consequence: **any** difference between the two rules is a visible, unexplained
movement of splats that no brush ever highlighted. The requirement is therefore
not "the edited cells look right" but "re-deriving an untouched cell is a
no-op".

## What the divergence was

`collectChunkCandidates` used to take the normal from a central difference of
`ChunkStore::sampleWorld` at ±0.15 m, with `n = (0,1,0)` as the degenerate
fallback. That field is **byte-quantised and nearest-sampled**
(`sdfRaw * VOXEL`, one lattice cell per sample), so deep inside a thick body
`d(x+2) == d(x-1)` *exactly* and the gradient cancels to zero. The fallback
then handed a fabricated straight-up normal to a wall cell.

Measured on the cabin (`CabinPart1.vxw`, lattice x 544..592, y 510..538,
z 599..649), store-derived normals vs the bake's final normals:

| rule | object surfels >30° off | mean error | fabricated straight-up |
|---|---|---|---|
| central difference (old) | 27.8 % | 22.4° | 1191 |
| exposed-face mean alone | 1.24 % | 3.80° | 0 |
| exposed-face mean + neighbour smoothing | **0.61 %** | **1.06°** | **0** |

End-to-end (`buildChunkSurfelsRange` over whole chunks vs `buildSurfels`, after
the fix): thick cells 6 of 11378 off by >30°, mean 0.57°, zero spurious-up,
zero wrong entry counts.

## The rule, in three stages

Mirrors `buildSurfels` exactly, evaluated on the store's air test
(`sdfRaw > 0`, out-of-lattice = air):

1. **raw** = mean of the outward directions of the exposed faces — `meanNormal`'s
   rule. No exposed face at all = buried = no entry. A cancelling sum stays
   **zero**; it is never replaced by a direction.
2. **expansion** — a cancelling cell, or a *thin corner* of a thin object
   (`nonZero >= 2 && thinCellAt`), becomes one entry per exposed face carrying
   that face's axis normal (`faceEntry`, never smoothed). This is not optional
   decoration: without it a cancelling cell would reach the GPU as a zero
   normal, and with a single invented direction the flat faces of a one-cell
   stem stay uncovered. One cell can therefore own several candidates, so
   `SurfelRange::keys` is no longer one-per-cell, and `buildMicroSurfels` skips
   repeated keys so a cell still yields **one** micro set (as the bake does).
3. **smoothing** (`params.smoothNormals`) — a non-expanded entry averages its
   own normal with the **raw** normals of the face neighbours present in the
   range. If that average cancels, keep the cell's own raw normal rather than
   fabricate one.

The `rawN` field on `SurfelCand` is what makes this possible: stage 3 must read
neighbours' *pre*-smoothing normals, exactly as the bake reads `rawNormals[]`.

## Traps this cost

- **A single exposed face does not pin the normal to that face.** Both paths
  *smooth* with surface neighbours, so a wall cell can legitimately come out
  tilted ~30°. An early version of the guard asserted `dot(n, face) > 0.98` and
  failed on correct code. The invariant is *parity with the bake*, not
  "equals my exposure mean".
- **A guard needs an object chunk.** On terrain the same rule is far more
  forgiving (the field's own `terrainHeightfieldNormals` blend dominates), so a
  terrain-only check cannot see this class of bug at all.
- **The SDF byte 0 is a one-sided quantisation floor.** `int(d/VOXEL)`
  truncation plus `raw == 0` counting as solid (the SVO DDA tests `sdf <= 0`)
  makes a small-positive-distance cell read as *solid* in the store while
  `VoxelField` calls it air — 0.77 % of cabin cells, and never the other way
  round. That is a storage-format property, predates live editing, and must not
  be "fixed" (it would move the DDA's solid test). The guard's *angular*
  comparison therefore skips cells whose 3×3×3 neighbourhood contains an `sdfRaw
  == 0` cell, so it measures the rule and not the floor. The headline
  `fabricatedUp == 0` check stays unconditional.
- **Smoothing neighbourhoods are range-limited.** Stage 3 averages over
  candidates *in the refreshed range*, so a cell on the region's edge smooths
  with fewer neighbours than the bake did. Bounded by the margin, and much
  smaller than a rule mismatch.
- **The suite shares one world.** `testLayeredWorld()` is a process-wide
  singleton and earlier cases stamp into it. A new case that edits it must
  `lw.invalidateStore()` on the way *in* (to get the pristine baked pools) and
  again on the way *out* (so the next case is not handed this case's edits), or
  it becomes order-dependent — which is exactly how the first version failed:
  green alone, red in `unit_store_tests`.

## Gate

`tests/test_store.cpp` → "live surfel normals follow the bake, never a
fabricated up". Two halves:

1. **parity** — a full-chunk store range against `buildSurfels` of the same
   field: no zero normals, `fabricatedUp == 0` (a disk pointing straight up
   where the bake has it elsewhere — the reported symptom as a number), no cell
   >30° off, mean <5°.
2. **live** — stamp one voxel **2 cells out** along the wall's exposed face.
   Two, not one: a cell placed right against the wall would legitimately remove
   that face and change its exposure. Two is still inside the ±3 refresh margin,
   so the wall really is re-derived — proven, not assumed, by checking the new
   cell gained a surfel and the stamp landed in the wall's chunk before
   asserting the wall's normal is bit-identical.

Verified to fail loudly on the pre-fix code: `fabricatedUp` 40, mean 47.3°,
`grossOff` 3, and the live half reported `dot(wallAfter, wallBefore) == 0`.

## Related

- [[entities/live-edit-brush]] — the brush, undo, overlay, per-voxel mode.
- [[concepts/smooth-terrain-brush]] — why Smooth needs the 12-cell margin.
- [[concepts/measurement-discipline]] — the guard has teeth only because it was
  re-run against the old code.

## A parity gap found by measuring normals (2026-10-03)

The store path was missing the bake's **terrain-heightfield normal stage**
entirely. The bake blends every terrain-top normal 0.55 of the way toward a
two-scale gradient of the height *texture* — `heightAt()` bilinearly samples the
`rg32f` float top field, tapped at `e = 0.35 m` and `e2 = 0.10 m` and mixed
0.55. The store had no equivalent, so terrain shaded from exposed faces alone:
an identical slope looked smooth after a bake and faceted after a live edit.

That is the parity rule being violated by *omission* rather than by a divergent
rule, and it is the version the rule most easily misses — nothing "grew a rule
of its own", a stage was simply absent. Ported as `storeTopAt` +
`storeHeightfieldNormal`, mirroring the bake's construction and reading the
store's own live column tops (the store has no height texture, so the top is
found by a bounded downward scan from the surfel's own cell).

**Measured, on a 1-in-4 staircase ramp** (ideal normal 14.04° off vertical, 576
terrain-top surfels):

| configuration | mean error | worst |
|---|---|---|
| blend OFF (the gap) | 19.09° | 65.16° |
| bake weight 0.55 (ported) | 17.90° | 65.16° |
| weight 1.00 (trust the gradient) | 16.93° | 65.16° |

**The weight is not the limiting factor**, so do not tune it expecting a fix.
Two reasons the residual stays high: the metric partly penalises *correct*
riser normals (a step-edge cell's true surface normal is diagonal, because it
has a riser), and the neighbour pass rebuilds `n` from its own blended value
plus the neighbours' **unblended** `rawN` — parity with the bake, which smooths
`rawNormals[]` — so a 0.55 blend survives at roughly one term in five.

**Where the real fix lives.** A Smooth stamp already computes a *float* relaxed
height before `lround`ing to the lattice. That float field is a continuous
surface, so its gradient is a continuous normal; deriving from the *integer*
tops cannot get there. Stage 2 should therefore be: carry the float
pre-quantisation field for the stamp, derive the normal from its gradient, and
blend it in weighted by the same falloff that moved the geometry (zero weight at
the rim, so unedited surroundings keep the bake's exact normal). It must apply
to the **smoothed region only** — applying it generally would shade the authored
staircase as if it were smooth and change the shipped look everywhere.

Gate for the ported stage: `tests/test_store.cpp` "surfelize: a staircase
ramp's normals need the store heightfield blend" asserts the fixture really
steps (`topOf(20)==20`, `topOf(24)==19`, `topOf(40)==15`) and that the blend
improves, and the sweep is monotone.
