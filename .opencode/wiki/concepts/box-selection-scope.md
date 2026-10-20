---
title: Bounding-box selection scopes the brush
tags: [live-edit, selection, ui, brush, post-comp, testing]
sourceRefs: [src/voxel/box_select.hpp, src/app/edit/box_select.cpp, src/app/edit/live_edit.cpp, src/app/frame/run_input.cpp, src/app/frame/run_brush_preview.cpp, shaders/post.comp, shaders/common_surfel.glsl, tests/live_edit_check.py, AGENTS.md]
lastReviewed: 2026-10-10
---

# Bounding-box selection scopes the brush

A world-space lattice AABB that acts as the **selector** for the live-edit
brush. The box is defined by the user, drawn by the post pass, reported (cells
+ surfels) on release, and — the point of the feature — **every** stamp is
clipped to the cells inside it. The pure math is one struct
(`src/voxel/box_select.hpp`, unit-pinned by `tests/test_box_select.cpp`); the
App wiring is `src/app/edit/box_select.cpp`.

## The gesture and the state

- **Shift+LMB drag** (tool armed, or Ctrl held): the pressed cell is the
  anchor, the cell under the cursor the opposite corner. It reuses the same
  hover pick the brush uses, so the box lands on real cells with **no plane
  maths** — corner-to-corner in lattice space, inclusive `[lo, hi]`.
- **X** clears it (also the Edit section's **Clear box** button). With no box
  the brush behaves exactly as before.
- Rotate/Move ignore the box: they act on a *layer owner*, not on cells.

## One representation, three consumers

The box is stored **once** (`App::m_boxSelect`, inclusive lattice cell indices)
and read through one accessor, so the three consumers cannot drift:

1. **The stamp** — `App::applyEditLive` clamps the edit cell set via
   `CellBox::clampEdits`. A stamp entirely outside the box does *nothing*: no
   store patch, no stroke, no undo step (it returns before
   `commitStoreEdits`).
2. **The hover tint** — `BrushUBO` grew two vec4s (`bBoxLo`/`bBoxHi`, cell
   indices + active flag) and `sharedInBrushVolume` clips the volume test with
   the same integer compare. Without this the preview would promise cells the
   stamp never writes.
3. **The wireframe** — the post pass strokes the 12 AABB edges in screen space
   from the same cell indices (world corners derived from the push constants'
   `worldSize`/`voxelSize`), gated on render flag bit 16 like the other
   outlines.

Cell indices (not world metres) are the shared currency because the pick, the
brush rasterizers, `ChunkStore` and the surfel keys all live in that space; a
float bounds comparison would drift by a half-voxel against the cell set a
stamp actually writes.

## Contents report

On release (and for the `VF_TEST_BOX` hook) `reportBoxSelection()` counts
solid cells / object-owned cells straight from the `ChunkStore`, and sums
`buildChunkSurfelsRange` over the overlapped chunks — **the same range builder
the live-edit path uses**, so the surfel census inherits the bake's normal
pipeline instead of a local rule of its own (see
[[concepts/live-edit-surfel-parity]]). A fully buried box legitimately reports
0 surfels: disks only exist on exposed faces.

## Verifying it (the part that cost two iterations)

`live_edit_check.py --only box` (ctest `box_select_check`, labelled
`live-edit` **and** `preview`). The comparison that makes it a real gate:

- The control for a **scoped stamp** is a `VF_TEST_BRUSH` render at the same
  cell with the same box — preview only, no edit. The scoped stamp must come
  out **pixel-identical** to it. That single diff pins both halves at once:
  the CPU clamp (nothing commits) and the shader tint clip (nothing is
  promised). A broken clip tints the visible brush volume and shows up here.
- `VF_TEST_EDIT` arms the tool at *startup*, and the frame loop then re-picks
  at the window cursor, so a plain untouched baseline disagrees over **where
  the preview is** rather than what the box scoped — pin the hover with
  `VF_TEST_BRUSH` in both frames.
- Geometry hygiene: a "far" box that still overlaps the brush ball tests
  nothing (the first version did), and a fully buried box reports 0 surfels by
  design.

Headless hook: `VF_TEST_BOX="x0,y0,z0,x1,y1,z1"` activates the selection
before `VF_TEST_EDIT`, so a shot can prove the scoping.

## Interactive verification

The interactive gesture was verified by a peer session with real X11 input
(XTEST, 1600×900 window) on 2026-10-18: all four checks pass — wireframe
draws (3650 → 10 387–11 620 cyan pixels while boxed, back to 3610 after
X), the tint clips to the box (an entirely-outside hover shows **no** tint
at all, and unclips after X), an outside stamp is inert (the
"fully outside the selection box (N cells dropped)" log with no stroke/undo/
overlay write), and X clears (log "cleared (the whole world is editable
again)"). Release census fired correctly. The idle scene animates
(clouds/water/grass), so pixel-identity claims need a same-state floor —
the boxed frames are indistinguishable from it. This closes the
interactive-coverage gap named in
[[concepts/interactive-ui-coverage-gap]].

## Gotchas worth remembering

- `applyEditLive` returns early on a fully-outside stamp — that also means no
  "live edit: N cells" log line exists, which is the check's strongest
  assertion (absence, not a zero).
- The Smooth planner emits contiguous per-column spans; clipping per cell can
  half-apply a column. That is the honest behaviour of a scoping box, and the
  log line "N of M stamp cells kept inside the box" makes it visible.
- See also [[concepts/oriented-brush-rasterizer]] (the volume shapes being
  clipped) and [[concepts/brush-preview-visibility]] (why the tint is a
  preview, never the result).
