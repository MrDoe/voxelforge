---
title: Live-edit brush (Carve / Add / Delete / Paint / Smooth / Rotate / Move)
tags: [live-edit, chunk-store, surfels, splat, svo, persistence, brush, smoothing, terrain, rotation, trackball, move, placement]
sourceRefs: [src/voxel/live_editor.cpp, src/voxel/chunk_store.cpp, src/voxel/chunk_store.hpp, src/voxel/editable_world.cpp, src/voxel/editable_world.hpp, src/app/main.cpp, shaders/splat.frag, src/render/splat_pass.cpp, src/render/splat_pass.hpp, shaders/splat.vert, shaders/splat_cull.comp, shaders/splat_tile_bin.comp, shaders/splat_tile_render.comp, tests/live_edit_check.py, tests/test_store.cpp, tests/test_editable.cpp, tests/visual_check.py, docs/rendering.md, docs/tooling.md]
lastReviewed: 2026-09-26
---

# Live-edit brush

> **Post-split (2026-09-26).** `src/app` was split per subsystem; `main.cpp` is
> now a 16-line entry point. All `main.cpp:<line>` references formerly on this page
> have been re-anchored to **symbols**. Use [[entities/app-subsystems]] for the map and
> for the two invariants that straddle files — the click gate's write side is in
> `edit/live_edit.cpp` and its read side in `frame/run_input.cpp`.

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

A second stamp is now gated by **two independent suppressions**, because the
one click-is-two-voxels bug has two distinct causes with distinct signatures:

- **The stack — suppressed by identity, no threshold.** The dominant cause is
  not jitter: the Add changes what the ray hits, so a *held* click re-picks the
  top face of the voxel it just created and builds a tower. The stamp records
  the cell it wrote (`m_lastStampWroteCell`), and a pick of **that exact cell**
  is refused outright — `m_hoverHit.voxel == m_lastStampWroteCell`. This is
  deterministic and involves no tuned constant at all.
- **Jitter — suppressed by a small travel threshold.** A candidate the pointer
  has not travelled `kDragTravelPx` (**6 px**) since the *last stamp* is
  refused. This covers ordinary hand wobble.

Properties that matter:

- A stationary click is exactly one edit **however many frames the button is
  held for** — nothing here depends on timing.
- The gate can only ever *delay* a stamp, never add one, so the existing
  world-distance rate limit for drags is untouched.
- No click is ever lost: `doStamp` is initialised to `lmbEdge && editLmb`
  and the gate block only ever *sets* it true, so a press's first stamp is
  unconditional. A press that misses the hover still lands exactly one stamp
  as soon as the pick hits.
- Deliberate dragging still paints at frame rate, and — because travel is
  measured *since the last stamp* rather than net from the press point — a
  genuine **small circular drag is never disabled**. That was a real defect of
  the net-from-press design and is the reason it was reverted; see the history
  note below before reintroducing anything like it.

**Two structural facts about the gate that a line-number citation hides.**
`kDragTravelPx` is *declared* in `src/app/ui/ui_types.hpp` but **used at exactly one
site** — the click gate. That single-use property is why it lives with the UI
vocabulary rather than beside its only reader. And `m_lastStampWroteCell`
**straddles the `src/app` subsystem split deliberately**: it is *written* by
`edit/live_edit.cpp` (`applyEditLive`, and cleared in `undoEdit`) and *read* by
`frame/run_input.cpp` for the identity test. A description of the gate that omits this
will send the next reader looking for the write in the frame code, or the read in the
edit code, and finding neither.

**Residual limit, stated plainly:** a jittery click whose hand moves 6 px
while held can still land one extra neighbour. The stack case is eliminated by
identity; the jitter case is only *narrowed*, because a screen-space threshold
is the only signal available. That is the honest limit — do not describe it to
a user as "fixed".

`m_lastStampMouse` is refreshed on every stamp, so the travel measure is
per-stamp. `m_hasStamp` is cleared on mouse release and on world reload, and
the reload path re-syncs `m_lmbWasDown` from the physical button so a reload
during a held click cannot manufacture a fresh stamp edge.

### Design history — the reverted net-from-press rule (do not reintroduce)

The first fix measured travel as **net displacement from the PRESS point**
(`kDragTravelPx`, briefly 10 px then 14 px) rather than since the last stamp.
It killed the stack, but it was **specific to neither cause**: it traded a
narrow breakage (jitter only) for a wider one, because net displacement means
a **drag that stays within the threshold radius of its press point paints
nothing — including a genuine small circular drag** (~a 12 px dead disc at the
final 14 px value). Review caught it; the design was reverted to the
two-suppression form above, and the 14 px constant was **deleted rather than
retuned**, because identity suppression needs no threshold and only the jitter
case is left to tune.

A **time-based debounce** was tried before both and rejected on two counts: it
rate-limits a real drag, and it still double-stamps the case that actually
mattered — a *slow* press longer than the window whose re-pick landed on a
different cell. Travel has no such hole.

**Verification.** The trigger reads `glfwGetMouseButton` directly, so there is
no seam *inside* the app, and `check_per_voxel` pins the *cell count per
stamp*, not the click-vs-drag gate — different properties. But an out-of-app
seam already exists: `tools/vf_input.py` has
`click(button=LMB, hold=0.08, settle=0.10)` driving a real XTEST
`ButtonPress`/`ButtonRelease`, which reaches the same GLFW state
([[concepts/x11-input-injection]]). The discriminating assertion is the reported
bug itself — **`click(hold=2.0)` with no pointer movement must still be exactly
one edit** — which separates this design from the rejected time debounce (that
one fails it). Two caveats: XTEST needs a real X display, so it is
`test-visual`-shaped rather than a ctest unit test; and the recipe is
**unvalidated** — a proposed test, not a working one.

A time-based debounce was tried first and rejected on two counts: it
rate-limits a real drag, and it still double-stamps the case that actually
mattered — a *slow* press longer than the window whose re-pick landed on a
different cell. Travel has no such hole.

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

## RESOLVED (2026-09-26) — Undo replaced a chunk's baked surfels: a margin bug

**Symptom.** `Undo` appeared to rewrite surfels in regions of a chunk that were
never touched — geometry missing, some surfels positioned wrong. Seen in the GUI
as a hollow cabin in splat mode (a visual report, not an instrument reading).
What was measured: the chunk's baked surfels were replaced by store-derived ones
across a region that includes object geometry, and the corruption is
**chunk-level** — it is not confined to the picked cell. Cell `562,524,607` is a
**cabin** cell (`pick object`); see the history note below for a two-step wrong
correction this page published and then retracted.

**Root cause (measured, not inferred).** `undoEdit` forced the **exact** stamp
margin (`kExactStampMargin` = 12 cells) on *every* undo, because Smooth needs the
exact band — it changes normals/AO further than the cheap margin covers. So a
**one-voxel Add was undone by re-deriving a 25³ box** of geometry (12 cells of
margin each side). On a *terrain* chunk that is merely wasteful. On an **object**
chunk it is **wrong**: the store's object surface/crease classification does not
match the bake's, so the wide re-derivation invented **599 edge bridges inside
that box against 52 in the entire chunk** from the original bake. Those bridges
are what the corruption consists of.

**Fix.** The undo step now stores the margin the stroke actually used
(`m_undo.back().margin`) and replays with it, so undo is exactly as surgical as
the edit it reverts. `kExactStampMargin` is now escalated only where it is
actually needed — the Smooth object-surface path.

**Measured before/after**, on cabin cell `562,524,607` (`pick object`, 52 baked
edge bridges in its chunk), 1-voxel add then undo, differenced against the
untouched baseline:

| | before fix | after fix |
|---|---|---|
| pixels differing after undo | 2.571 % | **0.185 %** |
| run split (parents + edges) | 1310 + 599 | **4298 + 52** |

0.185 % matches the add-only state, and 4298 + 52 is identical to the bake's
split — i.e. undo is now a true inverse. Terrain was never affected (0.02 %),
which is exactly why the suite never caught it.

### History of a wrong correction (2026-09-26) — the cell is object; the *reader* was broken

This section has been corrected **twice in one day**, and the sequence is the
useful part. Kept deliberately, because the second wrong turn was more
instructive than the first right one.

1. A check docstring called `562,524,607` "a solid cell on the hamlet cabin's
   outer face". This page repeated that as fact without measuring it.
2. An ownership field was added to the stamp log. It read **`pick terrain`**, so
   the cell was declared terrain and this page published a correction
   retracting the cabin-wall premise.
3. **That correction was itself wrong.** The cell logs **`pick object`** and
   always did. The reader was broken, not the world: the headless stroke hook
   built `m_hoverHit` by hand — `m_hoverHit = {}; hit = true; voxel = p;
   normal = storeNormalAt(p)` — and never set `object`/`layer`, so a
   default-constructed `PickHit` (`object = false`) made **every** headless run
   report terrain regardless of what it stamped. Fixed by `adoptPickOwnership()`,
   which resolves the owner from the **load-time oracle**
   (`m_layers.field().sampleWorld`) at all three hook sites. Post-fix:
   `546,527,642` → object, `562,524,607` → object, `432,509,452` → terrain with
   **0** edges against the object cell's **52**.

**The numbers never moved.** 2.571 % and 599-vs-52 were produced by the chunk run
split and a whole-frame pixel diff, neither of which ever touched the pick hook.
The broken instrument invalidated the **label** attached to the measurement, not
the measurement. That distinction is worth keeping: a broken instrument does not
always spoil the number, and reflexively distrusting a number because a nearby
instrument is broken throws away good data.

**On "hollow cabin".** The user *saw* a hollow cabin, and the corruption is
chunk-level in a chunk carrying cabin geometry, so the visual report and the
measurement are consistent. It remains a visual report rather than an instrument
reading, and is described that way.

### The lesson that actually generalises: a NEW log field is a NEW instrument

Not "check the instrument" — that was said all night and did not prevent this.
The sharper form: **the first reading from a newly added field deserves no more
trust than any other reading.** A brand-new field has never been calibrated
against anything, so its initial output is the *least* trustworthy data in the
system, and it is read with the most confidence because it is the newest and
answers exactly the question being asked. Here that meant believing a
one-hour-old log field over a world already measured and mapped. Calibration
needs a case where the new field and an independent source must **disagree** —
two obvious-wood cells logging terrain is what exposed the hook, not any amount
of re-reading the code that produced it.

### The generalisable lesson: a wider re-derivation is not a safer one

This is the part to carry forward. The instinct that a larger refresh margin is
more thorough is **wrong on object chunks**: because the store-derived surface
and crease classification does not match the bake, widening the region does not
converge on the baked answer, it **diverges** from it. Any future path that
re-derives object geometry from the store over a region wider than the edit
needs the same scrutiny. Raise the margin only for a stated reason, and prefer
replaying the margin the stroke used.

### Why the suite missed it (coverage gap, now measured)

`tests/live_edit_check.py` only ever edits **terrain** — there is no case that
edits an object chunk. With the numbers above that is no longer an argument: the
terrain case moves 0.02 % of pixels, so a terrain-only run sees essentially
nothing and **cannot fail**. The defect lived in a chunk that **carries object geometry**, so a
terrain-only suite never touched a chunk of that kind. Noted on
[[concepts/focused-test-groups]]; the gate now exists
(`check_object_undo_surgical`, see Tests). Note also that the four paths initially
suspected — `readChunkSurfels`' clamp to `microStart`, the seed lambda's
parents/edges split, `rebuildRun`'s assembly, and `patchChunkSurfels`' metadata
updates — were all **correct**; the bug was one layer up, in which margin the
caller asked for. See [[concepts/detail-pipeline]] for the layout contract they
rely on.

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

The preview has its own invariants and its own two gates
(`check_depth_sensitivity`, `check_svo_preview`) — why a size or depth change
can be invisible, and the ~1.25 % noise floor any such gate has to clear, is in
[[concepts/brush-preview-visibility]].

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

`tests/live_edit_check.py` also carries **`check_object_undo_surgical`**, the
gate for the margin class described above. It runs in the **full** path only
(not the fast profile) and renders **cabin** cell `562,524,607` (`pick object`,
52 baked edge bridges in its chunk) three ways — untouched, add-only, and
add-then-undo — comparing **runs**, not just pixels. The primary assertion is
**threshold-free and content-drift-immune**:

- **True-inverse test (primary).** The post-undo run split must **EQUAL the
  add-only run split** — measured **4298 + 52 == 4298 + 52**; the bug gives
  **4157 + 602**. No threshold, so it cannot drift when the hamlet is
  re-authored, and it holds regardless of whether the cell is terrain or object.
- **Region holds real baked geometry (premise).** The split log must show a
  3-digit parent count. Needed because a headless stamp **happily writes a voxel
  into open air** — a stale coordinate still logs `1 cells` and then fails the
  comparisons for the *wrong* reason (it blames the undo: "1+1 vs 0+0"). With
  this, an air control fails naming the stale coordinate instead of comparing
  two empty regions.
- **Pick is an object cell (premise).** `pick object` in the stamp log. This is
  the assert that closes the terrain-drift path, and a **measured control**
  proves it: terrain cell `432,509,452` passes the true-inverse comparison
  **silently** — `5875+0 == 5875+0`, diff 0.011 % — and only this assertion
  fails it. Without it the check would go green while testing the one class
  that provably cannot catch this bug.
- **Stamp happened (premise).** `live edit: N cells` present, the
  `check_per_voxel` pattern, before any outcome is judged.
- **Secondaries:** edge bridges ≤ 200 (measured 52 healthy, 599 wide) and
  post-undo pixel diff ≤ 0.5 % (measured 0.184 % vs 2.571 %).

The **0.184 % residual is expected, not a partial undo**: touched chunks keep
live store-derived shading until the next full reload, so a small diff against
the untouched baseline is inherent. The run-split equality is what proves a true
inverse. Reasoning lives in the check's docstring so it travels with the
assertion.

**Status: green, both crossed groups (2026-09-26).** `test-live-edit` 3/3
(`unit_store_tests`, `live_edit_check`, `fast_live_edit_check`) and
`test-visual` 3/3 (`visual_check`, `gpu_selftest`, `fast_visual_check`). The
coverage argument, not just the green: the change is in `App`'s pick plumbing
(`adoptPickOwnership`), and the two groups that read it are live-edit — where
`check_object_undo_surgical` asserts the pick class **directly**, so a regression
is red rather than silent — and visual, which covers the rotate/move/owner-label
paths that branch on `m_hoverHit.object` (`frame/run.cpp` for `m_lastPickObject`,
`ui/` for the owner label, `frame/run_input.cpp` for the hover-owner path).
`test-surfel` and `test-store` were deliberately not run: the only edit outside
`App` is the `getenv("VF_TRACE")`-gated region log in `live_editor.cpp:154`,
which is a log plus a move of a temporary and does not change the run — and
`tests/live_edit_check.py` sets `VF_TRACE=1` in three places, so those lines
**do** execute under the green live-edit group rather than sitting untested
behind a guard.

**One coupling worth knowing:** the check parses the run-split out of that
`VF_TRACE` diagnostic line, so the gate depends on a **log** existing. The
failure direction is right — a missing or reworded line fails loudly ("no
run-split log to bound the refresh") instead of passing quietly — but the test is
tied to a log format, so a cosmetic change to that `spdlog` line is a test
change.

**Method note — the strongest signal came from a negative control, not from
reasoning.** The premise assert exists because someone ran the check against an
**air** coordinate and watched it fail for the wrong reason. Reasoning about
whether a check can pass vacuously did not find that; a deliberately broken input
did. Prefer negative controls when auditing a test's failure modes.

**Three vacuity paths, all closed by premise asserts.** A hardcoded constant
cannot false-alarm, but "cannot misfire" is only half the argument — a check that
*silently stops testing* is worse, because a false alarm gets investigated while
a vacuous pass gets trusted. (1) *Cell becomes non-solid* → closed by the
premise asserts. (2) *Cell drifts onto terrain* → previously the weak path, since
terrain is the class that provably cannot catch this bug. **Closed too, but by
the true-inverse comparison rather than by knowing the ownership class**: if
post-undo equals add-only, undo is exact no matter what was picked. An earlier
draft of this page proposed asserting the pick's ownership class; that was the
the class that provably cannot catch this bug. **Closed by the `pick object`
premise assert**, and confirmed by the terrain-cell control above: the
true-inverse comparison alone *passes* on terrain, so it is necessary but not
sufficient. (3) *Cell becomes air* → closed by the baked-geometry premise; the
air control fails on both the pick assert and that premise.

See also [[entities/svo-render]] for the two backends and the shared G-buffer.
