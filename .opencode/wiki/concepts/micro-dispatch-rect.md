---
title: A pass's screen rect must follow the object's POSE, not its baked AABB
tags: [micro, rendering, culling, dispatch, alpaca, fur, gotcha]
sourceRefs:
  - src/render/micro_svo_pass.cpp
  - src/render/fur_pass.cpp
  - shaders/micro_overlay.comp
lastReviewed: 2026-10-26
---

# A pass's screen rect must follow the object's POSE, not its baked AABB

## Symptom

The animated alpaca went **fully or partly transparent while it walked**. The
fur coat kept drawing (sometimes floating over the missing body), which made it
look like a shading/alpha problem inside the micro overlay.

## Cause

`MicroSvoPass::record()` culls the pass and sizes its **compute dispatch
rectangle** from each micro world's AABB (`MicroWorldRow::lo/hi`). Those are the
**baked** bounds. But the rendered object is not there: `microMapWorld` maps
every sample through the behaviour's transform (the walk) and the live
Move/Rotate previews, and the skeleton is applied on top. The body can therefore
be metres outside its own rectangle.

The dispatch rect is not a hint - pixels outside it are **never shaded**, and
`anyVisible == false` skips the pass entirely:

- measured with a frozen walk offset, rect `[17,7..429,200]`:
  - −1 m: posed screen centre `(412,147)` - inside, but the silhouette's
    right/bottom fringe lay beyond `x=429`/`y=200` and was sliced off;
  - −2 m: posed centre `(516,209)` - **outside the rect**, the body is cut and
    the far side is simply background;
  - +3 m: posed centre `(270,62)` - the head crosses the rect's top edge `y=7`.
- After the fix (same camera, freeze offsets −1.5…+1), the silhouette vs an
  animal-free baseline measured 9387 / 7282 / 5554 / 4195 / 3337 / 2809 px, and
  the −1.5 m bbox `(396,76)-(522,261)` crosses the old `429`/`200` edges - i.e.
  the old rect clipped most of that frame.

The fur pass (`src/render/fur_pass.cpp`) had already mirrored the pose chain to
place its own rectangle, which is why the coat outlived the body. The micro
pass was the one that did not.

## Fix

`MicroSvoPass::record()` now computes a `posedCentre` with the **same forward
chain the shader applies** (the inverse of `microShadeToBaked`):

1. live rotation preview about its pivot,
2. behaviour pivot rotation, then the behaviour offset,
3. the staged Move translation.

The cull and the rectangle use that centre, with radius = AABB half-diagonal +
**one bone's reach** (0.30 m), because a swinging limb carries surface outside
the rest box - the field is the rest pose, sampled through the bone's inverse,
so the shader can indeed draw there. The rect projects the bounding sphere's
four screen extremes at the nearest depth (the fur pass's formula), instead of
projecting the 8 static AABB corners.

Diagnostic: `VF_TRACE_MICRO_RECT=1` logs each world's baked vs posed centre, its
screen point and the final rect.

## Rule

A screen-space rectangle or frustum cull is a **render-space contract**, so it
must be derived from the same transform chain as the shading. Any object that
moves without re-baking (behaviour, previews, bones, skinning) needs its bounds
posed first. The baked AABB remains valid for the *field*, never for *where the
pixels are*.

See [[concepts/micro-voxel-svo-overlay]], [[concepts/fur-raster-parity]].
