# AI editing: in-app OpenCode panel + MCP server

> For agents: `.opencode/skills/vf-mcp/SKILL.md` is the operational skill for
> driving the MCP server (tool map, scene data model, authoring recipes,
> texture/anchor rules, verification and gotchas). Load it before world edits.

Two entry points mutate the world:

1. the **in-app AI panel** (`src/app/chat_ui.cpp`) — a thin client of a
   *dedicated* OpenCode session (one per scenery) whose `voxelforge` agent
   (`.opencode/agents/voxelforge.md`) calls the same `vf_mcp` tools as any
   external agent. There is no local tool dispatcher and no direct Ollama
   client in this path;
2. the **`vf_mcp` stdio MCP server** (`src/ai/mcp_server.cpp`) itself, for
   external agent hosts.

Both can mutate the world through two lanes: authored DATA under
`scenery/<name>/` (`ai_edits.vxw`, layer `.vxw`, `world.json`), which the running
app hot-reloads within ~1 s, and live-store intents (`mcp_intents.jsonl`) that
the App executes through its own brush/undo/GPU paths on the next frame. The
in-app panel adds whole-turn Undo (snapshot/restore of authored files), renders
agent screenshots from `workspace/screenshots/`, and persists UI-wide text size
through the `uiFontScale` UI setting.

## In-app panel / OpenCode backend

- `src/ai/opencode_client.{hpp,cpp}` — HTTP Basic auth from the local service
  registration file (`~/.local/state/opencode/service.json`), `VF_OPENCODE_URL`
  override, `POST /api/session` with `location.directory`, `POST .../prompt`,
  `/interrupt`, permission auto-reply (`once`, no approval UI by decision),
  `/api/event` SSE on a worker thread, and `/api/session/{id}/environment` for
  per-turn scene context.
- Session IDs are cached per scenery in `.opencode/voxelforge_sessions.json`.
- The agent is limited by permissions to `voxelforge_*` MCP tools, the
  OpenCodeRAG tools (`search_semantic`, `get_file_skeleton`, `find_usages`,
  `describe_image`, quirk memory) and RAG-backed reads; `set_scenery`/`set_ui`
  are denied this milestone. Shell, file edits and git commands are denied (a
  previous session lost tree-wide work to `git stash`). See the allow rules in
  `.opencode/agents/voxelforge.md`.
- Screenshots: `vf_mcp render_shot` now defaults to
  `workspace/screenshots/shot_<time>.ppm` (gitignored); the panel decodes PPM
  into a cached Vulkan texture and shows it on the tool card.
- Undo: before each prompt the panel snapshots `world.json`, `ai_edits.vxw`
  and every layer `.vxw` into `/tmp/opencode/ai_undo/<pid>/<turn>/`; Undo
  restores them and requests a world reload. `vf_mcp` re-adopts its cached
  world when file mtimes change, so `probe`/`ground` follow the restore.
- Text size: the panel's **Text** slider writes `uiFontScale` into the `ui`
  manifest block; `App::rebuildUiFont()` rebuilds the ImGui atlas/style
  between frames (0.8–2.5); default is now **2.0** for high-res displays.
  At 2×, font pixels double but widget padding scales only 1.5×; the sidebar
  rail/footer/pane width scale fully (524 px rail+footer+pane at 960×540,
  logged once as `sidebar layout: ...`); AI-panel explicit heights scale too.
- Model: the panel has a **Model** dropdown. Default is **Beam Free**
  (`opencode-go/beam-free`); free models are listed by default, **All** shows
  the full catalog. The choice is persisted per scenery in
  `.opencode/voxelforge_sessions.json` and applied with
  `POST /api/session/{id}/model`; `VF_OPENCODE_MODEL=provider/id` pins it for
  scripted runs (overrides the persisted choice). Reasoning deltas are hidden
  from the chat view. Chat replies use OpenCode's low-verbosity style: one
  compact `ok/ERR/... tool(args)` line per tool call (click to expand details),
  with the model's direct text answer shown normally (multi-line messages are
  split and wrapped correctly). The current Ctrl+LMB pick is injected into each
  prompt as a `[UI context: selected voxel cell [...], world position [...]]`
  line, so "here"/"this"/"selected" resolves to the bottom-center anchor. A
  2-minute silence watchdog re-enables Send if a turn never reports idle. The
  agent prompt requires a plain-language answer for every turn — tools are
  optional and chat-only questions get no tool calls. Enter sends a message;
  Shift+Enter adds a newline.
- Optimizations: SSE events are filtered by session ID on the worker thread;
  undone snapshots skip unchanged files; agent screenshots are downsampled to
  360 px before GPU upload (24-card / 32-texture cache bound); tool names come
  from `session.tool.input.started`; MCP connection is ensured at session
  setup (the mcp-lazy plugin starts disconnected).

Known limitation: the MCP tool process inherits the OpenCode service's
environment, so a one-launch `VF_SCENERY`/`--scenery` override on the app does
NOT reach it — the agent inspects the pointer (`scenery/last.json`) scenery.
Normal pointer-based launches are aligned; keep this in mind when testing with
scratch copies outside the repo.

## Legacy direct LLM client (not used by the in-app panel)

The following describes `OllamaClient` and the six-tool bridge that USED to
drive the chat. They are kept for compatibility/tests; the in-app panel now
uses OpenCode + vf_mcp (see above). New work should not extend this path.

Precedence: env var > CLI flag > default.

| source | variables |
|---|---|
| env | `VF_LLM_URL`, `VF_LLM_MODEL` |
| CLI | `--llm-url` / `--ollama-url`, `--llm-model` / `--ollama-model` |
| defaults | `http://127.0.0.1:11434`, model `gemma4:12b`, 120 s timeout |

Endpoint shapes are auto-detected in `OllamaClient::setBaseUrl()`: a URL
ending in `/v1` switches to `/v1/chat/completions` (llama.cpp, vLLM, …);
anything else uses Ollama's `/api/chat`. The choice is **sticky** — once a
request succeeds on one path it stays until the URL shape changes again.
`ping()` health-checks via `GET /api/tags` / `POST /api/show`.

Requests run on a worker thread; tokens (content + optional thinking trace)
stream into the UI, finished results are queued and polled each frame.
Tool calls execute on the main thread during poll.

### System prompt

`src/ai/system_prompt.hpp` pins down: world constants, the anchor rule
("the picked voxel IS the center-bottom voxel"), the palette ids, the six
**exact** tool names, placement rules ("here" → omit anchor), and response
style. If you change canonical tools or palette size, update this file too.

### Tool-call normalization (`src/ai/tools.hpp`)

Small models invent names ("add_rock", "rock_1", "place box") or omit
arguments. Normalization rescues them:

- `toolKey()` — lowercase alnum only, strip verb prefixes
  (`add/create/make/place/put/spawn/build/new`, repeatedly), strip trailing
  digits: `"Add_Rock_2"` → `"rock"`.
- `canonicalToolName(key)` — substring match onto the canonical set:
  cylinder/pillar/column/post → `create_cylinder`;
  ellipsoid/sphere/boulder/rock/stone/pebble → `create_ellipsoid`;
  stamp/mosaic/pixel → `create_stamp`; listworld/layers/list → `list_world`;
  box/crate/cube/block/chest/brick → `create_box`; exact `probe`.
  Structural nouns win over rock-family keywords ("stone_pillar" is a pillar).
- Deletion is split by target: `delete`/`remove`/`erase` + `voxel`/`cell`/
  `region`/`area` → `delete_voxels` (a surgical carve); plain
  `delete`/`remove`/`erase` → `delete_object` (whole layer). Without the
  first rule an invented "remove voxels" silently wiped the entire layer.
- `injectToolDefaults()` — missing args get conservative defaults:
  box `[4,4,4]`, cylinder r0.35 h0.9, ellipsoid `[0.5,0.5,0.5]`
  (`[0.6,0.45,0.6]` flat boulder when the name says rock). Material inferred
  from the name when absent (white/grey/light-rock→5, rock/stone→4,
  wood/log/plank/crate→6, leaf/foliage/hedge→8, sand→3, soil/dirt→2).
  Explicit arguments are never overridden.
- Unknown keys return an error listing valid tools; the chat UI retries with
  that feedback up to 2 times.

Canonical chat tools: `create_box, create_cylinder, create_ellipsoid,
create_stamp, list_world, probe`.

## EditableWorld API (`src/voxel/editable_world.{hpp,cpp}`)

Persistent layer = `scenery/<active>/ai_edits.vxw` + its manifest entry. All anchors are
**bottom-center lattice cells**: the selected voxel's center is where the new
object's bottom-center sits.

```cpp
EditableWorld(std::string assetDir = VOXELFORGE_ASSET_DIR);

bool   load();                 // reads ai_edits.vxw if present (missing OK)
bool   save() const;           // writes record-only .vxw + ensureManifest()
bool   ensureManifest();       // adds ai_edits entry (first object layer,
                               // starts disabled) if absent
void   enableInManifest() const; // flip enabled=true (done on first edit)

std::vector<VoxelRecord> makeBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) const;
std::vector<VoxelRecord> makeBoxMeters(glm::ivec3 anchor, glm::vec3 sizeM, uint8_t mat) const;
std::vector<VoxelRecord> makeEllipsoid(glm::ivec3 anchor, glm::vec3 radiusM, uint8_t mat) const;
std::vector<VoxelRecord> makeCylinderY(glm::ivec3 anchor, float radiusM, float heightM, uint8_t mat) const;
std::vector<VoxelRecord> makeStamp(glm::ivec3 anchor, const std::vector<StampCell>& cells) const;

size_t importLayer(const std::string& vxwPath, glm::ivec3 anchor);
size_t append(const std::vector<VoxelRecord>&);      // dedupe, first wins
size_t appendBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat);
void   clear();                                      // wipe + persist
```

Notes:

- Rasterizers sample the SDF over the shape's AABB and keep cells within the
  ±0.20 m surface band (`kBand`) — shells with correct materials, matching how
  the baker sweeps authored objects.
- `makeBox`: `sizeVox` *includes* the bottom-center cell (y grows upward from
  the anchor row; x/z extend around it).
- `makeEllipsoid` centers at `anchorCenter + (0, radius.y, 0)` so the bottom
  pole touches the anchor voxel center.
- `importLayer` translates a foreign `.vxw` so its bounding box's bottom-center
  lands on `anchor` and appends the copy to `ai_edits.vxw` — the only runtime
  placement path for baked layers (enabling a layer shows it at its baked
  coordinates instead). Dedupes against existing edits; returns count added.
- `clear()` empties the layer but keeps the manifest entry (disabled).

## vf_mcp protocol

Transport: stdio, newline-delimited JSON-RPC 2.0 (MCP stdio transport).
All logging goes to **stderr** — stdout is the transport.

Methods: `initialize` (replies protocolVersion `2024-11-05`, tools capability),
`ping`, `tools/list`, `tools/call {params:{name, arguments}}`; notifications
are consumed silently; unknown methods get error `-32601`. Tool results are
`{content:[{type:"text",text:…}], isError?}`.

The server loads its own `LayeredWorld` for probe/ground queries (includes
ai_edits), and writes edits through its own `EditableWorld` instance.

### Tools

| tool | arguments | behavior |
|---|---|---|
| `list_layers` | – | name/file/role/enabled per layer (packed skipped) + session edit count |
| `enable_layer` | `layer` (name or file), `enabled` | rewrite manifest entry; refuses landscape ("always enabled") and packed roles |
| `probe` | `x,y,z` (meters) | signed distance + material id + solid/empty from the merged field |
| `ground` | `x,z` | terrain height + suggested bottom-center anchor cell |
| `add_box` | `anchor` \| `ground`, `material`, `size` (voxels) or `size_m` (meters) | default size 4³ voxels, clamped to [1,32]³ |
| `add_cylinder` | `anchor`\|`ground`, `radius`, `height`, `material` | defaults r=0.35 m h=0.9 m |
| `add_ellipsoid` | `anchor`\|`ground`, `radius:[rx,ry,rz]`, `material` | default [0.5,0.5,0.5] m |
| `add_stamp` | `anchor`\|`ground`, `cells:[{dx,dy,dz,mat}…]` | ≤1000 literal 0.1 m cells relative to anchor |
| `clear_edits` | – | wipe `ai_edits.vxw` |
| `get_layer` | `layer` (name/file) | detailed layer info (role, enabled, pos, rot, scale, behaviour, voxel count) |
| `set_layer_transform` | `layer`, `pos?`, `rot?`/`rotX?`/`rotZ?` (or yaw/pitch/roll), `scale?` | update a layer's placement in the manifest |
| `set_layer_behaviour` | `layer`, `behaviour` | set autonomous behaviour (e.g. "grazer" for micro objects) |
| `inspect_layer` | `layer`/`path` | bbox + voxel count + material histogram for a .vxw |
| `layer_stats` | `layer` (name/file) | AABB + voxel count for a layer |
| `duplicate_layer` | `source`, `name` | copy a .vxw layer to a new name (registered, disabled) |
| `rename_layer` | `layer`, `new_name` | rename a layer (file + manifest entry) |
| `delete_layer` | `layer` (name/file) | delete a non-protected layer |
| `delete_voxels` | `layer`, `box:[x0,y0,z0,x1,y1,z1]` (or `min`/`max`), or `anchor` + `radius` (m) / `component:true`; `connectivity?`, `mat?`/`from?`, `dry_run?` | surgically remove CELLS from one layer (e.g. a single tree in `isle_forest`); refuses landscape/packed/ai_edits and a whole-layer selection; atomic temp+rename, **not undoable** |
| `list_micro` | – | list micro-voxel object layers with placement |
| `list_materials` | – | palette materials: id, name, rgb, reflectivity, roughness, emissive |
| `list_textures` | – | texture files + current material bindings |
| `list_models` | – | STL/OBJ mesh files in assets/models |
| `world_info` | – | world constants (size, voxel, water level, grid, materials, max lights) |
| `probe_region` | `min:[x,y,z]`, `max:[x,y,z]` (lattice cells) | solid/air counts + material histogram in a box |
| `reload_world` | – | reload the cached world field from world.json |
| `list_lights` | – | authored point lights in world.json |
| `add_light` | `pos`, `color?`, `radius?`, `intensity?`, `follow?` | add a point light |
| `remove_light` | `index` | remove a point light by index |
| `update_light` | `index`, `pos?`, `color?`, `radius?`, `intensity?`, `follow?` | edit one authored light in place |
| `set_texture` | `mat`, `file?`, `scale?`, `emissive?`, `emissiveScale?`, `card?` | bind a texture to a material (empty file = palette; card preserved unless given) |
| `recolor_layer` | `layer`, `mat?`, `from?`, `r?`, `g?`, `b?`, `refl?`, `rough?`, `tex?` | edit existing cells: material/colour/response and per-cell atlas slot (`tex`: 0 clears, 1..255 binds a slot) |
| `get_ui` | – | read the persisted "ui" block + authored sun (and the derived camera aim) |
| `camera_pose` | – | read the LIVE camera from the running app (pos, yaw/pitch degrees, forward); inline or a queued id for `edit_result` |
| `set_ui` | any of ~40 named keys (see below) | patch sidebar-rendered settings; only given keys change |
| `set_sun` | `elev`, `azim` (degrees, toward the sun) | author the scene sun; the app rebakes shadows |
| `list_sceneries` | – | list scenery folders and mark the active one |
| `set_scenery` | `scenery` | switch the live app to another scenery (World panel path) |
| `render_shot` | `out?`, `cam?` = `[px,py,pz,tx,ty,tz]` (position **and look-at target**, metres), `width?`, `height?`, `mode?`, `animtime?`, `sun?` | headless render to a PPM image path; the last three numbers are a target point, NOT yaw/pitch |
| `inspect_rig` | `layer` | derive + report the skeleton (bones, roles, parents, segments) of a micro layer |
| `set_pose` | `layer`, `pose` (flat: bone,rx,ry,rz,...) | set a static rigged pose on a micro layer |
| `view_pick` | `screen:[x,y]` (logical window pixels) or `cell`/`x,y,z` | click into the running VIEW; report cell, world position, material, owning layer, distance |
| `dig_cave` | target as above + `radius?` (0.3–2.5 m, default 1), `depth?` (0.5–10 m, default 3), `style?` (`dripstone\|plain`, default `dripstone`) | one-call natural cave: App composes mouth, tunnel, chamber and optional dripstone through the GUI brushes as one undoable store stroke; returns cells/chunks/ms/undo **inline** |
| `view_click` | target as above + `mode:carve\|add\|delete\|paint\|smooth\|rotate\|move\|select`; brush args `diameter`/`depth`/`strength`/`mat`/`box_scope`, transform args `yaw`/`pitch`/`roll` or `dx`/`dy`/`dz`, `layer?`, `commit?` | use an edit tool directly through the App's real paths; returns an id for `edit_result` |
| `view_undo` | – | undo the last live-edit stroke in the running app |
| `view_clear` | – | clear ALL live edits and the overlay (irreversible) |
| `view_box` | `box:[x0,y0,z0,x1,y1,z1]` or `clear:true` | set/clear the selection box that scopes every cell brush |
| `edit_result` | `id` | engine instrument report for any queued view_* / stamp intent |

#### Clicking into the view

The `view_*` tools are intents: `vf_mcp` validates the arguments and appends a
line to `scenery/<name>/mcp_intents.jsonl`; the running app consumes it between
frames. A `screen:[x,y]` target goes through `App::pickAtWindowPos` — the same
screen→ray conversion, store march and micro-vs-world competition the mouse
uses — so a pixel chosen from `render_shot`/`capture_screen` resolves to the
same cell a Ctrl+LMB click would. Cell/xyz targets stay available for
camera-independent edits. The app then drives `applyEditLive` (all five cell
brushes), the selection box, `undoEdit`, `clearLiveEdits`, and the same
`commitRotation`/`commitMove` the sidebar's Apply buttons call; undo, surfel/SVO
patching, height/water and overlay persistence come for free. `commit:false`
stages a rotate/move preview without writing `world.json`. Results land in
`mcp_results.jsonl` and are read with `edit_result {id}` — if no app is running
the result stays "pending". See `.opencode/wiki/concepts/mcp-intent-bridge.md`.
Gate: `tests/mcp_view_check.py` (CTest `mcp_view_check`, label `mcp`).

#### One-call cave fast path

When the prompt carries `[UI context: selected voxel cell [...]]`, "dig a cave
here" is one `dig_cave` call — no `list_layers`, `ground`, `surface_scan`, or
`render_shot` first. `vf_mcp` validates the target/style/size and posts a
`dig_cave` intent without building its own `LayeredWorld`. The running App
resolves the target (a `screen:[x,y]` click uses `pickAtWindowPos`), then runs
Delete/Carve/Add sub-stamps through `applyEditLive` with one shared drag state.
The result is one undo step, one overlay queue, and an inline JSON instrument
report (`cells`, `chunks`, `runSurfels`, `firstStampMs`, `ms`, `undoSteps`).
If no App answers within two seconds the id stays queued for `edit_result`.

This is the fast **store lane**: it appears on the next frame and persists in
the gitignored `runtime_edits.vxw`, but it is not authored source content.
`commit_overlay` cannot promote deletions because record layers cannot express
air. A permanent cave still needs a baked rock-shell layer with an open mouth.
Every scratch/headless run must use `VF_OVERLAY_PATH=<tmp>` or
`VF_NO_OVERLAY=1`; use `capture_screen`, not a default fresh-process
`render_shot`, to prove the live window changed.

Gate: `tests/mcp_view_check.py` starts a live headless App, calls `dig_cave`
once, requires an inline report in under five seconds, verifies one undo step
and overlay persistence, then exercises the rest of the view/edit tools.
Measured on the shipped hamlet: `help` 3 ms, first mouth stamp 89 ms, complete
13-stamp cave 776 ms, one-call round trip 787 ms.

#### Carving one object out of a baked layer

`delete_voxels` is the per-cell counterpart to `delete_object`. Preview first,
then carve:

```sh
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"delete_voxels","arguments":{"layer":"isle_forest","anchor":[444,559,481],"component":true,"dry_run":true}}}' | ./build/vf_mcp
```

`component:true` flood-fills the 6-connected component containing the picked
cell, so one tree goes while the rest of the forest stays. Use `box` (inclusive
lattice cells) or `anchor` + `radius` (metres) when a canopy overlaps a
neighbour, and `mat`/`from` to restrict by material; `connectivity:26` is
available for diagonally-connected foliage. The `.vxw` is rewritten
atomically via temp+rename, but there is **no Undo** — back up the layer
first. The in-app Delete brush remains the reversible alternative (it writes
to `runtime_edits.vxw`).

### UI settings block (`"ui"` in world.json)

vf_mcp is a separate process: it cannot poke the running app's members. The
control surface rides the channel the app already polls — `world.json` — as
DATA: a top-level `"ui"` object whose keys mirror the Render and Edit panels
(`src/app/world/ui_settings.cpp`). The app re-reads it on the ~0.5 s manifest
poll and applies a field only when the file value differs from the live
member, so a GUI drag is never snapped back by an unrelated manifest edit,
and a manifest with no `"ui"` block behaves as before. At startup the block
lands before `VF_*` env overrides, so explicit scripted A/B flags still win.

Keys (all optional; `set_ui` merges): `renderFlags` (bitmask: 0 AO,
1 Shadows, 2 Flora, 3 Water, 5 SSR, 6 SSAO, 7 DetailNormals, 8 Enclosure,
9 TerrainInterp, 10 AutoExpose, 11 Transmission, 13 PhotoCards), `backend`
(`"splats"|"svo"`), `taa`, `volFog`, `motionBlur`, `dof`, `linearFx`,
`autoExposure` (0/1), `exposure` (0.1–4), `tonemapLook` (0–2),
`ssaoStrength`, `ssaoRadius`, `ssaoBlur`, `ssaoDebug`, `splatRadius` (0.5–2),
`edgeBlend`, `edgeShrink`, `edgeFill`, `edgeBridgeSize`, `cornerFill`,
`lightBudget`, `lightK`, `camSpeed`, `camPos` [x,y,z] / `camX/Y/Z`,
`camYaw`, `camPitch` (deg), `camTarget` [x,y,z] (look-at POINT, metres;
wins over `camYaw`/`camPitch`), `furEnabled`, `furPerCell`, `furLength` (m),
`furWidth` (m), `furCurl`, `furCycles`, `furTilt` (deg), `furSss`,
`brushMode` (`carve|add|delete|paint|smooth|rotate|move`), `brushWidth`
(voxels), `brushDepth` (voxels), `brushMaterial` (palette id),
`smoothStrength`, `falloffCurve`, `smoothPreserveVolume`, `scenery` (folder
name — routes through the same between-frames switcher as the GUI).

Camera placement is the part agents get wrong: raw `camYaw`/`camPitch` are
**degrees in the app's convention** (yaw 0 looks along +X, positive yaw turns
toward +Z, pitch positive looks up), not a compass bearing from −Z and not
radians. Prefer `set_ui {camPos:[px,py,pz], camTarget:[tx,ty,tz]}` — the app
derives yaw/pitch from position → target with the same `atan2/asin` rule as
`--cam`, and a complete target wins over raw angles. `get_ui` echoes the
**authored** pose and its derived forward vector; `camera_pose` reads the
**live** camera from the running app (it may have moved with WASD since the
manifest was written). The app diffs camera fields against its last-seen ui
block, so an unrelated manifest write never snaps a moved view back to the
authored pose.

The sun is its own block (`set_sun`): the live follower applies it only when
the FILE value changed since the app last saw it — the session-only Day/Night
(P) rule holds. Writers use the temp+rename + read-back discipline of
`writeLightManifest` and preserve every foreign top-level key.

### Drawing textures onto cells

Three levels, all data-only and hot-reloaded:

1. `set_texture {mat, file, scale, ...}` binds an image to a whole material.
2. New cells: `add_voxels` / `write_object` accept `tex` on each cell — an
   atlas-slot override (the record's `reserved` byte → `Sample::tex` →
   surfel `tan_aspect.w`); the cell keeps its own material's response.
3. **Existing cells**: `recolor_layer {layer, tex:N}` sets that override on
   matched cells (`from` filters by material); `tex:0` clears it back to the
   material's own slot. `read_object` now prints `tex` on every cell line,
   so read → modify → write round-trips the override without losing it.

Slots 9–15 are the emissive band (only safe for non-glowing art if no enabled
cell uses those ids as its material); the override inherits the cell
material's m/tile scale. Splats are derived from voxels: a `tex` change flows
into their surfels automatically on reload — there is no direct per-splat
texture paint.

Conventions:

- Anchors are bottom-center lattice cells `[0..1023]³`; alternatively pass
  `"ground":[x,z]` to snap onto the terrain surface (uses the live field).
  Out-of-range components clamp.
- `material` clamps to 0–8, default **6 (wood)**.
- Non-native tool names go through the fuzzy normalizer (`create_*` maps to
  `add_*`), so LLM bridges can send invented names safely.

Example session by hand:

```sh
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | ./build/vf_mcp
echo '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"add_box",
      "arguments":{"ground":[3,9],"size":[6,4,6],"material":4}}}' | ./build/vf_mcp
```

Registration (opencode already wires this in `.opencode/opencode.json`):

```json
{ "command": "./build/vf_mcp", "args": [] }
```
