---
title: "vf_mcp v2 — fast, accurate, full-parity authoring plan"
tags: [mcp, authoring, tools, plan, selectors, editing, verification]
sourceRefs: [src/ai/mcp_server.cpp, AGENTS.md]
lastReviewed: 2026-11-20
---

# vf_mcp v2 — fast, accurate, full-parity authoring plan

A design plan derived from a session of scene-authoring requests (tree add/remove, gravel path, roof paint) where the bottleneck was repeatedly **discovery round-trips and hand-computed lattice cells**. Status: **proposed**, not implemented.

## Core insight

**The model expresses intent; the MCP server computes cells.** Every place the assistant hand-computed lattice cells (e.g. the 130-cell gravel ribbon) was slow *and* the main accuracy risk — mental arithmetic on a 0.1 m lattice is where mistakes live. Move geometry server-side.

## 0. Guiding invariants

1. **I1 — One code path.** Every MCP write calls the same engine function the GUI drives. No reimplementation. Proven pattern in this codebase: `App::chooseEditMode` shared by hotkeys + panel, `studioUploadRig` for both UI paths, `VF_TEST_*` hooks driving UI-only features headlessly.
2. **I2 — One selector grammar** shared by all verbs (`delete / paint / move / describe / inspect`).
3. **I3 — Every destructive write** is dry-run-able, snapshot-backed, and verifiable.
4. **I4 — Two lanes, one convergence**: store stamps (`runtime_edits.vxw`) for interactive/reversible; `.vxw` selectors for authored/permanent; `commit_overlay` promotes between them.
5. **I5 — Coordinates accept both** world metres and lattice cells, normalized server-side (kills the −51.2 offset trap).
6. **I6 — Blocked stays blocked** (`set_scenery`, `set_ui` remain panel-owned).
7. **I7 — Data-only**: no scenery facts in code; selectors/builders read scenery data at runtime.

## Phase 1 — Selector grammar + discovery (foundation, unblocks everything)

- `src/voxel/selector.{hpp,cpp}`: parsed selector — `{box[min,max] | anchor+radius | cell+component(mats[], connectivity) | mat | layer}`. Unit-pinned.
- **`describe(cell)`** tool — one call returning: winning owner (`layerId`, micro-aware `layerBounds`), material, store-edited vs pristine flag, canonical **pivot** (world layer = bottom-center; micro = min-corner + margin), component bounds + material histogram, placement hash.
- **`delete_voxels` upgrade**: `component:true, mats:[6,8]` → flood fill that *traverses only cells matching the mask*. Turns tree-in-forest from probe-grid + 3 carves into **1 call**, and fixes the "everything connects through the ground" over-deletion (measured: 13,349 cells wrongly selected without a mask).

Gates: selector unit tests; scratch-scenery functional test (masked component removes exactly one tree); refusal rules preserved (landscape/packed/whole-layer); `test-unit` + new `test-mcp` arm.

## Phase 2 — Region-scoped paint

- **`paint(layer, selector, {mat?, tex?, rgb?, refl?, rough?}, dry_run)`** — scoped recolor using the selector; keep `recolor_layer` as sugar for `selector=mat`. Per-cell tex override rides word1 byte 0 (already in the format).
- `dry_run` prints bounds + counts so a wrong box is visible *before* the write.

Why: the barn-roof task worked only because mat 7 was exclusively the roof; on a mixed building today I'd have to carve + rebuild. One call, region-exact.

Gates: mixed-material scratch object — paint only the roof region; histogram assert before/after.

## Phase 3 — Store-lane stamping (terrain **and** objects)

- **`stamp(mode: carve|add|delete|paint|smooth, at, {diameter, depth, mat?, strength?, box_scope?})`** driving `EditableWorld`'s *exact* rasterizers: `kCarveTopMargin` carve, axis-relative `makeDome`, `makeSphere`, one-sided surface-axis `makeSmoothEdits` (never per-voxel occupancy filtering — floaters), per-voxel single-cell mode, object-column protection.
- Reuses full stroke machinery: per-cell pre-state map → **one undo step per call**, overlay save, `LiveEditor` refresh, height-texture + water patch. `box_scope` mirrors the GUI box-selection clamp.
- Returns the **instrument report** (edited chunks, cell count, run split, surfels touched) *before* any pixel claims — assert-the-instrument-first discipline.
- Large edits steer to the baked lane (device stalls are click-scale by design).

Gates: extend `live_edit_check.py` with MCP-driver arm (`VF_TEST_EDIT` proves headless reachability); store-oracle agreement asserts; order-independent via `invalidateStore` in/out.

## Phase 4 — Transform staging + placement + snapshots

- **`transform(layer, {pos?, yaw/pitch/roll?, scale?}, commit)`** — `commit:false` drives the staged preview (`RotUBO` for world, micro pass preview + fur mirrors; micro records stay baked-frame); `commit:true` drives the panel's Apply write (world: manifest + reload; micro: `rotateMicroRecords` destination-driven + `minShift` rebase).
- **`place_copy(layer, at)`** — mirror `EditableWorld::importLayer` (records → anchor → `ai_edits` append), i.e. the per-layer Import button.
- **Snapshots**: every baked write auto-emits `<layer>.vxw.bak` (labelled, `ls`-verifiable); **`restore_snapshot(layer)`**. "Not undoable" disappears → no timid edits.

Gates: `VF_TEST_ROTATE/MOVE/TRACKBALL_PROBE` asserts; micro pivot ±1 mm test extended to MCP-commit; snapshot round-trip md5 test.

## Phase 5 — Intent builders (server-side generators)

- **`add_path(A, B, {width, mat, tex, step=0.2, snap='ground'})`** — server samples the heightmap per cell (the manual 130-cell build approximated cross-slope with centreline heights; this is *more accurate*, not just 1 call vs ~15).
- **`add_tree(at, {height, canopy_r, seed})`** — trunk + branches + clumped canopy; seeded RNG → deterministic reproducibility.
- **`add_object(kind, …)`** — data-driven kind registry (fence/wall/scatter), no hardcoded scenery.
- Keep-out guard baked in: warn when a path/build crosses the `live_edit_check` carve region (world x −3..3, z −1..6) — the reeds lesson.

Gates: determinism (same seed → byte-identical layer); path within ±1 voxel of sampled ground everywhere; conformity checks for any bound art.

## Phase 6 — Micro/animation parity

`clip_key(layer, bone, time, rotDeg)`, `clip_ease`, `play/pause` — driving `sampleTrackEuler` + `studioApplyBoneDrag` + `studioUploadRig` (the bone-drag headless hook is the template: a UI feature only a mouse could reach, gated headlessly). Poses pinned via `VF_TEST_BEHAVIOUR_FREEZE` for any verify involving animals; two pinned renders must be byte-identical.

## Phase 7 — Verification service (built into writes)

`verify:true` on any write → server renders before/after, returns structured verdict: changed-px count against the **1.25 % noise floor** (baseline = same-config rerun), mean luma deltas, optional probe checks. Store variant: instrument-first (chunk `changed`, cell solid, surfel gained) *then* pixels. Headless `--shot` never advances the clock — A/Bs stay deterministic.

## Phase 8 — Payload efficiency

- `write_object` accepts `from_file` (JSON/CSV) for big layers; selector-diff writes (`write_object(selector, changes)`) for surgical edits without 10 MB inline JSON.
- Multi-shape `add_*` in one call, grouped undo (the "make it natural" task was 6 separate strokes).
- Bulk `sample_ground(points[])`.

## Rollout order & effort

| Phase | Value | Depends on | Gate group |
|---|---|---|---|
| 1 selectors+describe | highest — unlocks 1–2-call flows | — | test-unit, new test-mcp |
| 2 region paint | high | 1 | test-unit |
| 3 stamp (store lane) | high — terrain finally reachable | 1 | test-live-edit |
| 4 transform+snapshot | high | 1 | test-micro, rotate hooks |
| 5 builders | medium-high | 1–2 | test-visual |
| 6 micro/clip | medium | 4 | test-micro |
| 7 verify | cross-cutting | 3 | per-group |
| 8 payload | medium | 1 | test-unit |

Each phase lands independently and leaves `AGENTS.md`/wiki/quirks updated; nothing regates the whole matrix.

## Measurable acceptance criteria

- Tree removal: **≤2 calls** (describe + component-delete-with-mask), zero probe grids, zero over-deletion.
- Path draw: **1–2 calls**, per-cell ground heights exact.
- Roof paint: **1 call**, region-scoped on a mixed-material building.
- Terrain stamp: **1 call + verify**, undoable as one step.
- Move/rotate: **2 calls** (stage → commit), pivot-correct for world and micro.
- Every destructive write: dry-run-able, snapshot-backed, verifiable.
- Zero regression: all existing gates green; GUI and MCP provably share code paths.

## Key risks

- **Parity drift** → I1 enforced by shared gates (MCP arm in `live_edit_check.py`), not by review.
- **Device stalls from MCP stamping while GUI runs** → documented: stamps are click-scale; big edits go to the baked lane.
- **Overlay creep** → `commit_overlay` + guidance that permanent content lives in layers.
- **Selector ambiguity** → dry_run defaults + verbose bounds/counts on every preview.

## Cross-references

- [[entities/vf-mcp-v2]] — current tool surface.
- [[wiki/concepts/brush-preview-visibility]] — the noise-floor measurement this plan's `verify` relies on.
- [[wiki/concepts/live-edit-surfel-parity]] — the shared-code invariant Phase 3 must preserve.
- [[wiki/concepts/geometry-format-policy]] — editor-path vs baked-path rules for objects and transforms.
