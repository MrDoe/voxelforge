---
title: Collision — the fly camera and the walking alpaca
tags: [collision, camera, micro, walker, chunk-store, testing]
sourceRefs: [src/voxel/collision.hpp, src/voxel/collision.cpp, src/app/world/collision.cpp, src/app/frame/run_input.cpp, src/app/frame/run_startup.cpp, src/app/world/micro_objects.cpp, src/voxel/micro_field.hpp, tests/test_collision.cpp, AGENTS.md]
lastReviewed: 2026-10-21
---

# Collision — the fly camera and the walking alpaca

Two consumers, **one oracle**: the fly camera is a 0.25 m sphere and a walking
micro object (the alpaca) is a short vertical cylinder. Both ask a
caller-supplied `bool solid(worldPoint)` and the sweep itself is pure geometry
(`src/voxel/collision.{hpp,cpp}`), which is why the same unit tests pin the
camera and the animal against an analytic wall. The live wiring is
`src/app/world/collision.cpp`.

## The oracle is the LIVE store, not the load-time field

- **World** — `App::worldSolidAt` reads `LayeredWorld::store().cellAt(...)`
  (the live `ChunkStore`), not `VoxelField`. An edit is collidable the moment
  it lands: a carved tunnel becomes walkable, an added wall blocks. The store
  already carries terrain + objects + runtime edits, so there is no second
  geometry path to keep in sync.
- **Micro** — each object is its own fine lattice, sampled in its **baked
  frame** after undoing every transform the renderer applies. The order is
  load-bearing and mirrors the shader ([[micro-voxel-svo-overlay]]):
  1. undo the staged **Move** offset (`q -= m_moveDelta`),
  2. undo the **behaviour** (`MicroBehaviour::toLocal`) unless a stage holds it,
  3. undo the staged **rotation** about the canonical pivot
     (`relativePlacementRotation`, the same helper the trackball previews with),
  4. subtract the layer's `worldOffset` and call `MicroField::sampleSdf`.
  `microFieldSolidAt(index, p)` is the single-field query; `microSolidAt` is
  the union. The headless check uses the single-field form so a neighbour
  0.3 m away cannot answer for the object under test.

## The sweep (`src/voxel/collision.cpp`)

- `sphereBlocked` samples **27 points** (centre, 6 faces, 12 edges, 8
  corners). A wall is at least one 0.1 m cell thick, so it cannot fit between
  samples. `kCameraRadius = 0.25f` — a point test lets the near plane cross a
  wall before anything refuses.
- `moveSphere` substeps at **half a radius** (≥ 2 cm, ≤ 32 steps). The substep
  is what stops a fast camera tunnelling through a one-cell wall.
- A refused substep **slides**: each axis is retried alone, largest component
  first, from the position the previous axis reached — so hitting a wall at an
  angle glides instead of stopping. A corner refuses all three axes
  (`stopped`).
- An **embedded start** (camera spawned in geometry, world loaded around it)
  applies the delta verbatim: a blocked start must never be a permanent trap.
  This is the escape hatch, not a collision failure.
- `cylinderBlocked` is the walker shape: centre + ring at `sides` angles ×
  `y0..y1` layers. The caller chooses the heights, so terrain is excluded by
  construction (see below).

## Camera wiring

`App::updateCamera` saves `m_camera.pos`, lets `Camera::update` apply the
input, then resolves the **delta** through `resolveCameraMove(before, pos -
before)`. Resolving the delta (not clamping the result) keeps look/roll out of
it and lets a diagonal move slide along a wall. The **initial** placement
(`--cam`, `--shot`, the spawn) is never collided: a saved camera inside a
mountain must still open the view. `VF_NO_COLLIDE=1` turns it off;
`VF_NO_COLLIDE=0` keeps it on.

## Walker wiring

`microFootprintBlocked(index, offset)` builds the object's rest footprint in
world metres from `MicroField::solidMin/Max` (which **exclude** the grid's air
margin), using the **circumscribed** radius
`0.5*sqrt(wx²+wz²) + 0.02` — the behaviour turns the animal about its base,
so the footprint sweeps the diagonal, not half the longer side.

The predicate deliberately differs from the camera's:

- **Terrain is excluded.** The walker is ground-clamped to the heightfield, so
  a raised terrain cell is ground it climbs, not an obstacle. Only
  `cell.solid && cell.obj` (a world object such as a cabin wall) blocks.
- **Other micro fields block**, sampled at their current behaviour transform.
  This is the object-vs-object case: the alpaca must not walk through the vase.
- **Water blocks** when the ground under the footprint centre is below
  `WATER_LEVEL + 0.01`: the alpaca has no buoyancy state, so it does not stroll
  into the lake.
- The band tested is `base + 0.02` up to `min(0.45, 0.6·height)` — low enough
  for a fence or a wall foot, high enough that the ground it stands on never
  refuses the step.

The response lives in `App::updateMicroBehaviours`, **not** in
`MicroBehaviour` (a pure transform with no notion of geometry):

1. snapshot `offset`, `heading`, `gait`; run `b.update(dt)`;
2. if the candidate footprint is blocked, withdraw the step and retry turns
   `{+1.2, −1.2, +2.2, −2.2, +3.0, −3.0}` rad; the first free one becomes the
   new heading;
3. if every retry is blocked, restore the pose and switch to `Graze`
   (`stateT = 0`) — the only state that cannot walk into the obstacle next
   frame;
4. if it moved, lift-only **ground clamp**: `max` terrain height under the
   footprint's four corners plus the centre, read from the CPU mirror of the
   **shaded height texture** (`terrainTopAt`; `patchHeightTexture` keeps the
   mirror in sync after every live stamp, so an added mound is ground the
   animal climbs). One lookup per corner, and the same column tops the terrain
   is shaded from. Raise `offset.y` until the lowest solid cell clears it by
   0.002 m. Max-of-corners is what stops a slope swallowing the downhill edge;
   lift-only keeps an authored platform height and never treats a dip as a
   hole.

A walker placed inside geometry is not special-cased yet: it grazes rather
than escaping. The shipped alpaca starts clear, and the headless check would
catch a regression (a frozen walker fails the "moved" assertion).

## What this does NOT cover

- **Static placement / Move-Apply overlap.** `commitMove` writes the manifest
  and reloads; it does not yet report "this vase now intersects the cabin".
  The oracle exists (`solidAt`, per-field queries) but the check is not wired.
- **Fur vs per-bone animation.** The rig animates the body per bone while fur
  cards are generated in the rest pose; collision reads the field's baked
  solids, so it follows the body, not the coat. See the
  [[micro-voxel-svo-overlay]] transform contract.

## Headless evidence

`VF_TEST_COLLISION=1` (implemented in `applyStartupEnvOverrides`, before the
first frame) drives the shipping paths and logs `PASS`/`FAIL`:

```
collision check: camera drop from y=2.00: stopped at y=0.12
                 (fell 1.88 m, hit=true, inside=false)
collision check: micro 'micro/vase.vxw'   approach along (1,0,0): stopped 0.62 m in
collision check: micro 'micro/urn.vxw'    approach along (1,0,0): stopped 0.62 m in
collision check: micro 'micro/alpaca.vxw' approach along (1,0,0): stopped 0.62 m in
collision check: 1 walker(s), 600 steps: 0 sunken, 0 overlapping
                 (min ground clearance 0.002 m)
collision check: walker 2 ('micro/alpaca.vxw') moved 0.45 m
collision check: PASS
```

The moved distance is gait-dependent (0.45–0.65 m across rig revisions); the
assertion is only that the walker *advanced*, so a frozen body still fails.
A run with **zero active walkers now FAILS** instead of passing vacuously —
see the gotcha at the end of this page.

The camera starts 2 m **above** the camera so the drop actually travels (a
drop from a point already at its stop was a vacuous pass: `fell 0.00`). The
micro approach tests each field alone. The walker sim advances the real
`updateMicroBehaviours` 600 frames at 1/60, so a `--shot` taken with the hook
shows the walkers 10 s on — the check is the artefact, not the frame. Override
the step count with `VF_TEST_COLLISION_STEPS=N`. Run it under
`xvfb-run -a` with `VF_NO_OVERLAY=1` + `VF_OVERLAY_PATH=/tmp/...`
([[headless-render-mouse-safety]]).

The pure sweep is pinned by `tests/test_collision.cpp` (7 cases, 26
assertions): blocked axis refused, free axis slides, a corner stops the move,
an embedded start escapes, a zero delta never samples, and the walker cylinder
refuses a wall but not thin air.

## Gotchas

- **`cellAt().solid` is the runtime truth** (`sdfRaw <= 0`); `.obj` is the
  world-object flag. The camera blocks on all solid; the walker blocks only on
  `.obj`.
- **Micro transform order is not negotiable**: Move preview → behaviour →
  rotation preview → `worldOffset`. The renderer applies behaviour outermost;
  a query that undoes it in another order collides with a pose that is not on
  screen.
- **`solidMin/Max` exclude the air margin.** Adding `origin + worldOffset`
  gives metres; adding the margin back would inflate every footprint by 2 cm.
- **`sampleSdf` outside the grid** must return the true box distance (see
  [[micro-voxel-svo-overlay]]); a sampler that returns 0 makes the walker
  believe every far point is solid.
- The walker ring test runs `microFootprintBlocked` **after** the steering, so
  a blocked candidate that the code failed to withdraw shows up in the
  self-check rather than only in a screenshot.
- **Zero walkers is a FAILURE, not a pass.** Every walker assertion is inside
  the per-walker loop, so an empty behaviour list logged `0 walker(s) ... PASS`
  — a vacuous green. It now logs an error naming the likely cause. That is not
  hypothetical: `writeManifest` emitted the layer keys it knew and **omitted
  `WorldLayer::behaviour`** (the loader read the key, the writer did not), so
  one app run rewrote `world.json` and erased the alpaca's walker. The writer
  now emits the key when set; keep that in mind for any new layer field —
  a loaded key that the writer drops survives exactly one run.
