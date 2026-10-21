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

### The pick still missed: the CPU sampler had no out-of-grid fallback

With the wiring right, the pick *still* never hit anything — and this is the
part worth remembering, because **every layer of it looked correct** and a
screenshot could never show it.

`MicroField::sampleSdf` is a trilinear sample of the dense field. Outside the
field's grid there is no cell in range, the accumulator stays at **0**, and the
sampler returns 0. A sphere tracer that reads 0 **steps 0**: the pick march
collapsed to its minimum step (a quarter of the object's own voxel) and
covered about **0.6 m** in its entire 512-step budget. The camera is 15 m from
the shore, so the ray never arrived. Measured: **0 of 81 rays** hit the real
alpaca — while the AABB test passed on every one of them, because the *box*
was found and the *field* was not.

The GPU never had this bug: `microMapWorld` in `shaders/micro_overlay.comp`
falls back to **the true distance to the world's box** outside it, precisely
because a flat constant is fatal for a sphere tracer (the shader says so in a
comment). The CPU sampler had no such branch. The fix mirrors the shader
exactly — outside the grid, return the distance to the grid box, with a floor
so the step stays positive on the face. The box *contains* the object, so the
box distance is a lower bound on the object distance and `0.85 ×` it is a safe
step.

**Why the unit test did not catch it**: the pick test cast its ray from
**0.30 m** away. The crawl reaches 0.6 m, so it *arrived* and the test passed.
The app's camera is 15 m away, so it did not. The regression test now casts
from **15 m** — the real distance — and pins the out-of-grid distance as
positive and growing.

**How it is verified without a mouse**: `VF_TEST_MICRO_PICK=1` makes the app
run *its own* `pickMicroObjects` from the camera toward every micro object and
report the result (`3/3 micro objects selectable`). It is a headless hook, so
the check needs no pointer and cannot disturb a user's cursor — and it
exercises the real path (the occluder list, the AABB, the field), not a
re-implementation. `tests/micro_check.py` asserts it.

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

### A micro object is manipulable with the MOUSE (three gates that refused it)

The panel's inspector was not enough: the object had to be draggable and
turnable with the pointer, like any other object in the scene. Three gates
refused it, and all three refused it for the same reason — **a micro object
has no world owner id**, and the world's code asks for that id to decide
whether a layer is real:

| gate | what it asked | the fix |
|---|---|---|
| the plain click that activates an object | `m_layers.layerId(picked) != 0` | also accept `isMicroLayer(picked)` |
| the Move axis handles | `layerId != 0` | also accept a micro layer — the bounds already came from `layerBounds()`, which is micro-aware |
| the Rotate trackball | `m_layers.layerBox()` (the *world* box — empty for a micro layer) **and** `layerId != 0` | use `layerBounds()` and accept a micro layer |

The activation gate is the one the user notices: with it closed, a plain click
on the alpaca selected **nothing at all** — no trackball, no owner, and the
Move mode had no handles to grab.

**The rotation's pivot** also had to become micro-aware (`App::layerPivotWorld`):
the world's `layerPivot` cannot know a layer that is not in the merged world.
The micro pivot is the bottom-centre of the object's **records** — the field's
grid starts `kMicroMarginCells` before the first cell, so the records' min
corner is the grid's origin plus the margin, and the records' extent is the
grid's extent minus it. Same convention as the world (centre in x/z, base in
y), so "yaw about the base" means the same thing for both.

### The live rotation preview lives in the MICRO pass

The splat pass's rotate preview is keyed by the world owner id
(`surfelBelongsToLayer` refuses id 0), so it cannot show a micro object: a
micro layer has no owner id, so the preview matched **nothing** and the object
sat still while the ring turned.

The micro pass therefore carries its own: the world table's row gains a
rotation matrix and a pivot, and the shader applies it **in the field's
frame**. The trick that keeps the change small: the rotation is applied at the
**one place the field is sampled** — `microMapWorld` maps the world point
into the object's baked frame, reads the field there, and returns a distance
in the world frame (a rotation preserves distances, so the value needs no
correction and the trace's step stays conservative). Everything downstream —
the union field, the gradient (and so the normal), the AO, the self-shadow —
then works in the world frame and needed **no change at all**. Only the
material lookup had to be told, because the octree descent and the cell that
owns the surface live in the baked frame.

The matrix is the app's own: the **old pose to the new pose**, about the same
pivot, exactly like the splat pass's preview. The shader applies its
transpose to go the other way (world → baked).

**Verified headlessly**: `VF_TEST_ROTATE_LIVE="90,0,0"` with
`VF_ROTATE_LAYER=micro/alpaca.vxw` turns the alpaca on screen without
writing the manifest, and `tests/micro_check.py` measures it (the preview
changes the frame, and the object is still there).

### The selection was invisible — and the interactive path had never been tested

The user reported "selection still doesn't work", and the app **was**
selecting: the pick fired and the activation fired. What was missing was the
part a user actually looks at, and it was missing in three places at once:

- **The Move axis handles were never drawn.** The overlay asked
  `m_layers.layerBox()` — the **world's** box, which is empty for a layer
  that is not in the merged world — so a micro object could be grabbed by
  the maths and showed nothing to grab. `layerBounds()` is micro-aware ✓.
- **The selection outline floated in the air next to the object.** The feed
  was the **world cell** under the hit; a micro object lives on its own 5 mm
  lattice, so that cell can be a metre from the surface. The hit **point**
  is fed now, so the box lands on the object.
- **The label said "Selected voxel"** — a micro object is not a voxel. It
  now names the selected layer (the World panel's row lights up too).

**The real lesson is the instrument.** Every hook written earlier proved
that `pickMicroObjects()` finds an object **in isolation**. The
screen→ray conversion, the store-vs-micro competition and the gates around
the selection were never measured — so a selection that failed in the app
could still pass the gate. Three changes follow:

1. The pick is now **one method** (`App::pickAtWindowPos`) that
   `processInput` uses and a test can call ✓ — a test measures what the
   pointer does, not half of it.
2. `App::m_testPointer` injects a window position and a button state into
   `processInput` ✓, so the **real** path (the text-field gate, the mouse
   capture, the ctrl/armed gates, the selection) can be driven headlessly.
   The user's rule is that a test must not take the pointer ✓, and this is
   how the interactive path becomes testable at all ✓.
3. `VF_TEST_CLICK_SCAN` finds the pixels where a micro object wins the real
   pick and clicks them, through **both** gestures: the Ctrl+LMB pick and
   the plain click that activates an object in the Rotate mode ✓.
   `tests/micro_check.py` asserts both ✓.

Measured: **12/12 picks and 12/12 activations**, every gate clear
(`wantMouse=0 textCaptures=0`).

**And the thing the user was actually fighting**: from the app's **default**
view the alpaca is a ~28×10 px sliver 15 m away — three hits on a 12 px scan
grid ✓. A one-pixel miss picks the ground, and the ground is a *different*
layer, so the selection "does nothing". Up close the same object is 580 px ✓.
**A micro object is selectable; it is small** — and the app's default camera
is a house view, not an object view ✓.

### The rig is DERIVED, cached, and generic

The skeleton is **not authored**. Nothing in the code knows what an alpaca is:
the bones are found in the object's own voxels, so a **newly imported model
animates the moment it is loaded** — no per-object code, no per-object asset.

The derivation (`src/voxel/micro_rig.cpp`) is generic and geometric:

- the **spine** is the longer of the two *horizontal* axes — and it must not be
  "the longest axis": an animal is usually **taller** than it is long (the
  alpaca is 1.3 m tall and 1.1 m long), so the longest axis is the vertical
  one and the whole derivation finds no legs at all (measured: 3 bones, 0 legs);
- the **belly** is a *percentile* of the band's cell heights, not the band's
  minimum: a quadruped's front legs stand *inside* the spine's band, so the
  minimum is the hooves and nothing is "below the core" (the legs would be
  part of the body);
- the **legs** are the vertical clusters below the belly, merged by their
  column size;
- the **head** and the **tail** are the mass beyond the core's two ends.

A bone is a **segment** plus its role, and the **gait is driven by the
roles** — the legs swing in diagonal pairs, the head nods, the tail sways — so
a cat animates the way a horse does.

**The rig is calculated once and saved.** Deriving it is a pass over the
object's voxels, so the result is cached next to the layer
(`micro/alpaca.rig`) and read back on every later load. The cache carries the
layer's signature (size + mtime), so an edited or re-imported layer
invalidates it. A missing or stale cache just means the rig is derived in
memory.

**A frame bug worth remembering**: the rig's points come from the **field's**
frame (the cells the object is made of), but the shader looks the bones up in
the object's **anchored** frame (the field's frame plus the placement). With
the two out of step, the nearest bone was garbage, the field was sampled
through a wrong rotation, and **the animal did not move at all** — the walk
looked broken with every number in the log correct. The rig is now shifted
into the anchored frame when it is handed to the GPU.

**A skeleton is a CHAIN, and a bone is a rigid transform `R·p + t`.** The
bones are not independent: each one has a parent (the legs, head and tail hang
off the spine), and the behaviour composes the child's transform with the
parent's (`R_world = R_parent · R_local`, `t_world = R_parent · t_local +
t_parent`). The user saw the failure this prevents: **a broken neck when the
animal bowed** — every bone rotated about its own joint *in the rest frame*,
so when the spine pitched, the head kept turning about a joint that had
already moved with the body, and the neck tore away from the shoulders.

The transform must be a general rigid one and **not** "a rotation about a
pivot": the composition of two pivot-rotations is not a pivot-rotation. So the
bone row carries a **translation** (not a joint), and the shader applies
`R·p + t` — with its inverse `Rᵀ·(p − t)` where the field is sampled, and the
forward where the material is looked up. The union field, the gradient (and so
the normal), the AO and the self-shadow then keep working in the world frame
untouched. The field is the REST POSE.

**Two traps in that change, both measured:**
- the upload used to write the bone's **joint** into what is now the
  translation slot, and a joint is a world position — so every object was
  translated by metres on the first frame: the vase and the urn vanished from
  the scene and the alpaca measured 76 px instead of 81 000. A bone's
  transform starts as the **identity**; the behaviour pushes the real one
  every frame, and an object with no behaviour keeps the identity.
- a half-applied edit still looks fixed. The first version of the chain fix
  committed the app side, but its **shader** edit silently failed an assertion
  (the file was never written), and the visual check passed anyway because the
  old pivot formula was close enough for the composed matrix. Verify the file,
  not just the picture.

### The animal span in place, and the graze is a BOW

The user saw the alpaca "wildly flickering", "moving way too fast" and "stuck
behind a stone". All three were **one** bug, in the collision response, and
the measurement that found it was a per-frame trace of the behaviour's state
(`VF_TRACE_BEHAVIOUR=1`): **the heading advanced +1.2 rad every frame**.

Two causes, both in the retry:

- `b.heading = prevHeading + turn` was applied on **every** frame while the
  obstacle was still there, so the turns **compounded** — the animal span in
  place, which is the flicker and the "too fast";
- the retry step was the **frame's** step (a fraction of a millimetre in a
  headless run), so a candidate a hair away was still inside the obstacle and
  **no turn ever looked clear**.

The response now sets a heading **target**, which the behaviour turns toward
at a limited rate (1.1 rad/s) and the wander resumes on arrival — the animal
swings round and walks off instead of snapping. The retry step is the animal's
own footprint, so a candidate really *leaves* the blocked spot.

**The graze is a BOW, not a head dip.** A head on a 0.32 m neck can only dip
0.3 m, and the alpaca's muzzle is 1.3 m up — nowhere near the ground. So the
graze pitches the **spine** about the body's **back** (the front drops, the
front legs stay put) and the head and neck reach down and forward on top of
it. The ease is 2.5 s in and 2.5 s out: an animal that snaps its head down
reads as a glitch, one that leans down slowly reads as an animal deciding to
eat.

Verified headlessly: head **up** at t=0, a quarter of the way down at
t=1.2 s, muzzle in the grass at t=3 s.

**A gate has to pin the scene it measures.** The walking animal broke three
of the micro gate's own checks — the footprint of a 90° yaw, the pick's reach,
the house view — because each of them measures a *still* scene. The gate now
turns the behaviour **off** by default and the behaviour check turns it on
for itself.

**...and it must not destroy what it suppresses.** The gate turns the
behaviour off by rewriting the manifest, and the first version did it by
**deleting** the `behaviour` key. That is self-poisoning: the gate snapshots
the micro entries at startup and restores them at the end, so a run that
snapshots *after* an earlier run (or an app write) had already removed the key
restores the removal — and the shipped scene loses the grazer permanently.
Measured 2026-10-21: `set_micro` popped the key on its very first call, a
crashed run took its snapshot in that state, and `assets/world.json` came back
without it; the alpaca never walked again. The gate now writes a real no-op
command (`"behaviour": "stop"`), snapshots whole layer entries, prints the
snapshot (so a pre-existing loss is visible in the log), and replaces each
entry whole on restore, verifying by equality.

### The fur must not slide

The texture is bound to the **surface**, not to the world. A world-space
sample slides over the animal as it walks and as its bones swing, so the micro
shader samples the atlas at the point's **rest-pose** position — the very
point the field is sampled at. The texture then sticks to the surface it
belongs to, and follows its bone.

Two details are load-bearing, and both were reported as "the fur moves with
the alpaca":

- **The normal must be taken into the rest frame as well.** `detailAlbedo`
  and `detailNormal` (in `common_base.glsl`) choose their triplanar
  projection axes from the normal argument (`pow(abs(n), 8)`), so sampling
  the *position* in the rest frame while passing the *animated* normal makes
  the pattern crawl over the skin whenever a bone turns — a 35° spine bow in
  the graze is plainly visible. The shader un-bones the normal
  (`microDirUnbone`), samples albedo and bump there, and re-bones the bumped
  normal for shading (`microDirRebone` → `microDirFromBaked`). At a rest pose
  the round trip is an exact identity: the vase (no skeleton) differs from
  the pre-fix shader by max 3/255, i.e. float noise.
- **The staged Move preview is a DISPLAY offset, not a world position.**
  `main()` shifts the hit point by `preview.xyz` so the object appears
  dragged, while `microMapWorld` traces in the baked frame. Every lookup that
  describes the *surface* must subtract the offset again
  (`microShadeToBaked`, `microMapShade` for the normal and the AO) or it
  samples the field *beside* the object: the material cell falls back to the
  filler, the normal collapses to the bounding box's, and the animal shades
  like cardboard while it is being dragged. The field **trace** must keep
  using `microToBaked` — subtracting the offset there would make the object
  stop following the pointer.

Verification gotcha: a headless `--shot` of an **animated** micro object is
not deterministic — the behaviour advances during the frames before the shot
(`run.cpp` clamps the step to 0.1 s), so two identical runs of the alpaca
differ by ~34 k px. Any shader A/B on the animal is therefore pose noise;
A/B on a skeleton-free object (the vase) instead.

### The AI can tell an animal what to do

`set_behaviour` is a chat tool: *"let the alpaca run"* becomes
`{"name":"alpaca","command":"run"}`, and the app's
`App::setMicroBehaviour` puts the animal into that command. The commands
are `run`, `walk`, `graze` (eat grass), `stop` and `auto` (the animal
decides for itself) — the same set the behaviour's own state machine uses, so
the AI's instruction and the animal's choice are the same mechanism.

The behaviour is a pure transform (`MicroBehaviour`), so the instruction is
just a field on it; the collision response stays in the app.


### The crash that never reproduced: what ASAN ruled out

The user's session died with **"double free or corruption (out)"** right after
`edit mode -> move (disarmed)` — a heap corruption, and a heap corruption is
never "just a rendering bug". One real bug of mine could produce exactly that
signature: `m_hoverHit.voxel` is a **world-lattice** cell, and a micro
object's cell index (a 5 mm lattice) written there sends the brush, the box
selection and `describeSurfelsAt` to the wrong chunk with a wildly out-of-range
index. That is fixed ✓ — but it was never *proven* to be the crash, and
"probably" is not a diagnosis.

So the ASAN build (`/tmp/opencode/asan`, `-fsanitize=address`) drove the paths
that can be driven **without a mouse** (the user asked for that, and it is
also the only way to test a headlessly):

| path under ASAN | result |
|---|---|
| the pick toward all three micro objects (`VF_TEST_MICRO_PICK=1`) | clean |
| the Move preview on the alpaca and on the vase (`VF_TEST_MOVE_LIVE=...,micro/...`) | clean |
| the Rotate preview on the alpaca (`VF_TEST_ROTATE_LIVE` + `VF_ROTATE_LAYER`) | clean |
| the Move preview **and** a bounding-box selection together | clean |
| a **world reload** → the micro objects re-baked and the occluder list re-pointed at them | clean |

The reload case is the one that matters most and the one that was hardest to
reach: the app polls the **layer files'** mtimes (not the manifest), the
micro layers are not in the merged world, so touching `micro/alpaca.vxw` does
nothing — a **world** layer's mtime has to change, **while the app runs**
(the initial load records the signatures, so a file touched before the start
is already in them). `VF_SYNC_RELOAD=1` makes the rebuild inline so it lands
inside the measured window.

**What is left**: the interactive path — the real Ctrl+LMB click, the panel's
Move button, the trackball drag — cannot be driven without taking the user's
mouse. So the honest status is *not reproduced*, with the plausible suspects
that could be reached all clean. The one bug of mine that would produce that
signature is fixed, and the fix is pinned by the world-lattice rule above.

## Verification

| check | result |
|---|---|
| `test_micro.cpp` (13 cases) | pass — incl. a negative control for the continuity check, the cast-shadow verdict, and the **15 m pick** that the 0.30 m one missed |
| `micro_check.py` (20 checks) | pass — visibility, material, silhouette, micro-to-micro shadow, RECEIVE, CAST, alpaca visible, fleece texture, **selectable**, **live rotation preview**, **the interactive selection path** (a Ctrl+LMB pick and a plain click, through the real input path), **the MOVE DRAG** (grab → the X handle → a 2 m drag), **the autonomous behaviour** and **the AI's command**, world untouched, SVO backend |
| `VF_TEST_MICRO_PICK=1` (headless) | **3/3** micro objects selectable through the app's own `pickMicroObjects` |
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
| the rig | derived: 7 bones (spine, 4 legs, head, tail) from the object's own voxels, cached next to the layer |
| the gait | the head up at t=0, a quarter down at t=1.2 s, the muzzle in the grass at t=3 s (2.5 s of easing) |
| the move drag | `move: grabbed micro/alpaca.vxw on axis X` → the X handle at (349, 300) → the drag ends with delta (+2.000, 0, 0) m |
| shadow reaches the LEGS | ratio 0.87 → 0.57 after the 0.35 m step cap |
