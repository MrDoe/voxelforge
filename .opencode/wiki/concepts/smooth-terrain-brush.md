---
title: Smooth brush (terrain + object surfaces)
tags: [live-edit, terrain, object, chunk-store, smoothing, brush, surfels, undo, water]
sourceRefs: [src/voxel/chunk_store.cpp, src/voxel/chunk_store.hpp, src/voxel/live_editor.cpp, src/voxel/live_editor.hpp, src/app/main.cpp, tests/test_store.cpp, tests/live_edit_check.py, docs/rendering.md, docs/testing.md, .opencode/wiki/entities/live-edit-brush.md]
lastReviewed: 2026-09-25
---

# Smooth brush (terrain + object surfaces)

**Smooth** (`ChunkStore::makeSmoothEdits`) is a *surface-position
relaxation*, not a 3D occupancy filter. It dispatches on the picked store
cell: a terrain cell relaxes terrain column tops, an object cell relaxes the
object surface along its own axis. Both paths plan the whole batch before the
first `ChunkStore::apply`, so every row sees the same pre-stroke snapshot —
in-place mutation would make the result order-dependent and undo harder to
reason about.

## Terrain pick

1. builds a cached top-cell snapshot for the circular footprint plus a
   one-cell sampling border;
2. rejects columns whose top is an object surface (`m_colTop` gate: a column
   with no landscape record, e.g. a promoted `Solid` underground chunk, is not
   editable terrain);
3. averages the eight X/Z neighbours (cardinal weight `1`, diagonal `0.5`);
4. applies a smoothstep radial falloff and rounds the relaxed integer lattice
   height;
5. clamps the result to the sampled neighbour range; and
6. emits `Clear` edits when lowering or `Set` edits with
   `StoreEdit::terrain = true` when raising. A changed span that contains an
   object is skipped, so sculpting a hillside cannot swallow a tree.

## Object pick

The same relaxation generalised to an arbitrary surface axis. The footprint is
the circular disk in the plane **perpendicular** to that axis, so one click
costs O(πr²) surface samples instead of an O(r³) volume scan.

- **Axis** = the picked cell's *one-sided* face: solid on one side, air on the
  other. Counting exposed faces is wrong — a 1×1 spike is air on both sides of
  X and Z and would be smoothed as a wall. Among one-sided axes the strongest
  SDF gradient wins (a corner's flat face beats its bevel).
- **Thin fallback** (a 1-cell wall: air on both sides of X) uses the SDF
  gradient for the outward sign, but only when it is the *only* open axis. A
  45° diagonal or a rod is thin across two or three axes and is left alone;
  otherwise the branch faired a staircase into a zigzag.
- **Rows** are found by scanning along the axis for the outermost object cell
  with air beyond it (a 24-cell local walk from the pick, then a full scan).
  Each row relaxes toward the weighted mean of its 8 lateral rows with the
  same falloff, clamped to their `min`/`max`.
- **Movement** is one contiguous span: lowering clears the object run from
  `target+1 .. current`, raising fills `current+1 .. target` with the row's own
  material/colour/response. So a bump is removed whole in one stamp (no
  unsupported floater), a notch fills, and a second stamp is a no-op.
- **Bump rule**: ground stands in for a missing lateral surface only when the
  whole 3×3 ring is bare *and* the run below rests on solid ground and is at
  most 4 cells (`kBumpRunCells`). A short grounded bump therefore settles onto
  the terrain in one stamp, while a tall post (longer run) and a staircase
  step or wall top (the ring has object surfaces) keep their height.
- **Ownership**: only object cells are cleared, every fill is
  `terrain = false`, and a raise whose span contains terrain is rejected. The
  terrain height texture and the water bed therefore never change.

A per-voxel occupancy filter (the first cut) planned its prunes from the
pre-pass set, so two mutually-supporting cells both died and the survivor
floated; it also eroded 45° walls. Relaxing a position instead of filtering
occupancy is what fixes both — see the quirk memory.

## Splat exactness

Object Smooth refreshes its live splat run over
`LiveEditor::kExactStampMargin` (=12, matching `ChunkStore`'s `kLiveBand`)
rather than the default ±3, because the store rebuild re-solves SDF normals
and baked AO/shadow across that whole band. `SmoothTerrainEdits::objectSurface`
marks the batch so `App::applyEditLive` selects it; undo and overlay restore
always use the exact margin. The patched run is then what a full reload of the
region would build. Other brushes keep the cheap ±3.

## Rendering and persistence

The batch goes through the normal live-edit pipeline: `LiveEditor` rebuilds
the affected SDF band and surfels, then the app patches the paged splat run,
the chunk-local SVO pool, and the terrain height texture. The height-texture
patch is terrain-only (`c.solid && !c.obj`) and still essential even though
water is a fixed plane: water shading, foam, absorption, and reflected-bed
marches read that texture. A lowering stroke also needs undo headroom:
`finishStroke` measures each inverse `Set` against the post-stamp column top,
so restoring a tall spike scans above the new surface. Overlay restore and
Clear use a full-column scan rather than fixed 64/128-cell headroom, and a
world reload flushes pending overlay writes and invalidates cell-state undo
history.

Strokes keep the normal per-cell pre-edit map, so a raised terrain cell is
restored with its original terrain/object bit. The overlay schema does not
change; `Clear live edits` and world reload need no special Smooth case.

## Controls and diagnostics

The Edit sidebar exposes **Size** and **Strength**. `Shift +`/`-` adjusts
strength while Smooth is selected; ordinary `+`/`-` changes the footprint
diameter. Headless hooks accept `smooth` as a mode and
`VF_SMOOTH_STRENGTH` (or `VF_EDIT_STRENGTH`) sets the initial value. The
`VF_TRACE` summary reports the branch (`smooth terrain:` / `smooth object:`)
plus valid rows and generated edits — useful when a visually flat patch
legitimately produces an empty batch.

The blue hover marker is a conservative ball around the footprint, not a
promise that every visible surfel in that ball moves. For terrain picks the
target set is decided by the pre-edit height snapshot; for object picks by the
surface rows found in the plane.

## Tests

`tests/test_store.cpp` pins terrain spike/pit relaxation, ownership, and the
object cases: the grounded spike collapses whole (and a second stamp is
empty), a plate notch fills with object-owned Sets, and a flat wall, a tall
post and a middle staircase step all produce **no** edits. `tests/live_edit_check.py`
(full gate) renders a cabin surface cell from the hero camera
(`VF_EDIT_DIAM=4.0`, `[object_smooth]` tag) and asserts the log says
`smooth object:` (never `smooth terrain:`) with a bounded but visible pixel
diff.

See [[entities/live-edit-brush]] for the complete brush workflow and
[[concepts/water-plane]] for the fixed-level water interaction.
