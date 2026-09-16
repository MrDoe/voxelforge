---
title: Live-edit brush (Carve / Add / Delete / Paint)
tags: [live-edit, chunk-store, surfels, splat, svo, persistence, brush]
sourceRefs: [src/voxel/live_editor.cpp, src/voxel/chunk_store.cpp, src/app/main.cpp, shaders/splat.frag, src/render/splat_pass.cpp, tests/live_edit_check.py]
lastReviewed: 2026-09-15
---

# Live-edit brush

The `C` edit panel ("Carve / Add") stamps one of four brush volumes at the
hovered surface point. **Every** stamp lands in the runtime `ChunkStore` and
patches both GPU backends in the same frame — there is no bake/record edit
path any more (the legacy `carve_edits.vxw` / `raise_edits.vxw` layers are
read-only leftovers); the "Clear live edits" button drops
`assets/runtime_edits.vxw` and reloads.

## Brush modes

| mode | volume | store op | notes |
|---|---|---|---|
| Carve | oriented cylinder, base at the hit cell, `depth` along **-normal** | `Clear` | depth-limited scoop on terrain; stops at the water level |
| Add | half-ellipsoid dome along **+normal** (`depth` = height) | `Set` | combines with existing geometry |
| Delete | ball of `diameter/2` about the hit cell | `Clear` | ignores depth; stops at the water level |
| Paint | same ball | `Paint` | recolours solid cells with the panel's Material combo; filtered by `store.cellAt().solid` so it never creates geometry |

Brush strokes persist through the store overlay (saved on mouse release) and
the hover ray-picks the store (`rayPickStore`), so the tool always works on
what is rendered.

## Water level: digs are flooded

Subtractive stamps (Carve/Delete) may dig below `WATER_LEVEL` (-0.9); the dug
volume is then **filled with water** instead of staying a dry hole:

- `App::floodNewlyDug` runs after a subtractive stamp. For every column it dug
  below the plane it checks the store for solid left at/above the plane (scan
  `[plane, cleared top]` + the first untouched cell above) and, if the column is
  now open, appends a water-plane splat at the 0.2 m grid point covering it —
  deduped through a wet-grid bitset (`m_waterGrid`), so repeated strokes and
  already-wet columns never double up.
- `SplatPass::patchWaterSurfels` re-uploads the whole water run (about 9k
  splats) with rebuilt per-chunk ranges. The upload reserves 4096 slots of
  headroom, so the common case is an in-place copy; only a flood larger than
  that pays the `growOpaque` relayout. The water slots get identity indirection
  entries (the vertex shader indexes `uCompact[gl_InstanceIndex]`).
- The flood is splat-only: the SVO water plane is analytic and floods by itself.
  A scoop that stays *covered* by terrain above the plane gets no water (it is a
  void, not a pond), and a column the water plane already covers is skipped.
- `VF_NO_WATER_FILL=1` skips the flood (A/B knob used by the tests).

## Persistence## Persistence

Edited chunks are flagged `edited` and serialized by `OverlayWriter` to
`assets/runtime_edits.vxw` (VXW v2 store section, schema 2 = per-chunk edit
AABB) on mouse release, temp+rename+debounced. The app restores it at startup
and after every world reload (`App::loadStoreOverlay`: GPU seed + refresh the
saved AABB). The file is intentionally **not** in `world.json`, so the layer
poll never reloads it. `VF_NO_OVERLAY=1` skips the restore — the test scripts
set it so an interactive session's painting cannot change reference shots.

## Tests

`tests/live_edit_check.py` (headless PPM, `VF_NO_OVERLAY=1`):
Add in splat **and** `--mode svo`; Delete/Paint in splat with `VF_MICRO=0` so
the measured diff is geometry, not the dropped micro tail; carve-hover tint
must be visible and warm and must leave water-plane pixels alone; water fill: a
6 m ball delete in flat ground beside the river floods with >=100 water splats
and — A/B against `VF_NO_WATER_FILL=1` — changes >1 % of the frame with
brighter/blue water-surface pixels over the pit.

`tests/test_store.cpp` pins the store semantics (adoption, edits,
localized==full rebuild, overlay round trip) and the deep-clear regression:
cleared cells stay air even where the chunk's `SolidBox`es cover them
(`fromBrick[]` must gate the box fill — without it 16 of 31 shaft cells came
back solid).

See also [[entities/svo-render]] for the two backends and the shared G-buffer.
