---
title: Preserving the surface when deforming a micro voxel object
tags: [micro-overlay, animation, deformation, surface, fur, bones]
sourceRefs: [shaders/micro_overlay.comp, src/voxel/micro_mesh.cpp, tools/micro_mesh_check.cpp, .opencode/wiki/concepts/micro-bone-claim-rule.md]
lastReviewed: 2026-10-26
---

# Preserving the surface when deforming a micro voxel object

When a micro voxel object is deformed (walked, grazed, rotated, moved), two things
must hold at once:

1. **The body surface stays continuous** — no holes, no cuts, no floating ghost
   voxels, no black pixels.
2. **The coat (fur) follows the deformed surface** — it is sampled in the REST
   frame and the behaviour transform is applied at field-sampling time, so the
   coat and the body agree frame by frame.

## Why this is hard

The micro overlay sphere-traces a **rest-pose SDF** through the inverse of a bone's
transform. When a bone poses (e.g. the graze bows the spine 35°), the mapping
between posed space and rest space becomes discontinuous at bone seams: a point
just inside the posed neck can un-bone to a rest point that belongs to a
*different* bone. The result is a **no-claimer shell** — a thin region where no
bone is willing to speak for the point — and the tracer strides through it.

## The two traps learned

- **The reject step (`kRejectDeep`) traded holes for black.** Turning it on
  discarded deep hits and kept marching → holes (217 core holes). Turning it off
  keeps holes at baseline (96) but deep hits shade black (243 px). The fix is
  neither on nor off alone: keep it **off** and add the **deep-hit retry**
  (`microMaterialAt` + walk-back toward the surface) which turns deep hits into
  proper skin (84 black px, below the HEAD baseline of 101).
- **The coat must follow the body.** It is sampled in the rest frame via
  `microRestPoint` / `microUnbone` (bone's inverse transform), and the
  behaviour transform is applied at field-sampling time (`microMapWorld` →
  `microShadeToBaked`). Sampling the texture in the animated frame makes the
  fleece crawl over the body; sampling it in the rest frame makes it stick.

## Verifying a mesh path

Do not trust a screenshot. Use the eroded posed-GT metric (core holes / ghost px /
near-black) and the `fur_check` gates, which measure confinement, shape, and
backend independence. See [[concepts/micro-bone-claim-rule]] for the full
mechanism and the two red herrings (cell-centre ownership, shrinking the cap)
that were measured and rejected.

## The mesh path: three things the voxelizer must not trust

A surface mesh is the only deformation source with no world→rest map, so the
discontinuity above cannot exist in it. Getting a *solid* out of a posed
triangle soup is where the traps are; all three below were measured on
`assets/models/alpaca_from_voxels.stl` (`vf_micro_mesh_check`, 2026-10-26).

1. **Triangle winding is author data, and it is wrong.** The alpaca's own
   side-car — extracted from the voxels, so it should be perfect — has 26 055
   of its 117 392 shared edges traversed in the SAME direction by both adjacent
   triangles, i.e. about a third of the surface is inverted. A
   "which side does the normal face" classification then marks ~1/3 of the
   surface cells on the wrong side, the shell grows one-cell holes, and the
   interior flood fill escapes through them (measured: 15.7 % agreement with
   the rest field, 12 328 cells solid in air).
2. **So the side test is crossing PARITY, not a normal sign.** One scanline per
   (y, z) row along +x, jittered off-axis so it cannot graze an edge; a closed
   surface is crossed an odd number of times from inside. Cost: 46 ms for the
   full 1.2 M-cell grid, against 150–183 ms for a per-cell closest-triangle
   band. Parity is even-odd, so two posed parts that interpenetrate (a leg
   rotated into the belly) leave an *enclosed* air pocket; a flood fill from
   the grid border seals exactly those, which is why the result is the union of
   the parts rather than their symmetric difference.
3. **A side-car mesh must be normalised into the layer's frame.** The alpaca
   STL starts at cell (5, 0, 8) rather than the object's min corner: loaded
   as-is, its far side falls outside the field's grid (the fill leaks out of
   the box) and the rest round trip reads 15.7 %. `loadMicroMesh` subtracts the
   mesh's own AABB min and records the shift in `restOffset`.

With those three fixed the rest round trip is **exact** — 157 700 / 157 700
cells identical to the field baked from the `.vxw` records. That is the gate the
whole path rests on: if the rest mesh does not reproduce the rest field, every
posed delta is built on a different object than the one on screen.

**Rigid 0/1 skinning is not volume-preserving.** The graze pose grows the solid
set from 157 700 to 172 349 cells (+9.3 %) at full depth, because a triangle
spanning two bones stretches. A delta voxelizer must therefore be defined as
"field(pose) minus field(rest)", never as "the rest solid rotated by the bones".

**Ownership flips are the reason the fleece used to slide.** Comparing the
fixed rule (un-bone the posed cell centre with each candidate's own transform,
then measure that bone's own rest segment) against the old one (nearest rest
segment to the posed point) over the posed surface: 2.9 % of cells change owner
at graze t=0.25, 6.2 % at 0.5, 8.5 % at 0.75 and 12.6 % at 1.0. The old rule
re-labels an eighth of the animal at full graze, and each flip samples the
texture at a point that belongs to a different bone.
