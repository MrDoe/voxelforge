---
title: Micro overlay bone claim rule and the deep-hit material retry
tags: [micro-overlay, sdf, sphere-tracing, bones, animation, graze, shader]
sourceRefs: [shaders/micro_overlay.comp, src/voxel/micro_rig.cpp, .opencode/wiki/concepts/micro-voxel-svo-overlay.md]
lastReviewed: 2026-10-26
---

# Bone ownership in the micro overlay: the claim rule and the deep-hit retry

`shaders/micro_overlay.comp` sphere-traces a micro object through a **rest-pose
SDF** that is read through the *inverse* of a bone's rigid transform. Everything
that goes wrong visually on an animated object — grey ghost voxels, holes in the
skin, pixels painted pure black — comes from one of two places: **whose
transform the point is read through**, or **what the material lookup does when
the trace stops in the wrong place**.

## The two ownership rules

- **`MicroRig::boneAt` (CPU, authoritative)** — nearest rest segment to the
  point, raw distance, no radius. Cell ownership (and so the forward-mapped
  ground truth) is decided at the **cell centre**.
- **`microClaimField` (GPU, the field's owner)** — bone *i* claims *p* iff, after
  un-boning *p* with *i*'s transform, *i* is the nearest rest segment to that
  point. The field is the **minimum over the claiming bones**; a point nobody
  claims is air and returns a *step*, not a verdict.
- **`microBoneAt` (GPU, for texture/rest point)** — argmin over
  `distance − capsule radius`. A third rule; it only feeds shading, not the
  field.

## The no-claimer shell — the real mechanism behind neck holes

Under the graze (bone rotations of 54–85°) the claim test fails for **every**
bone in a thin shell near the neck surface. Measured at a true GT surface point:
`unbone_neck6(p)` sat 0.2555 m from neck6's segment and 0.2533 m from spine's —
a **0.0022 m (0.27 voxel) margin** that flips ownership. Inside that shell the
fallback returns `min(capsule, 6·voxel)` = a flat **0.041 m** step, so the tracer
strides 6 voxels at a time and then does one of two things:

1. **jumps over** the thin negative band beyond the shell → the pixel shows
   background → a **hole**;
2. **lands deep** inside → the octree descent reaches a **solid terminal with no
   brick** (material lives only in the bricks around the surface) →
   `microSurface` returns false → `shadeMicro` paints **pure black**.

The reject step (`kRejectDeep`, now `false`) was a patch for case 2 that caused
case 1: it discarded deep hits and kept marching, eating the neck.

## Measured comparison (eroded posed-GT oracle, 640×360 graze)

| variant | core holes | ghost px | near-black px |
|---|---|---|---|
| HEAD: `argmin` + reject | 95 | 80 | 101 |
| claim + reject | 217 | 3 | 99 |
| claim, no reject | 96 | 3 | 243 |
| **claim, no reject + deep-hit retry (shipped)** | **96** | **3** | **84** |

So the claim rule is what removes the ghosts (80 → 3); disabling the reject
removes the neck holes; and the **deep-hit retry** is what stops the deep hits
from reading as black (243 → 84, below the HEAD baseline).

## The deep-hit retry

`microSurface` splits the lookup into `microMaterialAt(w, q, voxel, …)` (clamp →
chunk → octree → brick) and retries it up to **six** times, walking back along
the ray by the depth it is inside (`q -= dirRest·max(−fd − 0.3·voxel, 0.5·voxel)`),
stopping as soon as the field says the point has left the object. Two details:

- the ray **direction** must be turned into the owner's rest frame too
  (`microDirUnbone(w, microDirToBaked(w, rd), owner)`); it used to be added in
  the baked frame, which differs by the whole bone rotation once a bone is posed;
- the walk is in the *field's* frame, so `microFieldAt(w, q, voxel)` reads it
  directly.

## Red herrings (both measured, do not retry)

- **Measuring ownership at the cell centre** instead of the point: 371 vs 364
  core holes in the sim — no change.
- **Shrinking the no-claimer cap** (6 → 2 voxels, or a "creep" cap used when some
  bone maps the point inside): 58 → 44 core holes at best. The cap is not the
  lever, because the trace's 8-step bisection converges to the field's sign
  **flip** regardless of the incoming step size — the converged point is a
  property of the field, not of the step.

## Offline tooling

`/tmp/opencode/neck/` holds the replica used for all of the above: `fieldsim.py`
(trace + claim field), `gtcast.py`/`build_gt.py` (forward-mapped posed ground
truth, **stored in a grid that covers the POSED bounds** — the rest grid clips
the grazing head, which sat at z 19.58–20.00 while the rest grid ends at 19.54,
making the oracle blind to exactly the region with holes), `measure.py`
(cores/ghost/black), `sweep3.py` (variant sweep with a deep-hit metric).

See also [[concepts/micro-voxel-svo-overlay]] and [[concepts/fur-raster-parity]].
