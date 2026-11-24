---
title: vf_mcp v2 tool surface
tags: [mcp, tools, authoring, entities]
sourceRefs: [src/ai/mcp_server.cpp, src/app/edit/mcp_intents.cpp, src/ai/tools.hpp, src/voxel/selector.hpp, src/ai/builders.hpp, tests/mcp_v2_check.py, tests/mcp_view_check.py]
lastReviewed: 2026-11-24
---

# vf_mcp v2

The MCP server (69 tools) now reaches **both** authoring
lanes and the running app's live edit tools. Two invariants shape it: one code path with the GUI (via the
[[concepts/mcp-intent-bridge]]) and one selector grammar for every cell verb.

## Lanes

| Lane | Tools | Persisted in | Undo |
|---|---|---|---|
| Store (reversible/interactive) | `dig_cave`, `stamp`, `stamp_result`, `commit_overlay`, `view_pick`, `view_click`, `view_undo`, `view_clear`, `view_box`, `edit_result` | `runtime_edits.vxw` / staged `world.json` pose | one stroke/step via the app |
| Baked (permanent/authored) | `delete_voxels`, `paint`, `write_object`, `add_path`, `add_tree`, `recolor_layer`, `set_layer_transform`, … | named `.vxw` + `world.json` | `snapshot` / `restore_snapshot` |

## Selector grammar (`src/voxel/selector.hpp`)

`box:[x0,y0,z0,x1,y1,z1]` or `min`+`max`, `anchor:[x,y,z]`+`radius` (metres),
`anchor`+`component` (6/26 connectivity, material-masked flood fill), and `mat`.
Specs union. The material mask is load-bearing: an unmasked flood fill connects
a tree to the ground and over-deletes (measured 13,349 cells in a forest layer).
`dry_run` prints bounds and a per-material histogram, so a wrong box is visible
before the write.

## New tools

- `describe {cell|xyz}` — one call returns owner layer (micro-aware),
  material, terrain height, store-edited flag from the overlay chunk list,
  a bounded material-masked component census (bounds + histogram), and a lane
  recommendation. Kills the 3–4 probe guessing game.
- `paint {layer, selector, mat?, r/g/b?, refl?, rough?, tex?, dry_run?}` —
  region-scoped recolor; `mat` is the TARGET, `from` is the filter (conflating
  them silently selects zero cells — measured and fixed).
- `dig_cave {screen|cell|xyz, radius?, depth?, style?}` — the selection-driven
  one-call cave builder. The server skips its world build; the App composes
  Delete/Carve/Add sub-stamps through `applyEditLive` as one undoable stroke and
  the server returns the engine report inline (two-second wait, then pending).
  Defaults: radius 1 m, depth 3 m, `style:"dripstone"`. This is store/session
  state, not baked source geometry; `commit_overlay` cannot promote deletions.
- `stamp {mode: carve|add|delete|paint|smooth, cell, diameter?, depth?, mat?,
  strength?, box_scope?}` — posts an intent; the app runs `applyEditLive`.
- `stamp_result {id}` — instrument report.
- `view_pick {screen:[x,y]|cell|xyz}` / `view_click {…, mode}` — click into the
  running view at logical window pixels through `App::pickAtWindowPos`, then
  drive carve/add/delete/paint/smooth, rotate/move (`commit:false` stages) or
  select. `view_undo`, `view_clear`, `view_box` and `edit_result` complete the
  live-edit surface. See [[concepts/mcp-intent-bridge]].
- `snapshot {layer}` / `restore_snapshot {layer, file}` — labelled `.bak`
  inside the scenery, never `/tmp` (a directory recreate once ate the only
  copy of a 65 MB session).
- `commit_overlay` — promote additions into `ai_edits.vxw`, refuse deletions.
- `add_path {from:[x,z], to:[x,z], width?, step?, mat?, name?}` — samples the
  heightfield **per cell** across the width, not just centreline heights;
  refuses the live_edit keep-out (world x −3…3, z −1…6) unless explicit.
- `add_tree {anchor|ground, height?, canopy_r?, seed?, name?}` — deterministic
  seeded trunk + clumped canopy; same seed → byte-identical records.
- `place_copy {layer, anchor|ground}` — the per-layer Import button:
  `EditableWorld::importLayer` translates records to the anchor and appends
  only the copy to `ai_edits`.
- `transform {layer, pos?, yaw?, pitch?, roll?, commit}` — two-call flow.
  `commit:false` runs `worldfile::transformRecords` and reports the exact
  resulting AABB without writing; `commit:true` writes `world.json`. This is a
  computed placement preview, NOT a GPU gizmo preview.

## Gates

- Unit: `tests/test_selector.cpp`, `tests/test_mcp_tools.cpp`,
  `tests/test_mcp_builders.cpp` (in the `unit` CTest group).
- Functional: `tests/mcp_v2_check.py` (describe → paint dry-run/write →
  snapshot → stamp instrument → commit_overlay) and
  `tests/mcp_v2_selector_check.py` (masked component delete of one tree) run
  against throwaway sceneries with `VF_OVERLAY_PATH` in `/tmp/opencode`.
  `tests/mcp_view_check.py` (CTest `mcp_view_check`) adds the view-click path:
  pick/select/screen-carve share one resolved cell, all edit tools run, and
  rotate/move persist in `world.json`. Its live-App `dig_cave` case measures
  the one-call path (787 ms round trip, 89 ms first stamp), one undo step and
  overlay persistence.

## Verification

`diff_shots {a,b,threshold?}` compares two headless `--shot` PPMs and returns
changed-pixel count and mean absolute luma delta (engine TAA noise floor
~1.25 %). Pair it with `render_shot` before/after a write; for micro/animated
scenes pin the pose with `VF_TEST_BEHAVIOUR_FREEZE` and a fixed `--animtime`,
or the diff measures the animal's gait instead of your edit. The stamp tool
already returns instrument-first reporting (cells/chunks/surfels/ms/undo).

`clip_key {layer, bone, time, rot:[rx,ry,rz], ease?, clip?}` authors a bone key
through `readMicroClip`/`writeMicroClip` with the studio's own quantisation
(fps grid) and seed (`sampleTrackEuler` of the existing keys), registers the
`.anim` in the layer's `clips` list and hot-reloads. Pin animal poses with
`VF_TEST_BEHAVIOUR_FREEZE` before any pixel verification.

## See also

- [[concepts/mcp-v2-plan]] — the proposal this tool surface grew out of. Its invariants (one engine code path, one selector grammar, dry-run + snapshot + verify on every destructive write) explain why the verbs are shaped this way, and its phase list records what was proposed versus what shipped.

## Deliberately not done

GPU gizmo previews for transforms, per-layer commit routing and a clip transport
(play/pause) remain future work: they need App-side UI state or a render loop
that the stdio MCP process does not own. `transform(commit:false)` previews the
exact *AABB* through `worldfile::transformRecords`, not a GPU gizmo.
