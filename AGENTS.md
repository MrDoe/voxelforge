# AGENTS.md

> **Scope:** The project renders the voxel world primarily with **Gaussian
> surfels** rasterized from `.vxw` records, with the chunked-SVO raymarcher
> kept as the pixel reference (`--mode svo`). The dense reference raymarcher
> and all analytic runtime geometry are **gone** — geometry is derived solely
> from `.vxw` records via `VoxelField` (`docs/history/rework.md`). Keep this
> invariant in every change.
>
> **Full developer documentation lives in `docs/`** (start at
> `docs/index.md`): architecture, world format, GPU contract, AI/MCP tooling,
> testing gates, contributing guide. This file stays the authoritative quick
> convention sheet for coding sessions.

## Build
- Requires Vulkan 1.3, `glslangValidator`, CMake ≥3.24, Ninja, Python3.
- Primary build dir is `build`. All deps via FetchContent (spdlog, glfw, glm,
  VMA, doctest, imgui).
- `ninja -C build` builds binaries + compiles shaders to `build/shaders/*.spv`
  (target `vf_shaders`, `--target-env vulkan1.3`). Shader dir injected via
  `VOXELFORGE_SHADER_DIR`; `VOXELFORGE_ASSET_DIR` points to `assets`.
- `ninja -C build world` bakes assets (heightmap + `.vxw` layers +
  `world.json`) via `tools/heightmap_gen.cpp`. Purely an explicit extra tool:
  never wired into the default build, tests, or `start.sh` — after a clean
  checkout or deleting assets you must run it, or the app/tests exit with
  "run ninja -C build world".
- **Stale objects**: after editing a file, `ninja` occasionally reports "no
  work to do" and the binary keeps the previous contents. If a failing test
  names a line that is not in the file you just edited, the object is stale —
  `touch` the file and rebuild. Do not "fix" code that the file says is right.

## Tooling conventions
Failures here are almost always **harness** mistakes, not code bugs — and each
one costs a whole turn, because the bad call is rejected before any work runs.
- **`edit` takes exactly `path` + `oldString` + `newString`.** All three are
  required; omitting `path` fails schema validation before the edit is even
  attempted, and it fails *identically* on every retry. Put `path` first in
  the argument object. Copy `oldString` verbatim from a read — byte-for-byte.
- **`read`/`grep`/`shell`/`glob`/`edit` are top-level tools only.** Inside
  `execute` (Code Mode) only the catalog tools exist (`tools.search_semantic`,
  `tools.read`, …); reaching for `read`, `edit` or `shell` there fails with a
  no-such-tool error. To page a file inside `execute` use
  `tools.read({filePath, offset, limit})`; at the top level prefer `shell` +
  `sed -n A,Bp` / `grep -n`.
- **Tool arguments are parsed before execution.** An unescaped quote in JS
  passed to `execute` is a *parse* error, so the whole block — every tool call
  in it — silently never runs. Use backtick template strings for content that
  may contain quotes.
- **Same trap one level up:** nested double quotes inside an f-string built for
  a tool argument (`f"... '{k}' ..."`) break the argument before the call is
  made, and retrying the same shape just burns the turn. When a string will
  carry shell/ffmpeg quotes, use `%` formatting (`"... '%s'" % k`) or a
  template literal.
- **`execute` is a sandbox, not a runtime:** no `fs`, no timers, no direct
  I/O, and no `globalThis` / `text()` — build an array and `return` it.
- **Never edit a file a running process is executing.** A background job reads
  its script when it starts, so an edit landing mid-run is picked up
  half-applied and dies on a shape mismatch. `python3 -m py_compile` the file
  first, *then* launch the job.
- **Self-matching `pkill`.** `pkill -f "build/voxelforge"` also matches the
  invoking shell's own command line and kills the entire call. Defeat it with
  a bracket: `pkill -f "build/[v]oxelforge"`.
- **Long jobs: background them, don't poll.** Use `background: true`, say so,
  and stop; the harness notifies on completion. A `sleep`/poll loop in a
  foreground call only burns the timeout — and a sampling wait-loop races other
  work, so wait on the *driver process*, not on a sampled condition.
- **Don't kill processes you didn't start.** If another test driver owns the
  display or the GPU, wait for it or report the conflict; `pkill` on a shared
  binary destroys someone else's in-flight run.
- **Never `rm` with a glob near a file you depend on.** A tidy-up like
  `rm /tmp/x/*.vxw` will eat a backup you forgot was there, and an *unlabelled*
  backup cannot be told apart from a peer session's identically-named file —
  which is exactly how a live-edit session was lost here on 2026-10-01. Spell
  out the path; `ls -la` the matches first and confirm nothing is newer than
  what you are protecting; name backups so ownership is obvious (e.g.
  `overlay-remnant-619KB-05f4a21e.DO-NOT-RESTORE`); and verify a restore with
  `md5sum`, because matching mtime+size did not prove it.
- **Screenshots and metrics beat descriptions.** A vision caption of a render
  is too coarse to judge a 0.1% artifact and will confidently mislabel the
  cause — prefer numeric masks (luminance silhouettes, hole tests). Two false
  conclusions this session came from images: a "hole" that was really the
  world-wide water plane showing through a 0.2 m grid, and a "bug" that was
  another session's render contending for the GPU.
- **Measure, don't theorise.** A failing numeric/geometry assertion: print the
  values (throwaway `g++` binary, `--probe`, `vf_slice`) before proposing a
  cause. Two rounds of float-behaviour guessing on `makeDome` were both wrong;
  one measurement settled it. When compiling a scratch program against
  `editable_world.cpp`, pass `-DGLM_ENABLE_EXPERIMENTAL` (the gtx includes are a
  hard error otherwise) and link `picking.cpp worldfile.cpp chunk_store.cpp
  voxel_field.cpp`.
  Same rule for tooling: a "broken" measurement is usually a broken *probe*
  (a mis-indexed array, a stale window, a baseline captured before the scene
  existed). Confirm the instrument before believing the number.

## Assets — tracked (one exception)
- `assets/` is **versioned** (textures, layer `.vxw` files, `world.json`): the
  authored scene is the source of truth and is not reproducible from code.
  The only ignored file is `assets/runtime_edits.vxw` — the live-edit session
  overlay the app rewrites on every brush stroke (state, not content; the app
  loads it explicitly, it is never a `world.json` layer).
- **Five ways to destroy data here, and every one of them is silent.** The
  overlay is gitignored and the manifests are tracked, so damage is invisible
  in `git status`; check there before blaming a build.
  1. **Never `ninja -C build clean` (or `ninja clean`).** `build.ninja`
     registers `assets/heightmap.png`, `assets/world.json` and
     `assets/landscape.vxw` as OUTPUTS of the heightmap_gen custom command
     (targets `world` / `vf_heightmap`), so the versioned authored scene lives
     in the SOURCE tree while being registered as a build product — a clean
     deletes tracked files. Recover with `git checkout -- assets/…`, **not**
     `ninja -C build world`: a regen rewrites the authored hamlet and is a
     different, unrecoverable change. To force a recompile, `touch` the
     sources (that is also the stale-object fix in Build).
  2. **A headless run is NOT read-only against `assets/runtime_edits.vxw`.**
     With neither `VF_NO_OVERLAY=1` nor `VF_OVERLAY_PATH=<tmp>` set, the app
     loads the overlay and then *writes it back*: `loadStoreOverlay`
     re-derives chunks via `LiveEditor::refreshRegion`, which sets
     `Chunk::edited`, and `OverlayWriter` re-serializes only the flagged
     subset. Measured 2026-10-01: a 4-chunk / 42,969-surfel session became
     1 chunk / 9,641, and 3.26 MB became 619 KB, with no warning. `VF_NO_OVERLAY`
     suppresses only the *load* — a run that also stamps (`VF_TEST_EDIT`,
     `VF_TEST_STROKE_SAVE=1`) still writes. Use `VF_OVERLAY_PATH=/tmp/…` for
     every scratch render. (Caveat when debugging the reload bug itself:
     that path *requires* the shared location, because the bug is precisely
     that the layer path and the overlay path are the same file.)
  3. **`serializeEdited` walks `Chunk::edited` only**, so any process whose
     edited set is a *subset* of the file's chunk list persists a smaller
     file. A plain load-then-save round-trips, but a world reload re-adopts
     the store from freshly baked pools first, so repeated debugging runs
     shrink a session progressively. A small restore count is therefore NOT
     evidence of a broken restore path.
  4. **"Clear live edits" is irreversible** (`src/app/edit/live_edit.cpp`):
     it flushes the writer, then `std::filesystem::remove(overlayPath())`,
     with no backup step anywhere. Same for `VF_TEST_CLEAR=1`.
  5. `assets/world_all.json` is **tracked** and rewritten by
     `tests/test_authoring.cpp` and `tests/test_world.cpp` through a bare
     `std::ofstream` with no temp+rename, so an interrupted `test-world` /
     `test-unit` leaves a truncated manifest — the same reason `OverlayWriter`
     uses temp+rename.
  The durable fix for 2+3 is to stop rewriting the file from a derived subset
  (persist the loaded chunk list, or refuse to save when the set shrinks) and
  to default AUTOMATED runs at an isolated overlay path while keeping
  `assets/` for the interactive app. Do **not** make the default per-process:
  the interactive app is one long-lived process, so a PID-scoped default would
  discard a user's edits on restart — the one consumer who wants persistence.
- `assets/heightmap.png` and `assets/*.vxw` + `assets/world.json` are baked by
  `heightmap_gen`, which now emits **only the terrain shell** (`landscape.vxw`)
  plus the preserved highest-priority `ai_edits.vxw`; object layers are
  runtime-authored with `vf_mcp` (write_object/add_*). The manifest writer
  preserves foreign entries whose `.vxw` still exists on disk, so a regen keeps
  the authored scene (`hamlet_*`) instead of dropping it. `writeManifest` also
  preserves every top-level key it does not own (notably the `textures`
  table), so a vf_mcp layer edit can never silently unbind the atlas. The old baked object
  sweeps (cabin/forest/orchard/dock/scatter) and their analytic `common.hpp`
  shapes are gone from the baker (the analytics remain only as test fixtures).
- **Material textures live in `assets/textures/`** as PNG/JPG and are bound
  per material by the `world.json` "textures" table. The shipped set is
  downloaded from ambientCG (CC0) by `tools/fetch_textures.py`, which
  **mean-matches every photo to the material's palette colour** so the scene
  keeps its identity; `tools/gen_textures.py` (`ninja -C build vf_textures`)
  writes an offline fallback set as `proc_*.png`. Swap anything in the
  in-app **Textures** section (rail `TX`): it rescans the folder, writes
  only the `textures` key back to `world.json` and re-uploads the atlas
  (see "Photo textures" below). **Dropped-in art must be conformant** —
  seamless, albedo-only (no baked sun/AO), no corner watermark, on-palette:
  gate with `tools/check_texture.py`, repair with
  `tools/prepare_texture.py` (strip stamp → flatten → seamless). A raw photo
  or AI generation almost always fails at least one (measured: 1 of 13 AI
  candidates passed raw, 10 after repair). See
  `.opencode/wiki/concepts/texture-conformance.md`.
- **Runtime truth = records.** `LayeredWorld` merges enabled layers
  (first-wins-a-cell), builds `VoxelField`, and synthesizes the SVO.
  `reloadIfChanged()` polls mtimes every ~0.5 s; GUI toggles reload instantly.
- **Default world = lakeside hamlet** (runtime-authored): world.json ships
  `landscape` plus the `hamlet_*` object layers (hall, tower, pier, boat, well,
  market, garden, pines, props, reeds, reeds_far) built with
  `vf_mcp write_object`; layers are
  opt-out via the sidebar's **World** section (rail `WL`; a plain .vxw file
  list). Toggling
  triggers an incremental rebuild and refreshes SVO + terrain texture + shadow
  volume together (`applyWorldReload`). Content tests generate their own
  all-enabled manifest (`world_all.json`) from `assets/*.vxw`, so new layers
  join the test world automatically.
  **Keep-out rule**: `tests/live_edit_check.py` carves at world (0.05, 2.05)
  and (0.05, 3.05) and asserts the newly-exposed water matches open water
  within 12/255 per channel — do not place scenery in world x −3…3, z −1…6
  (the reed layers exclude it; reeds there shifted the carved-water mean 22
  codes in blue and failed the guard).
- **Layer files hold absolute lattice coords; enabling shows the object where
  it was baked.** To place a copy at the picked anchor, use the per-layer
  "Import" button (or `EditableWorld::importLayer(path, anchor)`): it
  translates the layer's records so their bottom-center lands on the selection
  and appends only that object to `ai_edits.vxw`. The `pos`/`rot`/`rotX`/`rotZ`
  manifest fields are **applied at runtime**: `transformRecords()` composites
  `Ry(rot)·Rx(rotX)·Rz(rotZ)` about the layer's bottom-center; enabling shows
  the object at its placed orientation (zero = bake-time identity).
- **STL/OBJ authoring is available in the GUI** via the sidebar's **Mesh**
  section (rail `IM`). It scans `assets/models/`, accepts a typed path, and
  exposes fit/scale, source-axis/winding, material, solid/shell, and lattice
  anchor options. Replacing an existing layer writes only its `.vxw`; the
  manifest pose/orientation is preserved. `VF_TEST_MESH_IMPORT` exercises the
  same `App::importMeshFromGui()` path headlessly. The offline equivalent is
  `vf_mesh2vox`; the current hamlet cabin was reimported from
  `assets/models/Forrest_Hunting_Cabin.stl` with its existing
  `pos=[0,1.65,0]`, `rot=-113`, `rotX=-89.8`, `rotZ=-63` unchanged.
- Don't hand-edit derived `.vxw`; edit terrain in
  `tools/heightmap_gen.cpp:terrainHeightAt()`, then `ninja -C build world`.
  Object content goes through the `voxel-object` skill / `vf_mcp`
  (`write_object`, `add_*`) — data-only, no recompile.
- `assets/ai_edits.vxw` is the live layer for chat/MCP edits; `vf_mcp`
  appends + saves immediately.

## Run
- `./build/voxelforge` — reference cam `1.0,2.0,1.5 → 5.3,1.0,11.3` (house.jpeg view), sun `34°/238°`.
- Keys: `WASD/QE` move, `RMB+mouse` look, wheel speed, `Ctrl+LMB` pick anchor,
  `F` toggles splats/SVO renderer, `N` toggles TAA, `[`/`]` shrink/grow splat disks. The window close button quits; `Esc` is reserved and does not exit.
  `B` toggles texture detail normals (render-flag bit 7, default on), `G`/`H` toggle SSR/SSAO,
  `J` volumetric fog (opt-in, see below). Enclosed-space sky occlusion
  (render-flag bit 8) has no key — it is on by default and `VF_RENDER_FLAGS=255`
  turns it off; see "Enclosed-space shading + light sources" below.
  **Edit brush hotkeys** (state-dependent, and the reason the key list is not
  one flat table): `Tab` is the ONLY View/Edit switch (the plain arm/disarm
  toggle, both directions); `C` = Carve, `A` = Add, `D` = Delete, `S` =
  Smooth, `M` = Move, and those pick a mode only while the brush is armed — in
  View mode they never switch the mode (A/D/S still fly, `C`/`M` do nothing),
  so a stray press while looking around cannot drag you into Edit. The mode
  keys' edge latches are still read every frame while disarmed, so arming can
  never make a stale latch phantom-fire.
  `+`/`-` change the width by one voxel and `Shift`+`+`/`-` the depth (Smooth
  strength) — those already existed and act only while armed; disarmed, `+`/`-`
  are exposure. **Bind these by keycap, not by position**: GLFW reports physical
  X positions, so on a German layout the `+` key is `GLFW_KEY_RIGHT_BRACKET`
  (which also grows splat disks) and `-` is `GLFW_KEY_SLASH` (unbound) — the old
  position-based test silently did nothing for the keys a German user actually
  presses. `keyShowsChar()` in run_hotkeys.cpp asks the layout via
  `glfwGetKeyName`, the numpad stays positional, and the splat-radius handler
  yields its `]`-position grow to the brush while editing when that key shows
  `+`. Do NOT use `io.InputQueueCharacters` for this: ImGui fills it from its
  event queue during `NewFrame()`, which runs in the *render* step — after
  `handleHotkeys` — so it is always empty there.
  **`W/A/S/D/Q/E` always move the camera, in every edit-tool state.** While
  armed, A/D/S are therefore dual-purpose: the mode is picked on the *edge*
  (one action per press) while flying is level-triggered (continuous while
  held), so holding A strafes and selects Add once. Do not "fix" this by
  yielding the keys to the brush while armed — that was tried
  (`Camera::moveKeysYieldsToBrush`) and reverted, because flying is how you
  aim the brush in the first place.
  Mode keys go through `App::chooseEditMode`, the single implementation shared
  with the sidebar's mode buttons — a bare `m_editBrush` write from the hotkeys
  would silently discard a staged rotate/move that the panel would have refused.
  Rotate has no hotkey: it needs a selected layer first, and a key that
  silently no-ops is worse than no key. `App::drawHotkeyBar` renders the current
  set at the bottom of the screen on a dark plate.
  **Rotate** is selected from the toolbox or the World Layers **Activate
  trackball** action. A plain LMB click on an object then activates that exact
  owner; ownership comes from the picked `VoxelField` cell and stable 8-bit
  layer ID, never an AABB. The visible trackball is centered on the placed AABB
  centre and sized from its projected bounds. Press-drag its outer/local-Y,
  wide/local-X, or tall/local-Z ring; a click on the object itself never
  starts a rotation. Release stages the pose; use the Edit section's
  **Apply rotation** button to persist it (or **Cancel** to discard it), and
  note the sidebar **footer** announces a staged transform from every section.
  The object remains activated after staging and after Apply. Ring
  hits are tested before ImGui capture and the whole sidebar is
  input-blocked (`ImGuiWindowFlags_NoInputs`) while the pointer is on a ring —
  and faded to 0.30 alpha during a drag — so the gizmo stays usable over the
  panel.
  The ring drag is **live** and composes on the object's own local X/Y/Z axes;
  forward splats, GPU cull, and tile bin/render apply the same selected-owner
  transform about the exact canonical
  bottom-center pivot + manifest position (bind 14 `RotUBO`; tile binding 24).
  Move mode uses the same selected-owner translation lane for a staged axis
  drag; Apply move persists `pos` only after release.
  The preview maps the current absolute pose with `Rnew * transpose(Rold)`, so
  it matches the committed `Ry(rot)·Rx(rotX)·Rz(rotZ)` pose. Camera movement
  is applied before picking, gizmo projection, and render-push construction so
  the ring cannot lag a moving view. No surfel
  rebuild, world reload, terrain, water, other layers, or source-chunk culling
  occurs during the drag. On release the accumulated yaw/pitch/roll is staged
  but not written; Apply writes it to `world.json`, and the final preview stays
  active until the rebuild lands. **Move** mode grabs the exact picked owner,
  constrains dragging to the selected world X/Y/Z axis (the scene shows all
  three colored handles), and likewise stages `pos` until its own Apply button.
  `VF_TEST_ROTATE_LIVE="yaw,pitch,roll"` is preview-only and must never
  commit; `VF_TEST_ROTATE` exercises the commit/rebuild path. Both use
  `VF_ROTATE_LAYER=<file>`. `VF_TEST_MOVE_LIVE="dx,dy,dz[,layer]"` is the
  preview-only translation diagnostic and never writes the manifest.
  `VF_TEST_TRACKBALL_PROBE=1` is a headless diagnostic used by
  `visual_check.py` to verify the independently projected bounds centre/radius
  and yaw/pitch/roll ring hit classes without changing the manifest.
  **Placement** — the searchable World section of the sidebar exposes per-layer
  `x/y/z`, yaw, pitch, and roll fields. `transformRecords()` rotates about the
  layer's authored bottom-center and then applies `pos`; the placement hash
  folds all three angles into incremental dirty tracking
  (`parseAndComputeDirty`). Angles are degrees and may use any range.
- **GUI = one docked left sidebar, no floating windows.** `drawSidebar()`
  creates a single `Voxelforge##Sidebar` window (`NoMove|NoResize|NoCollapse|
  NoSavedSettings`; native resize is disabled because position/size are forced
  with `ImGuiCond_Always`, so a stale `imgui.ini` cannot displace it) holding
  three parts: a 46 px **icon rail** (`ED WL RN TX IM AI` — 2-letter labels
  because the default ImGui font has no icon glyphs), a content pane showing
  one `Panel` section at a time, and a fixed 52 px **status footer**. The idle
  window is fully opaque; square, borderless child surfaces match the dock
  background, so no scene bleed or top/bottom inset bands remain. A custom
  8 px right-edge grip resizes it horizontally; double-click restores the
  responsive default, and the frame loop gives the grip priority over
  scene/ring input. It is an
  *overlay*: the render stays full-window and picking still runs camera→cursor,
  so the sidebar only costs screen area. Sections:
  `drawPanelEdit/World/Render/Textures/Mesh/AI`.
  `Ctrl+B` collapses it to the rail alone (`Tab` used to, and now toggles the
  edit mode instead); `Ctrl+1..6` jump to a section (bare
  `1..5` remain the render-flag toggles, and the `edge()` latch gives the
  Ctrl path the edge on exactly the frames the bare loop must ignore).
  `ChatUi::drawPanel` renders the chat body into the AI pane — the chat has no
  window, position, or visibility state of its own. `m_editActive` is
  tool-armed state, *not* a window flag: it gates LMB stamping in the frame
  loop, `Tab` also switches the rail to Edit, and an accent bar marks the armed
  rail button. A staged rotation/move is announced in the footer from every
  section (it is a pending `world.json` write). Keep widget widths relative
  (`-1.0f`) or ≤ pane width; the default pane is ≈300 px wide (the full
  sidebar is 334 px at 960×540) and the user can drag it wider or narrower.
  See `.opencode/wiki/entities/hud-sidebar.md`.
- Headless: `--selftest`, `--smoke N`, `--shot out.ppm --cam …`,
  `--probe X Y Z`, `--sun <elev> <azim>`, `--animtime <s>`, `--width/--height`,
  `--mode splat|svo` (default `splat`; SVO is the pixel reference).
- Splat perf knobs (all env, default tuned): `VF_LOD=0` disables the baked
  merged-terrain LOD rings (surfelize 2x2x2 / 4x4x4, draw-time selection
  `VF_LOD1`/`VF_LOD2` = 30/90 m, material-split blocks keep shore/rock
  boundaries readable; `VF_LOD_SPLIT=0` restores the single-majority disk;
  objects ride along unmerged — trees must
  never vanish), `VF_NO_GPU_CULL=1` disables the GPU per-surfel
  cull pre-pass (compute compaction + GPU-written indirect counts;
  bit-exact), `VF_NO_OCCL=1` disables the Hi-Z occlusion pyramid + GPU
  occlusion cull (~3 ms overhead at 720p; saves fragments when objects
  occlude the background). The depth prepass itself always runs: it is the
  opaque/water depth-resolve reference the base and band passes test
  against. `VF_SPLAT_DIRECT`/`VF_NO_INDIRECT_BARRIER` legacy A/B paths.
- Splat kernel knobs: `VF_ANISO=0` disables the anisotropic footprint bake
  (disks stretch up to 1.6x along the local crease; store path shares the rule),
  `VF_EDGE_SHRINK=0..1` tightens only opaque object parents with two
  non-opposite exposed lattice faces; normal disagreement never shrinks object
  coverage. `VF_EDGE_FILL=0` disables the derived small tangent-aligned crease
  bridges (GUI: **Rendering → Interpolate crease splats**, default on). The
  chunk layout is `[base | edge bridges]`: bridges stay in
  the always-on opaque range, add no draw call, and carry no extra shadow/AO
  march (GUI: **Sharp-edge fit**, default 0.35; changes trigger a world/surfel
  rebuild). `VF_LOD_SPLIT=0` restores the
  single-majority LOD disk,
  `VF_SPLAT_SIGMA` (Gaussian variance in normalized
  disk units, default 0.5), `VF_SPLAT_OPACITY` (centre alpha, default 0.9),
  `VF_SPLAT_DEPTH_TOL` (depth-resolve band in NDC depth units, default
  0.002 ≈ 10 cm; applied per frame as a dynamic rasterizer depth bias),
  `VF_SPLAT_EXTENT` (quad half-size), `VF_SPLAT_RADIUS`.
- Live edit: the sidebar's **Edit** section (`Tab` arms the tool) has seven brush
  modes — **Carve** (depth-limited cylinder scoop along the surface normal),
  **Add** (grow the surface out along the surface normal: the brush footprint
  disk is extruded `depth` and closed by a fillet of `min(radius, depth)/2`, so
  clicking a wall thickens it over its whole footprint and only rounds the
  outer edge — `makeDome`, and its loop bounds must project the reach on
  `axisDir`, never on world Y: the old world-Y bound clipped the footprint to
  `y >= base.y`, so a wall stamp covered nothing below the pick (0 of 4612
  cells measured) and read as a bulge), **Delete** (clear every cell in the
  brush ball), **Paint**
  (recolour the brush ball with the panel's Material combo), **Smooth**
  (`ChunkStore::makeSmoothEdits`: a terrain pick relaxes column tops toward a
  weighted local average with a circular falloff and protects object-owned
  columns; an object pick generalises the same surface-position relaxation to
  the picked cell's one-sided face axis — thin wall/rod cells use the SDF
  gradient, a diagonal rod is left alone — and moves the surface as one
  contiguous Clear/Set span, so a grounded bump collapses whole, a notch
  fills, and walls/posts/staircases stay fixed points; fills are always
  object-owned, terrain is never converted), **Rotate**
  (click an object to activate its bounds-centered trackball, then click-drag
  a yaw/pitch/roll ring; release stages and **Apply** commits), and **Move**
  (grab the exact owner and drag along the selected X/Y/Z axis; release stages
  and **Apply** commits) — plus a per-mode/size/perf readout and a "Clear live
  edits" button (drops

  `runtime_edits.vxw`).
- **The brush is sized in voxels, and 1 voxel is the per-voxel sculpt mode.**
  The sidebar's **Width** is a `DragInt` in voxels (1..120, i.e. 0.1..12 m;
  `App::brushVoxels/setBrushVoxels/quantiseBrush` keep `m_editDiameter` on the
  lattice, and every `VF_EDIT_DIAM`/`VF_EDIT_DEPTH` override runs
  `quantiseBrush()`, so `VF_EDIT_DIAM=0.1` is exactly 1 voxel). `+`/`-` step the
  width by one voxel, `Shift`+`+`/`-` the depth. At **1 voxel** with
  **Add**/**Carve** the stamp bypasses the volume rasterizers entirely and
  emits exactly one cell (`EditableWorld::makeSingleVoxel`): Carve removes the
  voxel under the cursor, Add places the one just outside the surface along
  the normal's dominant axis (the pick always lands on solid material, so
  adding the picked cell would be a no-op) — a single-axis step, never
  `round(n / VOXEL)`, which a smoothed corner normal would send 7 cells off
  target. The Depth slider is disabled at 1 voxel. The old 0.1 m radius clamp
  silently made even a "1 voxel" brush 3 cells across, and a 1-voxel volume
  would still take the neighbour along the normal (measured: 2 cells for Add,
  4 for Carve — the guard is `check_per_voxel` in `tests/live_edit_check.py`,
  which asserts the stamp log says exactly `1 cells`). **Delete/Paint need no
  special case**: `makeSphere(r=0.05)` already emits exactly 1 cell (and 4 at
  r=0.1), so 1 voxel reads the same in every mode.
- **The hover highlight is the per-voxel target.** The post pass draws a
  voxel-sized outline (`±0.62` voxel, edge band 0.10) from `uSel`/`uHover`
  under render flag bit 4 (default on, key `4`). The hover branch used to be
  centred on `uSel.xyz` instead of `uHover.xyz`, so with nothing selected the
  outline landed on the world origin (invisible) and with a selection it
  outlined the wrong cell. Keep the two feeds separate.
- **Every stamp patches the live store** (there is no
  bake/record path any more; the legacy `carve_edits.vxw`/`raise_edits.vxw`
  layers are read-only leftovers). The water is **one fixed-level plane**
  (`WATER_LEVEL = -0.9`) subdivided into a world-wide 0.2 m grid of coplanar
  surfels (`buildWaterSurfels`): the splat shader intersects the analytic plane
  per fragment and shades it identically, the grid is pure coverage, and the
  depth test against the opaque prepass hides the cells over dry land/objects.
  So a dig below the level is water with no per-column bookkeeping (no flood
  machinery); the SVO plane is analytic the same way. Water **caustics**
  (`causticAt()` in `common_base.glsl`, both backends) are composited at the
  surface, not the bed: the absorption alpha (`clamp(0.35 + depth*0.9, 0,
  0.97)`) hides the bed past ~0.7 m, so bed-only caustics were invisible even
  at 10x strength. What must stay in sync is
  the *height texture* the water shading reads for the bed (shore foam,
  absorption, the reflected-bed march): `App::patchHeightTexture` re-derives
  the edited columns from the runtime store and re-uploads that sub-rect after
  every stamp / overlay restore — stale, the channel water shaded as thin
  foam-washed land instead of water. The Carve scoop reaches
  `EditableWorld::kCarveTopMargin` (0.2 m) above the picked cell and keeps
  boundary cells (`d > VOXEL*1e-3`): a flat cap left a one-cell roof over the
  ±1 cell of relief around the pick — and `glm::rotation(up, -up)` is
  degenerate, so an exact `d > 0` test dropped half the base-plane layer (a
  channel dug that way stayed roofed). In the splat backend, hovering
  with the tool active **tints the affected splats** (the exact brush volume:
  the carve cylinder or the delete/paint ball) so the LMB result is visible
  first — bind 13 `BrushUBO` (`SplatPass::setBrush`), tested against the surfel
  *centre* (per-splat, matching the CPU rasterizer's cell set) plus a 0.06 m
  skin; water-plane splats are skipped at the tint site (the plane is one fixed
  level a carve exposes instead of removing); the **Add** preview tints the
  growth's footprint green (the volume is the exact extruded disk + fillet
  `makeDome` emits — brush radius in `bVolume.w`, growth depth as a negative
  `bAxis.w`, the skin folded into both — so the tinted splats are the surface
  the growth buries; the new shell has no splats yet); debug view
  `VF_SPLAT_DEBUG=15` shows the volume directly.
  `Undo` (panel button / Ctrl+Z) reverts the **last stroke**: every stamp
  records each touched cell's pre-edit state (first occurrence per cell wins) in
  a per-stroke map, and stroke end (mouse release / `finishStroke`) pushes it as
  one step (bounded 250k cells, 32 steps; an oversized stroke is dropped rather
  than partially undoed). Undo replays those cells as Clear/Set edits through
  the same store→GPU path a stamp uses (`App::commitStoreEdits`) and re-queues
  the overlay. `Clear live edits` drops every runtime edit + the overlay file
  in place: it flushes the overlay writer (a pending save could resurrect the
  file), re-adopts the store from the baked pools
  (`LayeredWorld::invalidateStore`) and re-seeds the touched chunks' surfels,
  SVO pools and height-texture columns, then requests a full world reload so
  the baked LOD/normals come back (the in-place revert lands in the same frame,
  the reload follows). Overlay path is overridable for tests
  (`VF_OVERLAY_PATH`). LMB **paints**: holding
  the button keeps stamping along
  the cursor (spacing = ¼ brush diameter) and the result appears in the same
  frame. Each stamp applies cells to the runtime `ChunkStore`, rebuilds only
  the edited block region, refreshes per-chunk surfel caches (`LiveEditor`)
  and patches the GPU buffers (`SplatPass::patchChunkSurfels`,
  `SvoPass::patchChunk`) — no `.vxw` write, no world reload, and hover/anchors
  use `rayPickStore`. Steady-state stamp latency is ~5–25 ms (brush-sized,
  see `VF_TEST_STROKE`): a chunk's surfel cache is seeded from the GPU's
  current run (`SplatPass::readChunkSurfels`) instead of re-baking the whole
  chunk — the freshly seeded run is the *pre-edit* GPU state, so `stamp()`
  must refresh the edited AABB ±3 on top of it or the first stamp in a chunk
  patches the old surface back and looks like a no-op. On mouse release the
  stroke is **saved asynchronously** to `assets/runtime_edits.vxw` (VXW v2
  store section, schema 3 = per-chunk edit AABB + texture tags) and restored at the next
  startup / world reload (GPU-seed + refresh the saved AABB, so the restored
  frame matches the session). Patched chunks keep their full run —
  `LiveEditor::chunkRun` regenerates it from the cached base run —
  while their LOD ring drops until the next full
  reload; `uHeight` is patched for the edited columns and the water plane is
  world-wide, so both follow a live edit. Headless hooks: `VF_TEST_EDIT="x,y,z,carve|add|delete|paint|smooth"` (one
  stamp), `VF_TEST_BRUSH="x,y,z,carve|delete|paint|smooth"` (activates the tool and
  renders only the hover preview, no edit), `VF_TEST_STROKE="x,y,z,steps[,mode]"`
  + `VF_TEST_STROKE_SAVE=1` (drag simulation + persistence),
  `VF_EDIT_DIAM`/`VF_EDIT_DEPTH` (brush size),
  `VF_SMOOTH_STRENGTH`/`VF_EDIT_STRENGTH` (Smooth relaxation, 0..1),
  `VF_LIVE_NOSPLAT=1` /
  `VF_LIVE_NOSVO=1` (skip one backend), `VF_SPLAT_NOWATER=1` (A/B: skip the
  water-plane draw so a test can prove a dug volume reads as water),
  `VF_TEST_UNDO=1` (close the pending stroke and undo it), `VF_TEST_CLEAR=1`
  (drop every runtime edit + the overlay), `VF_OVERLAY_PATH=<file>` (overlay
  read/write target; tests keep their edits out of `assets/`),
  `VF_NO_OVERLAY=1` (ignore
  `runtime_edits.vxw`; the test scripts set it so a session's painting cannot
  pollute the reference shots).
- Tile splat path (WIP, `VF_TILE=1`): compute-only pipeline
  `splat_tile_{bin,scan,base,render}.comp` — project+bin surfels into 16×16
  tiles via a (tile × entry) counts matrix, scan per-tile ranges
  (offsets/ends), convert counts to exclusive entry bases, fill the dup
  stream atomically (entry-private cursors, lane-0 ordered walk = exact
  forward submission order), and blend per pixel in registers with the
  same pure-Gaussian alpha + depth-resolve as the forward opaque pipe
  (a first loop mirrors the depth prepass, a second blends only the front
  surface band). Replaces the two forward raster passes. Status: renders
  the full scene; water bit-exact, opaque close in isolation, but the full
  composite still shows small per-component fp deltas through the blend
  chain. Perf NOT yet a win: 143 ms vs 57 ms forward geo at 720p hero
  (bin/fill 65 ms incl. two projection passes + contended atomics, render
  78 ms from full-tile per-pixel loops) — forward remains the default until
  parity is proven per scene (hero/corner/overview/house/water). The tile
  path quantizes depth to 1e-5 units internally; the forward resolve now
  uses the fixed-function gl_Position depth (no gl_FragDepth writes).
- GPU timestamp profiler: HUD "GPU ms" line + `VF_TRACE` log (`geo/post/fx/
  taa/tail`), 6 marks × 3 frame slots, readback after each slot's fence wait.
- `VF_TEST_FORCE_PRESENT_ERR="CODE[,CODE…]"` (a `VkResult` name or an int)
  drives the acquire/present result probe with **synthetic** results so its
  reporting has a positive firing test — device-lost/out-of-host-memory log at
  error, everything else at warning, **once per distinct code** (repeat a code
  in the list to see the latch work). It reports through the logger only and
  never touches the real present. It is called from the **headless** frame body
  as well as after the real present, because a headless render never acquires
  or presents the swapchain (it submits, then reads back offscreen) — a hook
  placed only beside `vkQueuePresentKHR` is unreachable from every test.
  Gated: `visual_check.py:check_present_probe`.
- Chat backend: Ollama defaults or any OpenAI-compatible server via
  `VF_LLM_URL=http://host:8080/v1 VF_LLM_MODEL=… ./build/voxelforge`.
  MCP: `./build/vf_mcp` (stdio), registered in `.opencode/opencode.json`.
- Present quirk: default IMMEDIATE on NVIDIA+X11 (`VF_PRESENT=immediate|mailbox`),
  per-swapchain-image acquire semaphores (`App::m_acquireSems`, created in
  `App::ensureAcquireSemaphores()`, consumed in
  `App::recordInteractiveFrame()` — all in `src/app/rhi/surface.cpp` and
  `src/app/frame/record_interactive.cpp`).

## Tests & verification — group-only
- Build first with `ninja -C build`, then run only the focused group(s) that
  cover the changed files: `ninja -C build test-unit`, `test-app`, `test-surfel`,
  `test-store`, `test-world`, `test-live-edit`, `test-visual`, `test-effects`,
  `test-textures`, `test-fog`, `test-preview`, or `test-smoke` (`test-fast` is
  an alias).
  **Never run bare `ctest --test-dir build` or an all-tests target.** All CTest
  entries are guarded by `tests/group_gate.py`; without an explicitly enabled
  group they return CTest's skip code.
- **`test-preview` is the narrow one.** Any change to the brush hover preview —
  `frame/run_brush_preview.cpp`, the BrushUBO feeds, `inBrushVolume` in
  `shaders/common_surfel.glsl`, the tint in `splat.frag`/`svo_raymarch.comp`, or
  the depth marker in `shaders/post.comp` — runs `test-preview`, not the whole
  `test-live-edit` matrix: `live_edit_check.py --only preview` renders its own
  baseline and checks the tint hue per mode, that the **Depth** slider moves the
  frame above the ~1.25% TAA noise floor, and that `--mode svo` shows the tint
  too. Add `test-live-edit` only when the change also touches stamping, undo,
  the store or the overlay. See `.opencode/wiki/concepts/brush-preview-visibility.md`.
- `test-app` covers the app's *pure leaf* logic — the gizmo screen maths
  (`ui/gizmo_math.*`), the command line (`cli/args.*`) and the sidebar
  vocabulary (`ui/ui_primitives.*`, `ui/ui_types.hpp`). Those were unreachable
  from a test while they shared a translation unit with `main()`; they are also
  inside `test-unit`, so the existing entry point still covers them.
- Use the smallest relevant group: surfel/radius changes use `test-surfel`;
  ChunkStore/brush/live-patch changes use `test-store` or `test-live-edit`;
  camera/shader/scene changes use `test-visual`; SSAO, texture, and fog
  changes use their dedicated groups. Add a second group only when the change
  crosses those boundaries. `test-surfel`/`test-visual` include the GPU
  `--selftest` check.
- `./build/vf_tests --test-case="*world*"` is available for one doctest
  filter while iterating; it is not a substitute for selecting a group.
- `./build/vf_tests --test-case="chunk*"` — ChunkStore foundation suite
  (`tests/test_store.cpp`): adoption vs `VoxelField`, cell edits + rebuild,
  store-based per-chunk surfels following edits.
- `python3 tests/visual_check.py build/voxelforge` — hero/house/water shots;
  coverage 3–98.5 %, black-in-silhouette <5 %, blue sky probe. It also renders
  `VF_SPLAT_DEBUG=16` ownership masks before/after a synthetic cabin preview:
  selected owner green, other objects red, terrain/water blue; the selected
  mask must move while the other-object mask stays fixed. This pins
  selected-`.vxw` isolation independently of layer AABBs.
- `python3 tests/live_edit_check.py build/voxelforge` — renders the hero view
  untouched and with `VF_TEST_EDIT` (live store patch) in **both backends**
  (splat + `--mode svo`) for Add, plus the splat-only Delete/Paint store modes
  (the diff is geometry only; a separate
  check pins that a patched chunk keeps its full run), the
  carve/add hover tints (warm vs green), the **Depth** slider and the SVO
  preview (both new: `check_depth_sensitivity` demands the frame change by more
  than the ~1.25% TAA noise floor, and `check_svo_preview` demands `--mode svo`
  show the tint red-dominantly — it had no preview at all), and the water plane (a dug pit must
  read as water-plane pixels — A/B against `VF_SPLAT_NOWATER=1` — and a carved
  channel that reaches the river must match the open water's colour within
  12/255 per channel; no subtractive preview tints the water plane); undo and
  `VF_TEST_CLEAR` must remove live geometry again (regression: an emptied
  chunk's patch was skipped, so the removed material kept rendering — the frame
  centre's luma must return to the untouched value) and the cleared overlay
  file must be gone;
  asserts a visible but bounded pixel diff and sane edited-frame probes.
- `python3 tests/fog_check.py build/voxelforge` — the three canonical shots ×
  both backends with `VF_VOLFOG` off/on, asserting the mean shift stays within
  −1..+12 %, **no new pixels below luma 20** (the old fixed-16 m march darkened
  ~42 % of the hero frame), distant dark geometry **lifts** (aerial
  perspective), and toward-sun in-scatter is ≥1.5× away (measured 4.2×).
- `python3 tests/texture_check.py build/voxelforge` — the atlas A/B (textured
  vs palette vs `VF_TEXTURES=0` bit-exactness) and the per-cell override.
- `./build/voxelforge --selftest --width 640 --height 360` — sky probe +
  coverage acceptance.
- `--probe X Y Z` reflects the live layered field (loads `world.json`).

## Architecture
- `src/voxel/voxel_field.{hpp,cpp}` — **the load-time geometry oracle**, built
  from merged records: terrain columns (top Y + material), object components
  flood-filled to solids with two-pass Dijkstra signed distance grids stored in
  a sparse hash; emits the GPU height texture (rg32f topY+mat), an object
  presence block mask, and the coarse r8_snorm object volume for shadows.
  `Sample::layer` carries the winning `.vxw` owner through object EDT/material
  propagation; terrain and live-added cells without a base owner use ID 0.
- `src/voxel/chunk_store.{hpp,cpp}` — **the runtime-explicit sparse voxel
  store** (M0–M3 of live editing): per-chunk cells with packed appearance +
  response, `Empty | Solid | Explicit` chunk states, sparse 8³ bricks plus
  solid boxes, cell-level `apply()` edits, chamfer SDF-band recompute and
  octree pool regeneration (`rebuildDirty`). **Rebuilds are region-limited**:
  the region is the edit AABB ± `kLiveBand` (12 cells), snapped to 8³ blocks,
  so a stamp re-materialises/re-derives only the affected blocks and copies
  untouched blocks verbatim (unit test pins localized == full rebuild).
  Adopted lazily from the resident pools by `LayeredWorld::store()`. Touched
  chunks are flagged `edited` and serialized by `OverlayWriter` into the VXW
  v2 store section (`assets/runtime_edits.vxw`, temp+rename, debounced, not
  listed in world.json — the app loads it explicitly at startup/reload).
- `src/voxel/live_editor.{hpp,cpp}` — instant-feedback brush driver: applies
  edits, refreshes per-chunk surfel caches (`buildChunkSurfelsRange`, only
  the ±3-cell region is re-enumerated/re-shaded) and returns the updated runs
  for GPU patching. First touch of a chunk seeds its full run once.
- `src/voxel/chunk_index.hpp` — canonical z-major chunk indexing
  (`chunkIndexOf/…`), shared by the SVO, surfel and store paths (the surfel
  path used to be x-major; do not reintroduce a second convention).
- `src/voxel/layered_world.{hpp,cpp}` — manifest load/poll/priority merge,
  per-chunk SVO synthesis with resident `ChunkPool`s for incremental rebuilds,
  water-volume marking below `WATER_LEVEL=-0.9`. It also allocates stable
  non-zero 8-bit owner IDs for enabled object/scatter files (filename FNV
  preference, collision probe, retained across toggles/reloads), retains the
  exact file -> ID and placed-pivot maps, and tags each merged object cell with
  its winning layer.
- `src/voxel/editable_world.{hpp,cpp}` — `ai_edits.vxw` writer
  (box/cylinder/ellipsoid/stamp rasterizers, bottom-center anchor).
- `src/voxel/common.hpp` — constants (`WORLD=102.4`, `VOXEL=0.1`, `GRID_N=16`,
  `BRICK_N=8`, palette/material tables, spot constants) **and baker-side
  analytic shapes** (`houseAt/treesAt/…`, used by `heightmap_gen` sweeps and
  tests as authoring truth — NOT linked into the renderer path).
- `src/render/svo_pass.{hpp,cpp}` — SVO reference compute pipeline;
  `RaymarchPush` (128 B) lives here. The world SSBOs use **chunk-local
  handles**: `GpuWorld::chunkInfo` (uvec4 per chunk = nodeBase/childBase/
  brickBase, binding 11) is uploaded with the five world buffers, and
  `patchChunk()` re-uploads one chunk's rebuilt pool into reserved per-chunk
  regions (relocating/growing as needed) without touching other chunks.
  `splat_pass.{hpp,cpp}` — primary Gaussian-surfel raster backend (sky/opaque/
  water pipelines, chunk draws, frustum culling). The surfel buffer is
  **paged**: each chunk owns a slot run with reserved capacity
  (`m_chunkStart/Count/Cap`, LOD+water in a trailing region) so
  `patchChunkSurfels()` can update one chunk without touching the rest; a
  chunk that outgrows its slot relocates to the opaque free area (growing the
  buffer if needed). `taa_pass.*` resolve. `src/rhi/*` Vulkan + VMA.
- `src/voxel/surfelize.{hpp,cpp}` — CPU surfel extraction from the live
  `VoxelField` (one anisotropic parent per outer surface cell, plus optional
  small hard-edge crease bridges, mean face normal + smoothing, baked CPU
  sun-shadow/AO/bent normal, chunk bucketing + water grid); rebuilt on every
  world reload (`rebuildSurfels`). Full and store paths preserve `Sample::layer`
  into the surfel's packed owner metadata, including deterministic edge
  children. The store path adds `buildChunkSurfels`/`buildChunksSurfels`
  (the BAKE's normal pipeline — exposed-face mean, per-face expansion for
  thin/cancelling cells, face-neighbour smoothing — plus parallel shading) for
  live edits. It must never grow a normal rule of its own: a stamp re-derives
  the edit AABB ± margin, so a divergent rule re-aims untouched splats. See
  `.opencode/wiki/concepts/live-edit-surfel-parity.md`.
- `src/voxel/heightmap.{hpp,cpp}` — terrain source of truth: 16-bit grayscale
  PNG (`kHmSize=2048`, meters `[-8,24]`); bilinear `sample()` + `gradient()`.
- `src/voxel/worldfile.{hpp,cpp}` — VXW v1 binary reader/writer (header + SVO
  buffers + 16 B voxel records); used by `heightmap_gen` bake and `EditableWorld`.
- `src/voxel/picking.{hpp,cpp}` — `rayPick()` against the records-derived
  `VoxelField` for `Ctrl+LMB` selection. `PickHit::layer` is the exact winning
  owner from the picked cell and drives the editor/AI owner label; never infer
  selection from layer AABBs.
- `src/app/` — the application, one directory per subsystem; `src/app/app.hpp`
  is the root header and `src/app/frame/run.cpp` is the only file that knows
  the frame-loop order. The map is in the header comment of `app.hpp`: `cli/`
  (command line), `rhi/` (surface bring-up + the present probe), `world/`
  (layered world, terrain/objvol uploads, runtime-edit overlay, surfel stream),
  `textures/` (the atlas binding table), `edit/` (live-edit brush, rotate/move
  commit), `mesh/` (STL/OBJ import), `ui/` (the one docked sidebar, its six
  sections, the gizmo screen maths), `frame/` (startup, the frame loop and its
  slices, both recording paths, selftest, profiler). `main.cpp` is the entry
  point only. CMakeLists lists every `.cpp` explicitly — a new file that is not
  listed there does not build.
- `src/app/chat_ui.cpp`, `src/ai/*` — chat UI, LLM client/tool parsing, MCP
  server (`vf_mcp`: add_box/cylinder/ellipsoid/stamp, add_voxels, write_object,
  read_object, delete_object, list_layers, enable_layer, probe, ground,
  clear_edits). See the voxel-object skill for how to author arbitrary objects
  via these tools (the `.vxw` file is produced by `write_object`/`add_voxels`,
  never hand-written binary).
- `tools/heightmap_gen.cpp` — offline baker; classifies terrain materials
  against lattice geometry (`latSlope`) so baked materials match what the GPU
  renders. `tools/scene_slice.cpp` — ASCII cross-sections of the field.

## Shaders — data-only rule
- `shaders/svo_raymarch.comp` is the SVO reference shader (+`taa_resolve.comp`,
  `post.comp`); `shaders/splat.{vert,frag}` are the primary
  splat path. Shared lighting lives in `common_base.glsl` (sky/PBR/AO/flora/fog,
  `applyFlora` with per-backend shadow dispatch) + `common_svo.glsl` (SVO
  traversal + `shadeTerrain`) + `common_splat.glsl` (splat marches +
  `shadeSurfel`/`shadeWaterSplat`).
- Surfel layout (80 B, 5×vec4, std430): `pos_rU` (w = radiusU, along the
  stored tangent), `normal_rV` (w = radiusV, across), `bent_sh` (bent normal +
  baked shadow), `mat_ao` (mat/refl/rough/packed metadata), `tan_aspect`
  (xyz = in-plane unit tangent; w = per-cell texture override; zero tangent =
  isotropic disk). `mat_ao.w` is
  `AO + 2` for water and `AO + 8 + 16*ownerId` for a placed object owner
  (IDs 1–254); ID 0 object/live geometry stays unowned. Packing happens after
  AO smoothing, which otherwise averages the metadata away.
  `shaders/common_surfel.glsl` is the shared decoder/encoder contract for the
  forward, GPU-cull, and tile shaders. The selected-owner `RotUBO` carries
  the relative rotation and an optional Move translation; shaders compare the
  decoded owner ID with its row-2 owner lane, so only the selected file's disks
  move. Terrain, water, and every other owner remain fixed. Rasterized
  as instanced quads drawn back-to-front per chunk; fragment does ray/disk
  intersect + one pure 2D Gaussian kernel (`alpha = opacity·exp(-0.5·d2/σ²)`,
  centres fill via source-over accumulation — no opaque core, no rim step).
  A depth-only prepass writes the nearest full-disk plane depth with the
  fixed-function gl_Position depth (no `gl_FragDepth` writes anywhere, so
  all passes keep early-Z and only front fragments shade — this is what
  keeps close-up overdraw bounded). An opaque **base** draw (depth-compare
  EQUAL, no blend) then writes the nearest fragment's colour with alpha 1
  at every covered pixel, so the sky can never bleed through low-alpha disk
  rims; a blended **band** draw applies `-VF_SPLAT_DEPTH_TOL` as a dynamic
  rasterizer depth bias (`vkCmdSetDepthBias`) and LESS-tests against the
  prepass, blending only the front surface's Gaussian coverage over the
  base (same-chunk far surfaces can't source-over in draw order). Water
  tests the prepass depth directly. VS projects with honest `w = vz` (never
  clamp: it smears behind-camera corners into giant blobs); backfaces
  collapse except when a per-frame CPU probe finds the camera buried in
  solid (`setBuried` → two-sided shells).
- Shadows for splats are BAKED per-surfel on the CPU (`shadowMarch` over
  `VoxelField::sample`, binary like SVO `softShadow`); the GPU shadow march
  (`softShadowSplat`) only serves the water path. `objDist` returns METERS
  (`r8_snorm × 1.26`); empty reads exactly `+kObjVolMax` (no info beyond).
  The sidebar toggle is **Shadows** (render-flag **bit 1**, renamed from
  "Sun shadows") and it gates EVERY occlusion march now: the baked sun `sh`
  AND the point-light visibility marches inside `applyLights()` on both
  backends. It also interacts with bit 8: `aoShEnclosure` requires
  `(gRenderFlags & (256|3)) == (256|3)`, so clearing bit 1 (`=253`)
  disables the enclosure proxy too — an A/B that clears only bit 1 changes
  cave shading on both backends by design, and `VF_RENDER_FLAGS=255` (bit 8
  clear) is the pre-lights escape hatch, not a bit-1 one.
- **Enclosed-space shading + light sources** (render-flag **bit 8**, default
  ON — `m_renderFlags` is now 511; `VF_RENDER_FLAGS=255` is the bit-exact
  pre-lights escape hatch). `skyVisibilitySvo` / `skyVisibilitySPlat` cast one
  ray along `mix(bent, +Y, 0.65)` and return 0/1, so a cave or a room reads as
  enclosed instead of open sky. Enclosed surfaces scale the analytic sky
  ambient by `mix(1, 0.16)` and the IBL by `mix(1, 0.04)`, then get an
  albedo-scaled fill (`vec3(0.075,0.08,0.09)·enclosed·(0.35+0.65·ao)`) —
  WITHOUT that fill the removed daylight just pushes them under
  `visual_check`'s 5 % black-in-silhouette gate (measured house 7.21 % → 4.26 %
  with it). **The splat sky test uses `objDist` ONLY**: `heightAt` reports
  "solid" for every point below the terrain surface, which marks any interior
  dug into a hillside (and any floor under a smoothed height texture) as
  underground. A heightfield cannot have overhangs, so terrain can never
  occlude an upward ray; the per-light shadow test still marches terrain,
  because a lamp behind a hill must not light the far side.
- **Light sources**: a top-level `"lights"` array in `world.json`
  (`{"pos":[x,y,z], "color":[r,g,b], "radius":m, "intensity":n}`, max 16,
  `radius<=0`/`intensity<=0` entries are dropped) parsed by
  `worldfile::loadLightManifest` into a std140 UBO at **binding 25** (528 B =
  two `vec4[16]` + count at byte 512 + 3 pad ints), declared once in
  `common_base.glsl` and read by `applyLights()`. Both backends march for the
  light (`lightVisibilitySvo` = one `exactSVOHit`; `lightVisibilitySPlat` =
  the heightfield+objVol binary march). Uploaded by `App::uploadLightSources()`
  — **call it only after every pass is initialised**: `setLights()` writes a
  descriptor of a set that does not exist until `init()` ran, and the silent
  no-op looks exactly like "the lamp is in the manifest but does nothing".
  Measured with one lamp in the cabin: mean luma 42 → 87, pixels under luma 30
  44.9 % → 11.0 %; the splat↔SVO delta is unchanged (-16.7 → -14.3), i.e. the
  light does not add backend divergence.
  **Emissive materials derive lights too**: `uploadLightSources()` builds a
  per-material emission table — palette `kEmissive` mats 9-15 (CPU copy in
  `common.hpp`, GLSL copy in `common_base.glsl`; change both or lit colour
  and glowing colour disagree) plus every texture binding flagged
  `"emissive"` in world.json, read from the ATLAS (`emissiveScale`/
  `meanColor`, both zeroed by `VF_TEXTURES=0`, so the bit-exact escape hatch
  kills derived lights with the glow) — then
  `VoxelField::collectEmissive()` buckets emitter cells on a 1 m grid, sorts
  count-desc/key-asc (same content ⇒ same lights, every reload), thins to
  1.5 m separation (one hearth, not twenty lights) and lifts each centroid
  to the nearest AIR cell, because a light buried in its own emitter is
  occluded by every receiver's march and would occupy a slot contributing
  nothing. AUTHORED lights fill the 16 slots first, derived fill the rest,
  truncation logged. Derived lights are a function of content — never
  persisted (`writeLightManifest` must not learn about them). Re-derived on
  every atlas reload (`reloadTexAtlas`) and world reload; the default hamlet
  derives 14 from `ai_edits` mat 15 + cabin/tower emitters. Texture-flagged
  light colour = the texture's mean colour × 3 × `emissiveScale`, gain 0.5.
- **Photo textures** (both backends): a `world.json` top-level `"textures"`
  array (`{file, mat, scale}` in m/tile, optional `emissive` +
  `emissiveScale` — written ONLY when set, so manifests that never used them
  round-trip byte-identically; paths relative to the manifest)
  binds a PNG/JPG per material into a fixed 17-layer **512²** atlas (layer ==
  mat; `TexAtlas`, bindings 22/23 shared by splat + SVO). `TexAtlas::kTexSize`
  is the only hardcode (mips/staging/blits derive from it); the shipped
  sources are 512², so raising it 256→512 recovered art that was being
  box-filtered away (VRAM 5.9→23.8 MB). Measured effect is real but modest
  (+4.8 % near-field HF energy) — resolution is a sharpness fix, not a
  realism lever; see `.opencode/wiki/concepts/texture-resolution.md`.
  `detailAlbedo`
  replaces the palette albedo with a **triplanar** world-space sample
  (world-locked, no UV unwrap; cannot do unique per-face art) and keeps the
  universal mottling/grain. `detailNormal` (render-flag **bit 7**, key **B**,
  default ON) adds a Sobel-of-luminance bump over the same projection —
  shading normal only, so `oGNorm`/SSAO/SSR are untouched, foliage skipped,
  no-op when untextured (`VF_TEXTURES=0` stays bit-exact); `kDetailRelief`
  0.08 ≈ +40 % HF energy. Missing files/entries fall back to the palette
  and never abort the world; `VF_TEXTURES=0` zeroes the slot table (bit-exact
  escape hatch). A per-cell override rides word1 byte 0 (see "Brick packing"
  below) and `tan_aspect.w`: `sampleTex` takes
  `slot = gTexOv > 0.5 ? round(gTexOv) : matTex[mId]`. The atlas reloads with
  the same layer-poll as world reloads (`reloadTexAtlas`).
  **Swapping is a GUI action**: the sidebar's **Textures** section lists
  every file in `assets/textures/` per material; a change rewrites only the
  `textures` key (`worldfile::writeTextureManifest`, layers + unknown keys
  preserved verbatim) and re-uploads. The shipped photos come from
  `tools/fetch_textures.py` (ambientCG CC0, mean-matched to the palette so a
  swap keeps the material's colour identity); `gen_textures.py` is the
  offline set. See `.opencode/wiki/concepts/texture-atlas.md`
  + `per-cell-texture.md`.
- Terrain: bilinear `heightAt()` over `uHeight` (**rg32f**: R=top world Y,
  G=material/255); terrain material from `.g` (`heightMatNearest`).
- Objects: brick SDF + material byte; **bit 7 of word1.mat = object flag**
  (set by the bake when the object field wins a cell) drives
  `isObjectSurface()` → SVO-gradient normals and brick materials.
- SVO shadows are exact binary DDA hits (`softShadow`); splat shadows are the
  same verdict baked per-surfel on the CPU, so both backends agree. The coarse
  r8_snorm 256³ object volume (clamped ±1.26 m) is only marched on the GPU for
  the splat water path now.
- Water plane `y=-0.9` bidirectional (`waterHit` above+below), `gUnderwater`
  absorption tint, bed-absorption skip when submerged, fog `0.0012`, AgX
  (post pass; the in-shader `aces()` is dead code).
- Brick packing: `word0=r|g<<8|b<<16|sdfByte<<24` (decode `raw*VOXEL`),
  `word1=tex|refl<<8|rough<<16|(mat|objFlag)<<24`. Byte 0 of word1 is the
  **per-cell texture override** (phase 2; was an always-255 `a` filler that
  nothing reads — albedo comes from word0, and the water flag is `mat_ao.w`).
  `ChunkStore` mirrors it as `StoreCell::tags`; 0 = use the material's atlas
  slot. The live-edit overlay schema is 3 because that byte used to be
  uninitialized garbage, so a stale v2 file paints random atlas layers.
  Empty-cell fallback in
  `map()` is `max(-sdBox(p,cmin,cmax), VOXEL*0.5)` + 6-step bisection.
- Push block `RaymarchPush` (128 B, `svo_pass.hpp`): camPos/Right/Up/Fwd,
  `a=(tanHalfFov,aspect,extentX,extentY)`, `b=(worldSize,voxelSize,gridN,_),
  sunDir` toward sun, **`misc.y=animTime_s`** (the struct comment claims
  `misc.x` — the shader and `App::recordInteractiveFrame` actually use
  `misc.y`). Don't reuse
  `a.w`. See `docs/rendering.md`.

## Gotchas
- `WORLD/VOXEL/GRID_N/BRICK_N` are load-bearing; changing one requires
  updating heightmap encode, `worldfile` meta check, and shader constants.
- `heightmap_gen`'s PNG writer is hand-rolled stored-deflate;
  `rowBytes = 1+w*2`.
- `vf_tests` no longer depends on `vf_heightmap`; without baked assets
  `unit_tests` aborts with "run 'ninja -C build world' first" instead of
  triggering a bake.
- No validation layers installed; rely on selftest/visual_check + `VF_TRACE`.
- ImGui tables must respect `BeginTable()`'s bool result. A clipped
  `BrushModes` table at 960×540 returned false, then an unguarded
  `TableSetColumnIndex()` segfaulted. Every table body is guarded, and the
  sidebar clamps its own height/width so the layout fits a 960×540 viewport
  (rail 46 px + pane ≈ 256 px, footer 52 px).
- ImGui uses `UseDynamicRendering`: the app must wrap `ImGui_ImplVulkan_RenderDrawData`
  in its own `vkCmdBeginRendering/vkCmdEndRendering` against the swapchain view
  (`frame/record_interactive.cpp`, LOAD op keeps the blitted world). Omit it and
  the whole UI silently renders nothing - no error anywhere.
- `imgui.ini` persists window positions across sessions; a layout saved by a
  wider display can push panels off-screen. The sidebar is immune: it is
  created `NoSavedSettings` with `ImGuiCond_Always` position/size, so a stale
  ini is ignored entirely (delete `imgui.ini` to reset the rest).
- High-DPI input must stay in logical window pixels: GLFW cursor coordinates
  and ImGui `DisplaySize` are logical, while `framebufferSize()` is physical.
  Picking and trackball projection use `DisplaySize` so activation/ring clicks
  stay aligned with the rendered object.
- `VoxelField::sample` returns quantised (int8) object distances; the shadow
  volume is coarser still (0.4 m texels) — don't use it for shading normals.
- `ChunkPool::root`/handles: `-1` (0xFFFFFFFF) = empty, **`-2` (0xFFFFFFFE) =
  solid terminal**. They are both negative as `int32_t`; a bare `root < 0`
  test wrongly treats solid chunks as empty (the bake's promotion loop relies
  on this ordering — see `ChunkStore::adopt`). `-2` is why promoted solid
  chunks are excluded from `Stats::activeChunks`.
- Baked brick SDFs quantise with `int(d/VOXEL)` (truncation), so cells within
  one voxel of a surface can store `raw == 0`; the SVO DDA treats `sdf <= 0`
  as solid. **Everything that tests brick signs uses `raw <= 0` = solid**
  (`ChunkStore::decodeCell`, rebuild materialisation, `buildPoolOnly`,
  `buildChunkSurfels`); a `< 0` test erodes those cells on every rebuild.
  Store/field sign comparisons must tolerate `±2·VOXEL` at surfaces.
- `ChunkStore::rebuildChunk` dense materialisation: a cell is brick-owned if
  *any* brick covers it, **air or solid** (`fromBrick[]` records that); the
  box/state fill must skip those, else a kept `SolidBox` re-solidifies cells an
  edit just cleared (deep clears silently stayed solid - a live carve then left
  a hidden crust). For the same reason an all-air region block that a kept
  straddling box covers must not drop its brick: `cellAt` checks bricks before
  boxes, so the box would cover the cleared cells again. Regression:
  tests/test_store.cpp "a deep clear stays air under the terrain boxes".
- SVO handles are **chunk-local**: every access adds the owning chunk's base
  from `uChunkInfo` (`common_svo.glsl` `Bases`/`chunkBases`). Never
  offset-adjust handles during the merge (`layered_world.cpp`) or reuse a
  node/brick index across chunks. `SvoPass::patchChunk` writes into reserved
  per-chunk regions: payload and childBase share the node-index space (both
  must be grown together), and bricks are counted in `BRICK_WORDS` (the SSBO
  size is ×4096 bytes, not ×4).
- Live edits (M1–M3): `assets/runtime_edits.vxw` holds the persisted edited
  chunks (VXW v2 store section); it is **not** in `world.json`, so the layer
  poll ignores it — the app loads it explicitly at startup / after every world
  reload (`App::loadStoreOverlay`) and patches the chunks. Patched chunks keep
  their full run (`LiveEditor::chunkRun`) but lose their LOD ring until the
  next full reload; `uHeight`/water stay stale in
  the edited region; both patches stall the device (`vkDeviceWaitIdle`), so
  painting is click/drag-scale, not a free-running sculpt loop. Rebuild-region
  gotcha: `Chunk::lo/hi` store **global lattice coords** (the region math
  converts to chunk-local), and solid boxes that straddle the region boundary
  are kept (`bricks win over boxes` in `cellAt` makes the overlap harmless).
  `Solid`/`Empty` chunks have **no `slotOf` table** — guard it (`hasSlots`) in
  any rebuild path and skip `Solid` chunks entirely (a dirty Solid neighbour
  used to segfault `rebuildChunk`).
- `Chunk::edited` (set by `apply`) drives the overlay serialization. Clearing
  it or dropping chunks silently loses persistence; `serializeEdited` only
  walks flagged chunks.
- `SplatPass::patchChunkSurfels` must accept **zero-surfel** patches: an empty
  run's `data()` is null, and the old `!data` guard silently skipped the patch,
  so a chunk emptied by an undo/Clear kept drawing its stale run (the store and
  the LiveEditor cache were already correct — the GPU patch was the only broken
  layer). Zeroing `m_chunkCount` (+ LOD flags) is enough; there is
  nothing to copy.
- Load cost is bbox-bound: `VoxelField::build` pays a padded-bbox EDT per
  object component — 72k components / 533k cells but **361M padded bbox cells**
  (~680x, `kPad` ~8 around 1-cell grass/pebble parts) = ~13 s of the ~17.6 s
  startup, which every `--shot` run and test render repeats (parallel renders
  do not help: the load already saturates the cores). Batching spatially-near
  components into one group is the intended fix; do not shrink `kPad` (it
  carries the stored SDF air band).
- Docs: `README.md` (product), `AGENTS.md` (this file), `docs/` (full dev
  documentation, start at `docs/index.md`),
  `docs/history/` (`rework.md` architecture plan, `ImplementationPlan.md`
  roadmap, `THREAD_SUMMARY.md` design log — historical).

<!-- opencode-crosstalk:begin -->
## OpenCode Crosstalk

Other OpenCode sessions in this workspace are reachable through the
`opencode-crosstalk` plugin: `crosstalk_status`, `crosstalk_peers`,
`crosstalk_send`, `crosstalk_inbox`, `crosstalk_claim`, `crosstalk_wait`.
Talk to each other, but keep working — only stop for coordination that prevents
a real collision.

- **Declare once, then keep moving.** `crosstalk_status` sets your role and goal;
  `crosstalk_peers` shows active sessions and their leases. Work that does not
  overlap theirs needs no coordination.
- **Talk before you collide.** If you need something a peer holds, `crosstalk_send`
  a short ask and continue elsewhere; replies are injected into live turns (use
  `crosstalk_inbox` to catch up). Never force a claim.
- **Lease what you are editing now.** `crosstalk_claim` takes an exclusive expiring
  lease on exact paths — no globs (`resources`, `note`, `ttlSeconds`). `renew` if the
  work runs long, `release` when done; a refusal names the holder.
- **Check the shared surfaces here.** `crosstalk_peers` before a render or rebuild:
  the GPU/display, the `build/` tree, `shaders/*.glsl` (hot-reloaded into a peer's
  running render), the live-edit overlay `assets/runtime_edits.vxw`, and the authored
  `assets/*.vxw` / `world.json` are shared — concurrent renders, rebuilds or writes
  there corrupt a peer's state. Use `VF_OVERLAY_PATH=/tmp/…` for every scratch render
  and label scratch/backup files so ownership is obvious.
- **Do not destroy what you did not create.** Never `ninja -C build clean` (it
  deletes tracked assets), never `rm` a glob near a peer's files, and never kill a
  process you did not start — wait with `crosstalk_wait` or ask with
  `crosstalk_send` instead.
- **Identity is automatic** — never pass a "who am I". Blocking calls are capped by
  `maxWaitMs` and may return early; that is normal.

Installed globally (`npm run setup` in `/home/christoph/code/opencode-crosstalk`);
`opencode api get /api/plugin` shows `opencode.crosstalk` active. Disable per
workspace with `"plugins": ["-opencode.crosstalk"]`; no permission rule is required.
<!-- opencode-crosstalk:end -->

<!-- BEGIN opencode-rag -->
## Code Navigation

ALWAYS use OpenCodeRAG tools before reading or editing:
- **Search first** — `search_semantic(query)` instead of grep/glob
- **Skeleton before read** — `get_file_skeleton(filePath)` then read specific lines
- **Usages before edit** — `find_usages(symbolName)` before modifying any symbol
- **Images via describe** — `describe_image(filePath, systemPrompt?)` — never read raw bytes
- **Recall quirks** — `recall_quirks(query)` when you hit a known pitfall
- **Add quirks** — `add_quirk(content)` when you discover a non-obvious fact
- **Fix quirks** — `update_quirk(id, ...)` / `delete_quirk(id)` when a stored quirk is outdated or wrong

If no results, run `opencode-rag index`.

### Decision model (`make_decision`)

- Classify, route, or score short text with the local tev1 model (`state` + 1-64 questions).
- Question types: `choice` (options map to descriptions), `noul` (true/false probability), `score` (ordered rubric levels, lowest first).
- Keep `state` short (~2k-token context); add a `none` option when no listed option may fit.
- Never use it as the only check for a high-stakes decision.
- With `decision.routeBeforeAsking` enabled: route option choices through `make_decision` before asking the user; ask the user only when the model is undecided (low confidence) or the choice is preference-based.

### Decision tree — ALWAYS follow this order
1. User mentions code behavior/architecture → `search_semantic(query)`
2. User mentions a file path → `get_file_skeleton(filePath)` THEN `read` on specific lines
3. User mentions a function/class/variable to edit → `find_usages(symbolName)` THEN `search_semantic` THEN `edit`
4. User asks a code question → `search_semantic` to gather context before answering
5. User asks about an image or visual asset → `describe_image(filePath)` (optionally pass `systemPrompt` to focus on specific features) to retrieve its generated description, then optionally `search_semantic` for related code
6. You encounter an error or need to recall a known pitfall → `recall_quirks(query)`
7. You discover a non-obvious fact or workaround → `add_quirk(content)` to persist it for future sessions
8. A recalled quirk is outdated or wrong → `update_quirk(id, ...)` to fix it, or `delete_quirk(id)` if it no longer applies

### Proactive triggers — you MUST call these tools when
- User asks about code behavior, architecture, or implementation details
- User asks to edit, refactor, or fix code — call `find_usages` first
- User references files or functions you haven't read yet
- User says "find", "search", "look up", "where is", "how does"
- User refers to an image, screenshot, diagram, or visual asset
- Before answering ANY code-related question, retrieve context first
- Before reading ANY file, call `get_file_skeleton` to orient first

### Anti-patterns — NEVER do these
- Reading full files without calling `get_file_skeleton` first (wastes tokens)
- Editing a function without calling `find_usages` first (breaks call sites)
- Answering code questions without calling `search_semantic` first (you guess at behavior)
- Using `grep`/`glob` when `search_semantic` would find the answer faster
- Treating image files as text — use `describe_image` instead of reading raw bytes
- Using `npx opencode-rag quirk` shell commands instead of the built-in quirk tools (`add_quirk` / `recall_quirks` / `update_quirk` / `delete_quirk`) (the tools are faster, already loaded in-process, and go through the trust monitor)

### MANDATORY quirk capture rules — you MUST call `add_quirk` when
- A build, test, or type-check command fails and you resolve it
- You discover an undocumented library constraint, peer dep, or workaround
- You learn an environment-specific requirement (OS, tool version, etc.)
- You make a design decision that future sessions should remember
- You resolve a gotcha that cost more than one attempt

### MANDATORY quirk hygiene — you MUST call `update_quirk` or `delete_quirk` when
- A stored quirk is outdated, wrong, or has been fixed — update it or delete it instead of adding a contradicting duplicate
- NEVER finish a coding session without adding quirks for resolved errors.
<!-- END opencode-rag -->
