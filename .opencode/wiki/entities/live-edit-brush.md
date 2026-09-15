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

## Water level

Subtractive modes (Carve/Delete) **always respect the water level**:

- a scoop whose hovered cell lies below `WATER_LEVEL` (-0.9) is refused
  outright (log: `edit: carve refused - … below the water level`);
- no `Clear` edit ever touches a cell whose centre is below the plane
  (`yMinDry`, i.e. lattice y ≥ 503); the log reports how many cells were held
  back (`… N cells held at the water level`);
- the hover tint clips at the plane too (`bFlags.x` in the brush UBO), so the
  water surface is never shown as affected.

Paint is not clamped (recolouring a submerged cell is not a geometry change).

## Stamp path

```
EditableWorld::makeXxx (rasterizer, cells inside the volume)
  -> StoreEdit{Set|Clear|Paint}
  -> ChunkStore::apply + rebuildDirty   (edit AABB ± 12 cells, block-snapped)
  -> LiveEditor::stamp                  (per-chunk surfel cache ± 3 cells)
  -> SplatPass::patchChunkSurfels       (paged surfel buffer)
     SvoPass::patchChunk               (chunk-local SVO arenas)
```

- `LiveEditor` seeds a chunk's cache once from the GPU's current run
  (`SplatPass::readChunkSurfels`, base surfels only; cell keys recovered from
  `pos - normal*0.05`) instead of re-baking the chunk — this removed the
  first-stamp hitch (~150 ms → ~20 ms).
- **Gotcha:** a GPU seed is the *pre-edit* geometry, so `stamp()` must
  refresh the edited region on top of a fresh seed. Without that refresh the
  first stamp of a chunk re-uploads the old surface and looks like a no-op
  (only the dropped micro tail changes). `loadStoreOverlay` already did this
  for restores; `stamp()` does it now too.
- Patched chunks lose their micro-surfel tail and LOD ring until the next full
  reload; `uHeight` (terrain texture) and water stay stale in the edited
  region. Patching waits for the device (`vkDeviceWaitIdle`), so this is
  click/drag-scale editing, not a free-running sculpt loop.

## Hover preview (splat backend)

Hovering with the tool active tints the splats the next stamp would affect, so
the LMB result is visible before clicking:

- bind 13 `BrushUBO` = volume centre + radius, axis + half length
  (0 = ball), tint rgb + strength; all-zero strength = off; flushed in
  `SplatPass::record` (`App` computes it per frame next to the selection feeds).
- The test runs on the **surfel centre** (`vCenter`), not the fragment
  position: per-splat, matching the CPU rasterizer's per-cell decision; the
  app grows the volume by a 0.06 m skin so the surfels' `+0.05 m` emitter
  offset stays inside.
- Tints: warm orange = carve, red = delete, the material colour = paint; Add
  shows nothing (it creates geometry, no splats are affected).
- `VF_SPLAT_DEBUG=15` renders the volume as a magenta mask.
- `VF_TEST_BRUSH="x,y,z,carve|delete|paint"` (+`VF_EDIT_DIAM`/`VF_EDIT_DEPTH`)
  activates the tool headlessly and renders only the preview.
- Not implemented for the SVO reference backend or the WIP tile path
  (`VF_TILE=1`).

## Persistence

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
must be visible and warm; water level: a deep scoop reports the held-back
cells, a submerged scoop is refused pixel-identically, and a subtractive
preview over open water tints no water-plane pixel. `tests/test_store.cpp`
pins the store semantics (adoption, edits, localized==full rebuild, overlay
round trip).

See also [[entities/svo-render]] for the two backends and the shared G-buffer.
