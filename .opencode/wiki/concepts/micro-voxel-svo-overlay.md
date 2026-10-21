---
title: Micro-voxel objects as separate SVO worlds (2026-10-20)
tags: [micro-voxel, svo, sdf, encoding, shader, separate-lattice, overlay]
sourceRefs: [src/voxel/micro_field.hpp, src/voxel/micro_field.cpp, src/voxel/micro_svo.hpp, src/voxel/micro_svo.cpp, src/render/micro_svo_pass.hpp, src/render/micro_svo_pass.cpp, src/app/world/micro_objects.cpp, src/voxel/surfelize.cpp, src/voxel/surfelize.hpp, shaders/micro_overlay.comp, tools/micro_voxel.cpp, tests/test_micro.cpp, tests/micro_check.py]
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

## Shadows: two-way, and coarse on both halves

Micro objects self-shadow and shadow each other exactly, and they participate
in the world's shadow **in both directions** — coarsely, because that is the
only honest way to bridge two lattices that must stay separate.

- **RECEIVE** (`worldShadow` in `micro_overlay.comp`): the micro surface
  marches the two COARSE world representations it already binds — `uHeight`
  (the terrain heightfield) and `uObjVol` (the 256³ coarse object volume). The
  verdict is **binary**, like the SVO reference backend's exact hits and like
  the micro worlds' own `microVisibility`. Do NOT copy `softShadowSplat`'s
  `shadow += 0.5` per unoccluded tap: that returns **0.5 for a fully LIT
  point** (measured: the vase's mean luma 109 → 84 with no occluder anywhere
  near it). It also needs its OWN test — `objDist < 1.2474 && objDist < -0.05`
  — because the coarse volume is the only thing that knows about the house.
  **Cap the step.** Stepping by the distance to the heightfield is cheap, but
  beyond ~2 m the far-field distance saturates at the object volume's 1.26 m
  range, so the step grows to ~1.07 m — wide enough to step clean OVER a
  canopy or a roof. Measured symptom: an alpaca whose body read shadowed and
  whose **legs read sunlit** (shadow-on/off ratio 0.87 vs the body's 0.50),
  because the legs' ray runs lower and the coarse sample lattice straddled the
  occluder — while the ground under the animal, whose shadow is BAKED with a
  fine DDA, disagreed with its legs. 40 taps with a hard 0.35 m cap fixes it.
- **CAST** (`microShadow` in `surfelize.cpp`): the world's sun shadow is
  BAKED per surfel on the CPU from the `VoxelField`, which by design does not
  contain the micro objects, so the bake consults each micro field directly.
  It is one binary verdict per surfel — the coarsest possible, and exactly the
  same shape as every other baked shadow in the project. Two things matter:
  the march is bounded by the object's world AABB and steps at a **quarter of
  the object's own voxel** (the world bake's 0.1 m DDA step is wider than a
  5 mm object and would step straight over it), and it gets its **own ray
  origin** (`pos + n * 0.01`) — reusing the world march's `pos + n * 0.3`
  starts the ray 0.3 m above a 0.3 m-tall object and finds almost nothing
  (measured: 1 surfel darkened instead of ~12).
  The micro objects load BEFORE `rebuildSurfels` for this reason, and the
  live-edit re-bake passes the same occluders so an edit cannot erase the
  shadow.

The cost stays bounded: the object's AABB is centimetres, so only the surfels
beside it ever run the micro march, and with no micro layer loaded the bake is
bit-identical to before (`microOccluders` empty).

**Backend caveat**: the CAST half lives in the SURFEL bake, so it is the splat
backend's. The SVO reference backend computes its shadow per fragment from the
world's own octree, which by design holds no micro cells, so it shows no micro
cast shadow. That is a real (documented) divergence; the reference views
(`hero`/`house`/`water`) contain no micro object, so no gate sees it.

**A pre-existing quirk noticed on the way**: `softShadowSplat` adds `0.5` for
every tap that does NOT hit anything and divides by the tap count, so a fully
LIT water fragment returns 0.5 rather than 1.0 — the sun on water is always
halved. It is calibrated into the water's appearance, so it is documented here
rather than changed.

- Live edits cannot touch micro geometry: the micro worlds are outside the
  16³ chunk stream entirely. (An edit to the WORLD ground beside a micro
  object re-bakes that ground and re-applies the micro cast shadow.)
- The micro pass is a no-op when no micro layer is loaded (`empty()`), so the
  cost with the feature off is a single branch.

## Placement: the terrain is quantised, so "on the ground" is not automatic

The terrain is a 0.1 m lattice, so its surface is **quantised**: a place the
heightmap calls flat can be a 0.1 m staircase in the baked world. An object
therefore only stands flat if it fits inside **one** terrain cell — the vase
(0.085 m wide) at `x = 12.0` spans `[12.0, 12.1]` and at `z = 12.0` spans
`[12.0, 12.1]`, one cell each. The urn (0.095 m) at 12.3 / 12.2 spans
`[12.3, 12.4]` and `[12.2, 12.3]`. Both footprints were **probed at four
corners** (the ground is −0.25 everywhere under them, so neither floats nor
sinks) — the heightmap alone would have said "flat" and been wrong about the
staircase.

The second object exists for the same reason the test does: with one object,
"multiple SVO worlds" and "self-shadow among micro objects" are both
**invisible**. The urn is placed where the vase's shadow falls — the sun is at
34°/238°, so shadows are thrown toward (0.85, 0.53) in xz, and the urn sits
0.37 m away in exactly that direction.

**Measured** with a three-way A/B (both objects / the urn alone / neither), on
the urn's own pixels: 246 of them (10.3 % of the urn) darken by a mean of
**45.5 luma** (114.5 → 69.1) when the vase is present. That is the vase's
shadow falling on the urn — micro-to-micro shadowing, with neither object
knowing anything about the main world.

## Authoring a non-lathe object: the alpaca

`vf_microvox` started as a LATHE (the vase and urn). An animal is not a solid
of revolution, so `makeAlpaca` evaluates an **SDF over the grid** instead — a
union of a body ellipsoid, a tapered neck, a head, a muzzle, ears, four
tapered legs, hooves and a tail — plus a **three-octave value-noise fur
displacement** on the fleece regions. Only the SIGN of the SDF is used (the
app derives its own distance field), so the primitives may be approximate
without any visual cost: that is what makes a hand-written animal cheap.

At 8 mm the alpaca is 1.3 m tall and 150 k voxels on a 57×170×142 lattice
(2.3 MB layer, 6.8 MB of GPU bricks) — 170 levels, versus the vase's 60.

**The texture lesson.** `assets/world.json` binds a texture per MATERIAL, and
a textured material ignores the record's colour, so:

- the fleece needed **its own material** (21, appended — `kPaletteN` 21 → 22
  with the GLSL tables and the atlas `kLayers`; every existing index is
  unchanged) or a fur photo would have turned the plaster vase furry;
- the eyes/hooves ride mat 4 (rock) and the muzzle mat 19 (thatch) precisely
  so they stay crisp and un-furry;
- the fur's `scale` is **0.2 m/tile**: fleece clumps are centimetres. A
  world-scale tile (1 m) would make the animal look like it is wrapped in a
  bedsheet.

A drop-in fur is gated by `tools/check_texture.py --material fleece` (the
`TARGETS` entry keeps the material's cream identity) and by
`micro_check.py`'s **fleece check**: a flat palette fill and a real fur photo
are indistinguishable to every other check, so the gate measures the luma
spread of the alpaca's own pixels.

## A micro object is PICKABLE (and the frame bug that made it invisible)

The app's pick path marches the **world's** store, which by design holds no
micro cells (they are separate SVO worlds on their own fine lattice). A micro
object was therefore invisible to the pointer: clicking it selected nothing, and
the Move mode refused it because it has no world owner id.

`pickMicroField` is the missing half — a sphere-trace of the micro field in
world space, with the hit bisected to the field's zero crossing. Two frame
mistakes of mine, both invisible in a screenshot and obvious in the field
profile:

- the local point was offset **twice** — the field's own origin is applied
  *inside* `sampleSdf`, so subtracting it as well offset the sample twice and
  the ray missed the object by the whole margin;
- the bisection bracket was a fixed step back instead of the **previous
  sample**: the march can step `0.85·d`, which is far more than the minimum
  step, and a bracket that does not contain the crossing makes the hit land
  cells *inside* the object.

**The frame bug that matters most**: `m_hoverHit.voxel` is a **world-lattice**
coordinate — the brush, the box selection and `describeSurfelsAt` all index the
world with it. A micro object lives on its own lattice, so writing its cell
index there sends every one of them to the wrong place *and to the wrong
chunk*. The pick now stores the world cell under the hit
(`worldToVoxel(mp.pos)`) and names the object by its manifest file
(`PickHit::microFile`), since a micro object has no world owner id.

## Live movement: a micro layer can be moved and turned

A micro layer's `pos` is the world position of the object's **min corner** (a
world layer's `pos` is a lattice offset instead), and `rot`/`rotX`/`rotZ` spin
the object about its **bottom-centre pivot** — the same pivot the world uses,
so "yaw about the base" means the same thing in both.

`rotateMicroRecords` (micro_field.cpp) applies the rotation to the layer's
**records on its own fine lattice**, and three details decide whether it
works or produces a 65 k cell mess:

- **Destination-driven**, like the world's `transformRecords`: every cell of
  the rotated footprint queries its pre-image, so the rotated shape keeps the
  source's solidity. A forward map drops cells wherever two source cells
  round to the same destination — holes in the surface.
- **Not bounded by the layer's own grid**: a 45° turn needs a wider footprint
  than the grid the object was authored in, and clipping it would cut the
  animal in half. (The alpaca's 90° yaw sweeps its footprint to x = −35.)
  The records are therefore emitted **rebased** — a record's coordinates are
  `uint16`, so writing a negative cell wraps it to 65535.
- **The pivot stays fixed in world space** while the shape spins about it. The
  rotation moves the shape's min corner, so the world offset is corrected by
  exactly that shift (`minShift` cells). With no rotation the shift is zero
  and the offset is `pos` — unchanged, so the shipped placements are not
  disturbed.

Applying a micro layer's transform re-bakes **only that object**
(milliseconds). The world's Apply still triggers the full world reload — the
separation pays off here.

**Gated**: `tests/test_micro.cpp` pins the contract (no coordinate wraps, the
volume survives, the pivot's world position is unchanged within 1 mm), and
`tests/micro_check.py` pins the end-to-end effect (the footprint's aspect
flips under a 90° yaw: 1.18 → 0.66, while the silhouette's base row does not
move: 559 → 559).

**A lesson about instruments**: both of the gate's first two "did it turn"
measures were wrong, and the code was right. The silhouette's *width* is not a
measure (the camera is diagonal, so a yaw can widen or narrow it: measured
344 → 425 px, the wrong way round), and from directly above the *base row* is
a world x/z, so it measures nothing at all. It takes two views — from above
for the footprint's aspect, from the side for the base row — to measure both
halves of the contract.

## Verification

| check | result |
|---|---|
| `test_micro.cpp` (10 cases) | pass — incl. a negative control for the continuity check and the cast-shadow verdict |
| `micro_check.py` (11 checks) | pass — visibility, material, silhouette, micro-to-micro shadow, RECEIVE, CAST, alpaca visible, fleece texture, world untouched, SVO backend |
| `test-unit` group | 8/8 pass |
| `visual_check.py` | only the pre-existing `house: sky probe not blue-dominant` failure; every other metric is **bit-identical** to before the micro layers existed (coverage 70.8 / 94.6 / 96.2 %), because the objects are not in those views |
| `live_edit_check.py` | exactly the documented pre-existing baseline: 3× `object_smooth` + 1× `object undo`, same 11002/11008 numbers — no regression |
| A/B the house view with the micro layers on vs off | **zero** differing pixels — the overlay provably cannot affect the 0.1 m path |
| the vase's mean colour | (18,18,18) flat dark → **(108,107,105)** bright neutral plaster |
| both backends | the same vase silhouette in `--mode splat` and `--mode svo` |
| RECEIVE (world → micro) | 116 luma with the world shadow off, 94 with it on |
| CAST (micro → world) | the bake reports 7 surfels darkened |
| micro → micro | 349 urn pixels darkened by a mean of 41 luma |
| the alpaca | visible (86 k px), fleece luma spread 24.7 (a flat fill would be ~2) |
| shadow reaches the LEGS | ratio 0.87 → 0.57 after the 0.35 m step cap |
