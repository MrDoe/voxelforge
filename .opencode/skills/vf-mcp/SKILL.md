---
name: vf-mcp
description: Drive VoxelForge through the vf_mcp MCP server — inspect the live world, author object/scatter/micro layers, bind textures with per-cell overrides, place lights, tweak UI settings, drive live view/edit tools, and verify with screenshots. Use for any request that changes the rendered scene via MCP tools, instead of hand-editing .vxw files or C++/GLSL.
---

# Using the Voxelforge MCP server

`vf_mcp` is a stdio JSON-RPC server (`src/ai/mcp_server.cpp`) registered as
`voxelforge` in `.opencode/opencode.json`. It is the authoring channel for the
running app, with two lanes: baked DATA writes under the active scenery folder
(hot-reloaded within ~1 s) and live-store intents executed by the App's own
brush paths on the next frame. No recompile, no hand-written binary `.vxw`,
no special-case code.

## FAST PATH — read first (selection-driven edits)

When the user says "do X here/this/selected" and the prompt carries
`[UI context: selected voxel cell [x,y,z], world position [...] metres]`,
**call the one-shot builder with that cell immediately — no inspection round-trips**:

- "dig a cave here" → `dig_cave {cell:[x,y,z], radius:1.0, depth:3.0,
  style:"dripstone"}` (one call; the App composes mouth+tunnel+chamber+
  dripstone as one undoable store-lane stroke and returns its report inline).
- "draw a gravel path to here" → `add_path {from:[...], to:[x,z of selection]}`.
- "add a tree here" → `add_tree {ground:[x,z], seed:<fixed>}`.

Do NOT front-load `view_pick`, `ground`, `surface_scan` slope scans,
`list_layers`, or doc re-reading before the first dig. The selection cell is a
lattice cell (bottom-center anchor); pass it straight through. `dig_cave`
computes geometry **inside the running App** through the GUI brush rasterizers;
the stdio server only validates and posts the intent. The "Inspect first"
workflow below is for **ambiguous/open-ended** requests, not for a selection +
a known verb. `dig_cave` already returns cells/chunks/first-stamp-ms/undo
inline — do not call `edit_result` or `render_shot` before replying. Use
`capture_screen` only when the user asks for visual verification.

### Cave contract (MUST follow — a cave dug wrong is invisible forever)

A walk-in cave is a **natural feature**. Two lanes, and only two:

1. **Store lane (fast/reversible)**: `dig_cave` (or `view_click`/`stamp` for a
   manual brush) carves the live `ChunkStore` immediately. The whole compound
   cave is ONE undo step and is queued to `runtime_edits.vxw`; the App restores
   that overlay after a world reload/restart. This is session state, not
   authored source content, and `commit_overlay` cannot promote deletions
   because record layers cannot express air.
2. **Baked lane (permanent/authored)**: a rock-shell object layer with an OPEN
   MOUTH above the heightfield column top, sitting on the terrain. The cave air
   is the empty space INSIDE the shell. The interior must CONNECT to outside
   air (an open mouth or skylight) — `VoxelField` flood-fills enclosed air and
   promotes it to solid.

**Rules:**
- Use `dig_cave` FIRST for "dig here"; it never needs an inspection call and
  returns the instrument report in the same tool call.
- NEVER assume a store carve changed the authored `.vxw`; it lives in the
  gitignored overlay. If the cave must become permanent source content, build
  the baked shell deliberately.
- NEVER run a headless `render_shot`/`--shot`/`--smoke`/`--selftest` against a
  scenery with a live `runtime_edits.vxw` without `VF_OVERLAY_PATH=<tmp>` or
  `VF_NO_OVERLAY=1`. The overlay is gitignored, so damage is silent and
  unrecoverable.
- Verify the LIVE window with `capture_screen`. A default `render_shot` is a
  fresh process and does not prove the long-running window changed; it can
  only A/B the overlay when explicitly pointed at the same isolated path.
- Before `view_clear` / the GUI's **Clear live edits**, copy the overlay to a
  labelled non-`.vxw` path and md5-verify — that action is irreversible.
- If a cave must be permanent, use `write_object` with a shapes list forming
  walls+ceiling+floor with an open mouth, or author a heightmap depression +
  object shell. Do not try to converge a store-lane deletion with
  `commit_overlay`.

## Tool map

- **Inspect**: `list_layers`, `get_layer`, `inspect_layer`, `layer_stats`,
  `list_micro`, `list_materials`, `list_textures`, `list_models`,
  `list_lights`, `world_info`, `probe`, `probe_region`, `ground`,
  `read_object`, `inspect_rig`, `fur_stats`, `get_ui`, `camera_pose`,
  `list_sceneries`.
- **Author geometry**: `add_box`, `add_cylinder`, `add_ellipsoid`,
  `add_stamp`, `add_voxels`, `write_object`, `import_mesh`, `recolor_layer`,
  `duplicate_layer`, `rename_layer`, `delete_layer`, `delete_object`,
  `clear_edits`, `set_layer_transform`, `set_layer_behaviour`, `set_pose`,
  `enable_layer`.
- **Look**: `set_texture`, `update_light`, `add_light`,
  `remove_light`, `set_ui`, `set_sun`, `set_scenery`.
- **Live view / edit tools** (the App's own paths, not data-file writes):
  `dig_cave`, `view_pick`, `view_click`, `view_undo`, `view_clear`, `view_box`,
  `edit_result`. `dig_cave` is the selection-driven one-call cave builder and
  returns its engine report inline. `view_pick {screen:[x,y]}` clicks into the
  running view at **logical window pixels** (use a `capture_screen` image to
  pick the pixel) and reports cell/world/material/owner. `view_click` uses the
  same pick with `mode: carve|add|delete|paint|smooth|rotate|move|select`; pass
  `commit:false` to stage a rotate/move preview instead of writing `world.json`.
  The `view_*` tools return an id — read the engine report with
  `edit_result {id}` (or `stamp_result`, same queue). A cell/xyz target is still
  accepted for deterministic, camera-independent edits. These intents are
  consumed by the running app; if no app is running the result stays "pending".
  `view_clear` is irreversible; `view_undo` reverts one whole stroke.
- **Verify/control**: `render_shot`, `capture_screen`, `reload_world`.

Tool names keep their raw MCP names in a direct `tools/call`; Code Mode
exposes them as `tools.voxelforge.<name>` inside `execute` (the Code Mode
catalog is partial — do not assume `tools.grep`/`tools.read` exist there).
The server fuzzy-normalizes invented names (`create_box` → `add_box`).

## The data model you are editing

- Active scenery folder: `scenery/<name>/` (manifest `world.json`, terrain
  `heightmap.png`/`landscape.vxw`, object/scatter/micro `.vxw`, `ai_edits.vxw`).
  `assets/` is the shared library (textures/models), not scene content.
- World lattice: 1024³ cells at 0.1 m; world is **origin-centred**,
  −51.2…+51.2 m per axis; water plane at y = −0.9. `inspect_layer` bounds are
  **cell indices**: world metres = `cell / 10.0 − 51.2` (the inverse is
  `cell = floor((metres + 51.2) / 0.1)`). `ground(x,z)`/`probe(x,y,z)` take
  **metres**. Dropping the −51.2 offset samples ~512 cells away and reads as
  valid air — this is the #1 source of authoring mistakes. Never mix the two.
- `ai_edits.vxw` is the highest-priority edit layer (appends, never overwrite).
  Layer merge is **first-wins-a-cell**: a new layer cannot replace a cell
  already owned by an earlier enabled layer unless written to that layer or
  placed at an unoccupied cell.
- `VoxelRecord`: `mat` (palette id / atlas slot), `r/g/b`, `refl`, `rough`,
  `tex` (per-cell atlas-slot override, the `reserved` byte). `tex:N` samples
  atlas slot N while keeping the cell's own material response and m/tile scale.
- Anchors: `anchor:[x,y,z]` is a **bottom-center lattice cell**; `ground:[x,z]`
  snaps to terrain height; `at:[x,y,z]` takes metres. The in-app panel appends
  `[UI context: selected voxel cell [...], world position [...] metres]` to
  prompts: "here"/"this"/"selected" means that bottom-center cell for `add_*`.
  If it says no voxel is selected, ask the user to Ctrl+LMB pick one first.

## Standard workflow

1. **Inspect first**: `list_layers`, `get_layer`/`inspect_layer` for bounds and
   material histogram, `ground`/`probe` for terrain and occupancy,
   `list_textures`/`list_materials` for available art. Be decisive: roughly
   ≤8 inspection calls, then write; one write per layer per turn where possible.
2. **Bind art** (if needed): `set_texture {mat, file, scale}` on an unused,
   non-emissive atlas slot; `scale` is metres per tile and `file:""` restores
   the palette fallback. **Never bind grey/gravel art to slots 9–15**
   (emissive band) unless no enabled cell uses that id as its material. A
   material is the unit of texture binding: a textured material ignores a
   record's colour, so a new look needs a new material OR a per-cell `tex`
   override. Check `list_textures` for existing bindings; duplicate mat
   entries are ambiguous — replace, don't append.
3. **Author**: simple props via `add_*`; detailed meshes via `write_object`
   with `shapes` or absolute `voxels:[{x,y,z,mat,tex,r,g,b,refl,rough}]`
   (up to 200k cells per call); edits to existing layers via `read_object`
   → `recolor_layer {layer, from?, mat?, r?, g?, b?, refl?, rough?, tex?}`.
4. **Place** with `set_layer_transform {layer, pos, rot, rotX, rotZ, scale}`
   (degrees about the placed bottom-center pivot; `scale` resamples onto the
   world lattice).
5. **Verify**: `probe`/`read_object` for geometry, then `render_shot` with a
   chosen `--cam`-style argument (default output
   `workspace/screenshots/shot_<time>.ppm` — gitignored). A screenshot is the
   proof the edit landed and the material reads correctly.
6. **Undo**: authored-file changes use the app's AI-panel "Undo Last AI Turn"
   (manifest, `ai_edits.vxw` and layer files from that turn). Live-store changes
   use `view_undo`, which reverts one whole stroke — including every sub-stamp
   of a `dig_cave`. Mention the applicable Undo in the final reply. Outside the
   panel, snapshot authored files before large edits and restore deliberately.

## Recipes

### Stony path from A to B
Resolve landmark AABBs with `get_layer`/`inspect_layer` (or use explicit world
coordinates); convert lattice bounds with `cell / 10 − 51.2` to metres. Sample `ground(x,z)`
every ~0.15–0.20 m along the centreline, build a deduplicated ribbon of
top-surface cells of width ~1.2 m, bind gravel to a free non-emissive slot,
then `write_object` the cells with `mat:<non-emissive>` + `tex:<slot>` into a
named layer (`path_a_b`). Verify with `render_shot`.

### Recolour / re-texture an existing object
`read_object {name}` prints every cell with `mat` and `tex`. Use
`recolor_layer {layer, from:<old mat>, mat:<new mat>, tex:<slot>}` for a whole
layer or a material subset; `tex:0` clears the override back to the material's
own texture. Avoid `ai_edits.vxw` (the app owns that live layer).

### Micro objects
`list_micro`, `set_layer_behaviour {layer, behaviour:"grazer", command:"run"}`,
`set_pose {layer, pose:[bone,rx,ry,rz,...]}`, `inspect_rig`, `fur_stats`.
Micro layers have their own fine lattice/SVO — do not world-brush them.

### UI settings / sun / lights (power tools)
`get_ui`/`set_ui` patch a flat `world.json` `"ui"` block (`renderFlags`,
`backend`, `exposure`, `volFog`, `splatRadius`, `uiFontScale`, …) which the app
hot-applies with diff guards. **Camera placement**: use
`set_ui {camPos:[px,py,pz], camTarget:[tx,ty,tz]}` and aim by a POINT — the app
derives yaw/pitch (`atan2/asin`, same as `--cam`) and a complete target wins
over raw angles. Raw `camYaw`/`camPitch` are DEGREES in the app convention
(yaw 0 = +X, positive toward +Z; pitch positive up), NOT radians or a compass
bearing from −Z. `get_ui` echoes the AUTHORED pose; `camera_pose` reads the
LIVE camera from the running app. `render_shot {cam:[...]}` takes SIX numbers
= position + look-at target in metres, not angles. `set_sun {elev,azim}` authors the scene
sun. `add_light`/`update_light`/`remove_light` and `list_lights` manage authored
point lights — never exceed the manifest's light budget. In the in-app AI
agent these tools are denied by permission (the panel owns them); external
agent hosts may use them deliberately.

## Layer placement

`pos` / `rot` / `rotX` / `rotZ` (via `set_layer_transform`) are applied at
runtime about the layer's **authored bottom-center**; zero means bake-time
identity. `scale` resamples onto the world lattice about that same pivot.
`importLayer` (app-side) translates a layer's records so their bottom-center
lands on the picked anchor. Enabling a layer shows it at its baked absolute
coordinates — it does not snap to the selection.

## Rules and gotchas

- **Never** run `git stash`, `git checkout` or `git reset` on shared paths — a
  previous session lost tree-wide uncommitted work that way.
- Never generate `.vxw` bytes by hand; always use authoring tools.
- `render_shot` writes into `workspace/screenshots/`; screenshots shown in the
  app panel are downsampled to 360 px, but the file on disk is full size.
- The MCP process inherits the **OpenCode service's** environment: a one-launch
  `VF_SCENERY`/`--scenery` override on the app does not reach it, so the tools
  inspect the pointer (`scenery/last.json`) scenery. Align these before
  scripting scene edits.
- **Fresh shells are the exposed case**: with no `VF_SCENERY` and no pointer
  file, tools resolve the **default shipped hamlet** and silently append into
  its `ai_edits.vxw`/`world.json`. Export `VF_SCENERY` in the *same command
  line* as each authoring tool, verify the first write's echoed target and
  check mtimes in the intended folder. Never assume the pointer exists.
- `vf_mcp` is registered with mcp-lazy: the service may report it `disabled`
  until connected; `ensureMcpConnected` (app) or `/mcps` handles that.
- Writing the manifest is atomic (temp+rename + read-back) and preserves
  foreign top-level keys; texture/light changes appear in the running app
  within ~1 s. Don't edit `world.json` concurrently from multiple writers.
- Keep calls bounded (one write per layer per turn where possible) and verify
  with a screenshot before declaring success.
