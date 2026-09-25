---
title: Live-edit brush (Carve / Add / Delete / Paint / Smooth / Rotate / Move)
tags: [live-edit, chunk-store, surfels, splat, svo, persistence, brush, smoothing, terrain, rotation, trackball, move, placement]
sourceRefs: [src/voxel/live_editor.cpp, src/voxel/chunk_store.cpp, src/voxel/chunk_store.hpp, src/voxel/editable_world.cpp, src/voxel/editable_world.hpp, src/app/main.cpp, shaders/splat.frag, src/render/splat_pass.cpp, src/render/splat_pass.hpp, shaders/splat.vert, shaders/splat_cull.comp, shaders/splat_tile_bin.comp, shaders/splat_tile_render.comp, tests/live_edit_check.py, tests/test_store.cpp, tests/test_editable.cpp, tests/visual_check.py, docs/rendering.md, docs/tooling.md]
lastReviewed: 2026-09-26
---

# Live-edit brush

The `C` edit panel selects a voxel brush at the hovered surface point. Carve,
Add, Delete, and Paint stamp analytic volumes; Smooth instead relaxes a
surface position (terrain column tops, or an object surface along its own
axis). **Every** stamp lands in the runtime `ChunkStore` and
patches both GPU backends in the same frame — there is no bake/record edit
path any more (the legacy `carve_edits.vxw` / `raise_edits.vxw` layers are
read-only leftovers); the "Clear live edits" button drops
`assets/runtime_edits.vxw` and reloads.

## Brush modes

| mode | volume | store op | notes |
|---|---|---|---|
| Carve | oriented cylinder, base at the hit cell, `depth` along **-normal**, top face `kCarveTopMargin` (0.2 m) above the base | `Clear` | depth-limited scoop; opens the ground it starts at (see [[concepts/water-plane]]) |
| Add | the surface **grows out** along **+normal**: footprint disk of `diameter/2` extruded `depth`, closed by a fillet of `min(radius, depth)/2` | `Set` | clicking a wall thickens it over its whole footprint, only the outer lip rounds; nothing is emitted behind the surface |
| Delete | ball of `diameter/2` about the hit cell | `Clear` | ignores depth |
| Paint | same ball | `Paint` | recolours solid cells with the panel's Material combo; filtered by `store.cellAt().solid` so it never creates geometry |
| Smooth | terrain: circular column footprint; object: circular disk in the picked surface's plane | `Clear` / object-owned `Set` | one weighted surface-position relaxation with falloff; terrain columns and object rows are relaxed along their own surface axis; object picks never convert terrain; `VF_SMOOTH_STRENGTH` controls 0..1 strength; splats refresh over the exact ±12 store band |
| Move | no store op | staged `pos` | grabs the exact owner; colored X/Y/Z handles select the constrained world axis; Apply persists the translation |

Digs may go below the water plane and are then flooded — details and the gotchas
in [[concepts/water-plane]].

Brush strokes persist through the store overlay (saved on mouse release) and
the hover ray-picks the store (`rayPickStore`), so the tool always works on
what is rendered.

## Per-voxel mode (1 voxel wide)

The brush is sized in **voxels** (1..120, i.e. 0.1..12 m) and 1 voxel is the
per-voxel sculpt mode. At that width Add and Carve bypass the volume
rasterizers and touch **exactly one cell** (`EditableWorld::makeSingleVoxel`):

- **Carve** removes the voxel under the cursor.
- **Add** places the cell just outside the surface, one step along the
  *dominant* axis of the normal. The pick always lands on solid material, so
  adding the picked cell would be a no-op; and the step must be single-axis —
  `round(n / VOXEL)` sends a smoothed corner normal like (0.7, 0.7, 0) seven
  cells off target. Stepping outward is not a convenience: `rayPickStore`
  returns the first *solid* cell along the ray, so an air cell can never be
  hovered and Add has no other target. Hence two different marks on screen —
  the post-pass outline is the voxel you picked (and, for Carve, the one that
  is removed), the green/warm tint is the cell the stamp will change.

Two things had to change to make one voxel actually mean one voxel: the radius
clamp in `applyEditLive` had a 0.1 m floor (so even a 1-voxel brush rasterized
3 cells across), and a 1-voxel *volume* would still take the neighbour along the
normal — measured at 2 cells for Add and 4 for Carve. The depth slider is
disabled at 1 voxel, `+`/`-` step the width by one voxel, and every
`VF_EDIT_DIAM`/`VF_EDIT_DEPTH` override is snapped onto the lattice by
`App::quantiseBrush()`, so `VF_EDIT_DIAM=0.1` is exactly 1 voxel.

The hovered voxel is outlined by the post pass (voxel-sized box, render flag
bit 4), which is what makes the target readable at this scale. The hover branch
of that outline is centred on `uHover`, not `uSel` — it used to read the
selection feed, so with nothing selected the outline sat on the world origin.

Gate: `check_per_voxel` in `tests/live_edit_check.py` asserts the stamp log
reports exactly `1 cells` (a cell count, not a pixel diff — a single 0.1 m
voxel is near the render/ASCII resolution limit), and
`tests/test_editable.cpp` "per-voxel add and carve touch exactly one cell"
pins the placement, the corner-normal step and the lattice clamp. See
[[concepts/oriented-brush-rasterizer]] for the volume-path rules.

## One click is one edit (click vs drag)

The stamp trigger is a **distance** test: the hovered cell must differ from the
last stamped one by at least `spacing` (`max(VOXEL, diameter/4)`), and
`m_hasStamp` must be true. That has no notion of a click, so a single press
stamps *again* the moment the hovered cell changes — from ordinary cursor
jitter, or because the Add itself changed which cell the ray hits. At
1-voxel width that is visibly **two voxels added or deleted per click**.

A second stamp now also requires the pointer to have moved **net displacement
`kDragTravelPx` since the PRESS point** — *not* since the last stamp, and not
total path length. Properties that matter:

- A stationary click is exactly one edit **however many frames the button is
  held for** — the fix does not depend on timing at all.
- Measuring from the press rather than the last stamp is what rejects a
  wobbling hand: a held pointer that jitters *and returns* near where it
  pressed never accumulates distance, while a real drag only increases its
  distance from the origin. A wobble measured from the last stamp would
  re-arm on every jitter excursion.
- The gate can only ever *delay* a stamp, never add one, so the existing
  world-distance rate limit for drags is untouched.
- No click is ever lost: `doStamp` is initialised to `lmbEdge && editLmb`
  and the gate block only ever *sets* it true, so a press's first stamp is
  unconditional. A press that misses the hover still lands exactly one stamp
  as soon as the pick hits.
- Deliberate dragging still paints at frame rate.

The press point is captured at the **first stamp of the stroke**, not at
LMB-down (`main.cpp`, the `!m_hasStamp` arm). Normally the same frame; it
differs only when the press frame's pick misses, in which case the stroke
simply starts where the first real edit landed. `m_hasStamp` is cleared on
mouse release and on world reload, and the reload path re-syncs
`m_lmbWasDown` from the physical button so a reload during a held click
cannot manufacture a fresh stamp edge — so the press point cannot go stale
across strokes.

**Known limit — a dead disc of radius `kDragTravelPx` (`main.cpp`, currently
14 px).** Because travel is net displacement, a drag that stays within that
radius of its press point paints nothing, *including a genuine small circular
drag* (~28 px diameter). That is the deliberate price of jitter rejection, not
an oversight. Two consequences for the next change here: do not "fix" it by
reverting to a time debounce (tried, rejected — see below), and do not read a
14 px value in this page as gospel, it is a tuned constant that has already
moved once (10 → 14).

A time-based debounce was tried first and rejected on two counts: it
rate-limits a real drag, and it still double-stamps the case that actually
mattered — a *slow* press longer than the window whose re-pick landed on a
different cell. Travel has no such hole.

**Verification gap.** The trigger reads `glfwGetMouseButton` directly, so
there is **no headless seam** to regression-test a click, and
`check_per_voxel` in `tests/live_edit_check.py` pins the *cell count per
stamp*, not the click-vs-drag gate — the two are different properties. The
whole gate is therefore verified **manually only** (click, then a single
Ctrl+Z restores), which is also why the threshold could be retuned to 14 px
with a green suite. A future edit can silently regress this. An env-injected
press point + travel offset would close it.

## Rotate trackball

Rotate is deliberately separate from the voxel brush. After the Rotate mode is
selected, one plain LMB click activates the exact owner carried by
`PickHit::layer`; the click itself does not rotate or write a manifest. A
bounds-centered trackball then appears around the placed AABB. Click-drag its
outer/Y circle, wide horizontal/X ellipse, or tall vertical/Z ellipse;
each gesture composes around the object's own local X/Y/Z axis. Release stages
the converted angles; the Toolbox/Dashboard **Apply rotation** button commits
them to that `.vxw` entry once, while **Cancel** discards the preview. Terrain,
water, other layers, and unowned live geometry are never part of the transform.

The trackball is only an interaction surface: rotation still uses the exact
canonical bottom-center pivot and the same `Rnew * transpose(Rold)` preview in
forward, GPU-cull, and tile paths. Its screen projection follows the
view-space depth math in `splat.vert`; ring hit-testing runs before ImGui
mouse capture, and entering Rotate hides the World Layers panel. Panels under
the ring are input-blocked so the gizmo cannot click a control underneath.
The focused UI flow is documented in [[concepts/layer-placement]].

## Water: one fixed-level plane

The water is a single analytic plane at `WATER_LEVEL = -0.9` (`concepts/water-plane`):
the splat backend rasterizes it as a world-wide 0.2 m grid of coplanar surfels
(pure coverage — the shader intersects the plane per fragment) and the depth
test against the opaque prepass hides the cells over dry land, so a dig below
the level is water with no per-column bookkeeping. Both backends shade the same
plane, and the app keeps the terrain *height texture* the water shading reads
in sync with the store (`App::patchHeightTexture`) after every stamp and
overlay restore. `VF_SPLAT_NOWATER=1` skips the plane draw (A/B for tests).

## Smooth brush

`ChunkStore::makeSmoothEdits(center, radius, strength)` is a non-mutating
snapshot-and-plan operation that dispatches on the picked store cell.

A **terrain** pick finds the top solid cell of each terrain column in the
circular footprint, samples the eight X/Z neighbours (cardinal weight 1,
diagonal weight 0.5), and applies

```
target = round(top + strength * falloff(distance / radius) * (neighbourAverage - top))
```

Targets are clamped to the sampled neighbour range, so one pass cannot create
a new spike. Lowering emits `Clear` edits for the removed surface cells;
raising emits `Set` edits with `StoreEdit::terrain = true`, which keeps the
raised cells terrain-owned rather than tagging them as a new object. A column
whose top is object-owned, or whose changed vertical span contains an object,
is skipped.

An **object** pick generalises the same relaxation to an arbitrary surface
axis. The axis is the picked cell's *one-sided* face (solid on one side, air
on the other; the SDF gradient breaks a thin wall's two-sided tie), and the
footprint is the circular disk in the perpendicular plane. Each row's surface
coordinate relaxes toward the weighted mean of its 8 lateral rows, clamped to
their min/max, and the movement is applied as one contiguous span along the
axis: lowering clears the object run `target+1 .. current`, raising fills
`current+1 .. target` with the row's own material/response. So a grounded bump
collapses whole in a single stamp (no unsupported floater), a notch fills, and
a flat face, a 1-cell wall, a tall post and a staircase step are fixed points
(a second stamp is a no-op). Terrain is never converted — a raise whose span
hits terrain is rejected, and ground stands in for a missing lateral surface
only when the whole 3×3 ring is bare and the run is a short grounded bump
(≤ 4 cells).

The app sends the batch through the normal `LiveEditor`/GPU/height-texture
path; no `.vxw` layer is written. Object Smooth refreshes the splat run over
`LiveEditor::kExactStampMargin` (12, the store's SDF band) so normals/AO
patched by the rebuild match a full reload; other brushes keep ±3. The blue
hover sphere is conservative: it marks the footprint, while the actual target
set is determined by the column heights (terrain) or the surface rows (object).
See [[concepts/smooth-terrain-brush]].

## Undo and "Clear live edits"

Each stamp records the pre-edit state of every touched cell (first occurrence
wins) in a per-stroke map; stroke end (`App::finishStroke`) pushes it as one
undo step (bounded 250k cells / 32 steps; an over-sized stroke is dropped
rather than partially undoed). `Ctrl+Z` / the panel's Undo replays those cells
as Clear/Set edits through the same `App::commitStoreEdits` path a stamp uses
(store → LiveEditor → GPU patch + height-texture sync + overlay save), so the
geometry disappears in the same frame. For a lowering stroke, `finishStroke`
derives extra height-texture scan headroom from the inverse Set cells relative
to the post-stamp tops. This is what lets undo restore a tall terrain spike,
including after several drag stamps, without leaving the water/bed texture
stale. The terrain-only height refresh ignores object solids; overlay restore
and Clear scan from the world top so their headroom cannot be too small. A
world reload flushes the overlay writer before loading it and drops the old
cell-state undo history, so stale inverses cannot target a new generation.
The touched chunks then keep the live path's store-derived shading until the
next full rebuild, exactly like a painted stroke.

`Clear live edits` (panel) does the nuclear revert in place: flush the overlay
writer (a pending save could resurrect the file), delete the overlay, re-adopt
the store from the baked pools (`LayeredWorld::invalidateStore` →
`ChunkStore::adopt`), re-seed the touched chunks (surfels, SVO pools, height
texture) and request a full world reload so the bake's LOD rings and normals
come back.

**Regression it pins (cost a long hunt):** `SplatPass::patchChunkSurfels`
used to ignore zero-surfel patches — an empty `std::vector::data()` is null and
tripped its `!data` guard — so any chunk whose whole run became empty (an
undone stroke that was its only content, a cleared world) kept drawing the
removed material. The store and the LiveEditor cache were correct; the GPU
patch was the only broken layer, which is why store-level unit tests pass
while the render falsifies the revert. `VF_TEST_UNDO=1` / `VF_TEST_CLEAR=1`
drive both paths headlessly, `VF_OVERLAY_PATH` keeps test overlays out of
`assets/`.

## Persistence

Edited chunks are flagged `edited` and serialized by `OverlayWriter` to
`assets/runtime_edits.vxw` (VXW v2 store section, schema 3 = per-chunk edit
AABB plus the per-cell texture tag) on mouse release, temp+rename+debounced. The app restores it at startup
and after every world reload (`App::loadStoreOverlay`: GPU seed + refresh the
saved AABB). The file is intentionally **not** in `world.json`, so the layer
poll never reloads it. `VF_NO_OVERLAY=1` skips the restore — the test scripts
set it so an interactive session's painting cannot change reference shots.

## Tests

`tests/live_edit_check.py` (headless PPM, hermetic via `VF_NO_OVERLAY` /
`VF_OVERLAY_PATH`):
Add in splat **and** `--mode svo`; Delete/Paint in splat with `VF_MICRO=0` so
the measured diff is geometry, not the dropped micro tail; the micro-detail
check reuses the splat edit run's log; carve-hover tint must be visible and
warm, the Add preview must tint green, and neither may touch water-plane
pixels; water plane: a 6 m ball delete below the level must read as water
against `VF_SPLAT_NOWATER=1`, and a channel that reaches the river must match
the open water's mean colour within 12/255 per channel (one level, one shader —
see [[concepts/water-plane]]); undo (`VF_TEST_UNDO`) and `VF_TEST_CLEAR` must
restore the frame centre's luma to the untouched value and delete the overlay
file (the zero-surfel patch regression above). Gate wall time is dominated by
the per-run world load, not by these renders — see
[[concepts/load-time-field-build]].

`tests/visual_check.py` also probes the selected-layer screen centre and all
three ring hit classes during the ownership renders, independently projects
the logged AABB/camera pose, and bounds terrain/water ownership motion. The
gizmo therefore cannot drift away from the rendered AABB while GPU rotation
remains otherwise green.

`tests/test_editable.cpp` "carve cylinder opens the whole disk it starts at"
pins the scoop geometry: full-disk top layer at +2 cells, floor at exactly the
picked depth, inclusive rim, add shell unchanged.

`tests/test_store.cpp` pins the store semantics (adoption, edits, Smooth
terrain spike/pit + object bump/notch/wall/post/staircase cases, localized==full
rebuild, overlay round trip) and the deep-clear regression:
cleared cells stay air even where the chunk's `SolidBox`es cover them
(`fromBrick[]` must gate the box fill — without it 16 of 31 shaft cells came
back solid). The full `live_edit_check.py` gate adds an `[object_smooth]` pair:
a cabin surface cell rendered from the hero camera must log `smooth object:`
(never `smooth terrain:`) and change a bounded but visible fraction of pixels.

See also [[entities/svo-render]] for the two backends and the shared G-buffer.
