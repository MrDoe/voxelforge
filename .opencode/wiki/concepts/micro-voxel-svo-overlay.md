---
title: Micro-voxel objects as separate SVO worlds (2026-10-20)
tags: [micro-voxel, svo, sdf, encoding, shader, separate-lattice, overlay]
sourceRefs: [src/voxel/micro_field.hpp, src/voxel/micro_field.cpp, src/voxel/micro_svo.hpp, src/voxel/micro_svo.cpp, src/render/micro_svo_pass.hpp, src/render/micro_svo_pass.cpp, src/app/world/micro_objects.cpp, shaders/micro_overlay.comp, tools/micro_voxel.cpp, tests/test_micro.cpp]
lastReviewed: 2026-10-20
---

# Micro-voxel objects as separate SVO worlds

A 30 cm vase at 5 mm voxels cannot ride the world's 0.1 m lattice, and
subdividing that lattice is forbidden (`WORLD/VOXEL/GRID_N/BRICK_N` are
load-bearing). So a micro object gets **its own lattice, its own SVO world and
its own overlay pass**, and the two never share a buffer.

- `role: "micro"` in `assets/world.json` → `App::loadMicroObjects()`
  (`src/app/world/micro_objects.cpp`) → a `MicroField` (dense fine lattice) →
  a `MicroWorld` (its own SVO) → `MicroSvoPass` → `shaders/micro_overlay.comp`.
- The world path never sees it: no `VoxelField`, no `ChunkStore`, no SVO pool,
  no live-edit overlay. A carve through the base world cannot disturb it, and
  `layered_world.cpp` refuses to resample a finer-than-world layer onto the
  world lattice (a silent downsample was the previous behaviour).
- The overlay composites into the main backend's **existing** G-buffer
  (`hdr`/`gpos`/`gnorm`) and overwrites a pixel only where a micro surface is
  nearer — so the world/micro boundary falls out of a distance comparison and
  no depth attachment or water-pass edit is needed.
- `shaders/micro_overlay.comp` **duplicates** the octree traversal rather than
  parameterising `common_svo.glsl`: the 0.1 m path must not be able to regress
  because of a feature that is off by default, and the codebase keeps
  bit-exact escape hatches (`VF_TEXTURES=0`, `VF_RENDER_FLAGS=255`) for
  exactly that reason. The cost is that the copy will drift; the shared
  lighting terms still come from `common_base.glsl`, so a vase is lit like
  everything else.

## The encoding contract — the hard-won part

**A micro field's distance is consumed by a sphere tracer, not by a DDA, so it
must not be stored the way the world stores it.**

The world bake stores the SDF as an **int8 count of whole voxels**. That is
correct there: the world raymarcher steps cell to cell and tests the byte's
SIGN, so its hit IS a cell and the cell index is always right. A sphere tracer
needs a real distance, and whole-voxel truncation destroys exactly that:

- the BFS puts the surface at ±0.5 voxel from a cell centre, so the outermost
  SOLID cell (−0.5 → `int` → 0) and the adjacent AIR cell (+0.5 → 0) **both
  store byte 0**;
- the byte field is therefore flat zero across a two-cell band straddling the
  surface, and the trilinear stencil returns zero across that whole band;
- the trace's zero crossing lands at the band's edge instead of on the
  surface, and the material lookup reads an **air** cell — black albedo,
  material 0 (grass). Measured: the vase rendered at `(18,18,18)`.

Three fixes, in dependency order:

1. **Eighth-voxel quantisation** (`kMicroSdfQuantum = 8`). The same int8 byte
   at 8× the resolution, so −0.5 and +0.5 voxel are −4 and +4 and the field is
   a faithful signed distance with a real zero crossing on the surface.
2. **A dense per-world field array** (`MicroWorld::field`, one byte per cell),
   not the bricks. The bricks also carry a per-cell distance, but the
   trilinear stencil reaches one cell past the sample point and a brick can
   only be indexed inside itself — so the stencil has to be clamped to the
   brick and the field **jumps** at every brick boundary (measured: +0.5 voxel
   to −1.0 voxel in one step, with no zero crossing between). The tracer's
   bisection then converges to a discontinuity instead of to the surface. A
   dense array has no such boundary. It is affordable precisely because a
   micro object is small: the vase is 25×68×25 = 42 k cells = 42 KB.
3. **An air margin** (`kMicroMarginCells = 4`) around the object inside the
   grid. The world AABB *is* the grid, and outside it the shader falls back to
   the distance to the box, which is **zero on the box face** — so a trace that
   treats "within eps of the surface" as a hit converges onto the box and the
   object renders as **its own bounding box** (a flat slab) with the material
   lookup on air. The margin keeps the object's real surface away from the
   face the tracer converges onto. The hit test is "strictly inside"
   (`d < 0`), because the field is negative only inside the object.

With the field right, the material lookup is a plain cell lookup: the cell
**containing** the hit (`floor((p - mn) / cellSz)`), *not* the
`trunc((p - mn) / cellSz - 0.5)` cell-**centre** convention the field
stencil uses — at a face that lands on the air side. If the containing cell
is not solid (a diagonal surface puts the crossing on a cell *corner*), the
3×3×3 neighbourhood is searched for the cell whose centre is nearest the
surface, i.e. the smallest `|field|` among the negative ones.

Two more things the field needs to be *correct*, not just continuous:

- The BFS walks **26 neighbours**, not 6. A 6-neighbour walk measures the
  Manhattan distance, which **over**-estimates the Euclidean one (up to √3),
  so a tracer stepping `0.85·d` could jump clean through a thin diagonal
  wall. The Chebyshev distance is never larger than the Euclidean one, so the
  step is always conservative.
- The out-of-grid **filler** cell carries a conservative distance
  (margin − 0.5 voxels), not the maximum: the stencil reaches one cell past
  the sample point, and a maximum-distance filler next to a real edge cell
  would explode the interpolated field into a huge (and tunnelling) step.

## Measure, don't theorise

The bug survived ~30 render-patch cycles because the diagnosis was done by
patching the GPU and looking at a mean colour. Two things fixed it:

1. **A byte-exact CPU replica of the shader** (field sampling, trace,
   material lookup) that prints the field along a ray. It showed the
   discontinuity directly: `+0.500 … +0.500 → −1.000 … −1.000` across one
   sample, with no zero crossing. The same replica is now
   `tests/test_micro.cpp`.
2. **A numeric probe instead of a caption.** The A/B (micro layer on vs off)
   with an ASCII silhouette showed the object was a **67×67 px square** — its
   bounding box — not a vase. That is what exposed the box-face hit, and a
   vision caption would have said "dark object".

A one-line lesson worth keeping: the failure mode of "the tracer finds the
wrong surface" is *invisible in a mean colour* and obvious in a silhouette.

## The instrument is now a test

`tests/test_micro.cpp` (9 cases) pins: the SDF's sign convention and
half-cell offset, the eighth-voxel quantisation, the margin and the origin
shift that keeps the object's world position, the dense field against the CPU
field cell for cell, the brick packing, a CPU replica of the trace finding the
object's surface (not the box's), and the field's continuity across a brick
boundary — with a **negative control** (the same measurement against a
brick-clamped stencil) that proves the continuity check can detect the bug it
guards.

## Consequences of the separation, stated plainly

- Micro objects self-shadow and shadow each other exactly. They do **not**
  receive the main world's shadows and the main world does **not** receive
  theirs: those marches live in the world shaders and by design know nothing
  about this pass. See [[concepts/measurement-discipline]] for why the
  decision was flagged rather than silently made.
- Live edits cannot touch micro geometry: the micro worlds are outside the
  16³ chunk stream entirely.
- The micro pass is a no-op when no micro layer is loaded (`empty()`), so the
  cost with the feature off is a single branch.
