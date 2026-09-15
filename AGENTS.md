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

## Assets — on-demand, gitignored
- `assets/heightmap.png` and `assets/*.vxw` + `assets/world.json` are baked by
  `heightmap_gen` (terrain shell records, authored object layers, and the
  highest-priority `ai_edits.vxw`, which the tool preserves across regens).
- **Runtime truth = records.** `LayeredWorld` merges enabled layers
  (first-wins-a-cell), builds `VoxelField`, and synthesizes the SVO.
  `reloadIfChanged()` polls mtimes every ~0.5 s; GUI toggles reload instantly.
- **Default world = full reference scene**: world.json ships with every baked
  layer enabled (cabin + conifer forest + orchard + dock/canoe + shore + ferns +
  bridge + terrain); layers are opt-out via the "World layers" panel (a plain
  .vxw file list). Toggling triggers an incremental rebuild and refreshes SVO +
  terrain texture + shadow volume together (`applyWorldReload`). Content tests
  generate their own all-enabled manifest (`world_all.json`).
- **Layer files hold absolute lattice coords; enabling shows the object where
  it was baked.** To place a copy at the picked anchor, use the per-layer
  "Import" button (or `EditableWorld::importLayer(path, anchor)`): it
  translates the layer's records so their bottom-center lands on the selection
  and appends only that object to `ai_edits.vxw`. The `pos`/`rotDeg` manifest
  fields are informational (bake-time), never applied at runtime.
- Don't hand-edit derived `.vxw`; edit terrain in
  `tools/heightmap_gen.cpp:terrainHeightAt()`, authored shapes in
  `src/voxel/common.hpp` (baker-side analytics), then `ninja -C build world`.
  Data-only changes can go straight into object `.vxw` layers.
- `assets/ai_edits.vxw` is the live layer for chat/MCP edits; `vf_mcp`
  appends + saves immediately.

## Run
- `./build/voxelforge` — reference cam `1.0,2.0,1.5 → 5.3,1.0,11.3` (house.jpeg view), sun `34°/238°`.
- Keys: `WASD/QE` move, `RMB+mouse` look, wheel speed, `Ctrl+LMB` pick anchor,
  `F` toggles splats/SVO renderer, `N` toggles TAA, `[`/`]` shrink/grow splat disks, `ESC` quit.
  `C` toggles the edit tool (Carve/Add/Delete/Paint radios in its panel).
- Headless: `--selftest`, `--smoke N`, `--shot out.ppm --cam …`,
  `--probe X Y Z`, `--sun <elev> <azim>`, `--animtime <s>`, `--width/--height`,
  `--mode splat|svo` (default `splat`; SVO is the pixel reference).
- Splat perf knobs (all env, default tuned): `VF_LOD=0` disables the baked
  merged-terrain LOD rings (surfelize 2x2x2 / 4x4x4, draw-time selection
  `VF_LOD1`/`VF_LOD2` = 20/60 m; objects ride along unmerged — trees must
  never vanish), `VF_MICRO_DIST`
  (default 20 m micro cull), `VF_NO_GPU_CULL=1` disables the GPU per-surfel
  cull pre-pass (compute compaction + GPU-written indirect counts;
  bit-exact), `VF_NO_OCCL=1` disables the Hi-Z occlusion pyramid + GPU
  occlusion cull (~3 ms overhead at 720p; saves fragments when objects
  occlude the background). The depth prepass itself always runs: it is the
  opaque/water depth-resolve reference the base and band passes test
  against. `VF_SPLAT_DIRECT`/`VF_NO_INDIRECT_BARRIER` legacy A/B paths.
- Splat kernel knobs: `VF_SPLAT_SIGMA` (Gaussian variance in normalized
  disk units, default 0.5), `VF_SPLAT_OPACITY` (centre alpha, default 0.9),
  `VF_SPLAT_DEPTH_TOL` (depth-resolve band in NDC depth units, default
  0.002 ≈ 10 cm; applied per frame as a dynamic rasterizer depth bias),
  `VF_SPLAT_EXTENT` (quad half-size), `VF_SPLAT_RADIUS`.
- Live edit: the edit panel (`C`, window "Carve / Add") has four brush
  modes — **Carve** (depth-limited cylinder scoop along the surface normal),
  **Add** (dome), **Delete** (clear every cell in the brush ball) and **Paint**
  (recolour the brush ball with the panel's Material combo) — plus a
  per-mode/size/perf readout and a "Clear live edits" button (drops
  `runtime_edits.vxw`). **Every stamp patches the live store** (there is no
  bake/record path any more; the legacy `carve_edits.vxw`/`raise_edits.vxw`
  layers are read-only leftovers). Carve/Delete (`Clear`) **respect the water
  level**: a scoop aimed at submerged ground is refused outright and no
  subtractive brush clears a cell whose centre is below `WATER_LEVEL=-0.9`, so
  the bed stays watertight (the log reports the held-back cell count).
  In the splat backend, hovering
  with the tool active **tints the affected splats** (the exact brush volume:
  the carve cylinder or the delete/paint ball) so the LMB result is visible
  first — bind 13 `BrushUBO` (`SplatPass::setBrush`), tested against the surfel
  *centre* (per-splat, matching the CPU rasterizer's cell set) plus a 0.06 m
  skin, with `bFlags.x` clipping subtractive brushes at the water plane; debug
  view `VF_SPLAT_DEBUG=15` shows the volume directly. LMB **paints**: holding
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
  store section, schema 2 = per-chunk edit AABB) and restored at the next
  startup / world reload (GPU-seed + refresh the saved AABB, so the restored
  frame matches the session). Patched chunks drop their micro tail + LOD ring
  until the next full reload; water and `uHeight` stay stale in the edited
  region. Headless hooks: `VF_TEST_EDIT="x,y,z,carve|add|delete|paint"` (one
  stamp), `VF_TEST_BRUSH="x,y,z,carve|delete|paint"` (activates the tool and
  renders only the hover preview, no edit), `VF_TEST_STROKE="x,y,z,steps[,mode]"`
  + `VF_TEST_STROKE_SAVE=1` (drag simulation + persistence),
  `VF_EDIT_DIAM`/`VF_EDIT_DEPTH` (brush size), `VF_LIVE_NOSPLAT=1` /
  `VF_LIVE_NOSVO=1` (skip one backend), `VF_NO_OVERLAY=1` (ignore
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
- Chat backend: Ollama defaults or any OpenAI-compatible server via
  `VF_LLM_URL=http://host:8080/v1 VF_LLM_MODEL=… ./build/voxelforge`.
  MCP: `./build/vf_mcp` (stdio), registered in `.opencode/opencode.json`.
- Present quirk: default IMMEDIATE on NVIDIA+X11 (`VF_PRESENT=immediate|mailbox`),
  per-swapchain-image acquire semaphores (`src/app/main.cpp`: `m_acquireSems`
  at :148, created in `ensureAcquireSemaphores()` at :177, used at :923).

## Tests & verification — run in order
- `ninja -C build && ctest --test-dir build` runs `unit_tests` (doctest) +
  `visual_check` + `live_edit_check` (headless PPM). Must pass before any
  shader/world change is done.
- `./build/vf_tests --test-case="*world*"` for a single suite.
- `./build/vf_tests --test-case="chunk*"` — ChunkStore foundation suite
  (`tests/test_store.cpp`): adoption vs `VoxelField`, cell edits + rebuild,
  store-based per-chunk surfels following edits.
- `python3 tests/visual_check.py build/voxelforge` — hero/house/water shots;
  coverage 3–98.5 %, black-in-silhouette <5 %, blue sky probe.
- `python3 tests/live_edit_check.py build/voxelforge` — renders the hero view
  untouched and with `VF_TEST_EDIT` (live store patch) in **both backends**
  (splat + `--mode svo`) for Add, plus the splat-only Delete/Paint store modes
  (micro detail off so the diff is geometry, not the dropped micro tail), the
  carve-brush hover tint and the water-level rules (a submerged scoop is
  refused pixel-identically, a deep scoop reports the held-back cells, and no
  subtractive preview tints the water plane); asserts a visible but bounded
  pixel diff and sane edited-frame probes.
- `./build/voxelforge --selftest --width 640 --height 360` — sky probe +
  coverage acceptance.
- `--probe X Y Z` reflects the live layered field (loads `world.json`).

## Architecture
- `src/voxel/voxel_field.{hpp,cpp}` — **the load-time geometry oracle**, built
  from merged records: terrain columns (top Y + material), object components
  flood-filled to solids with two-pass Dijkstra signed distance grids stored in
  a sparse hash; emits the GPU height texture (rg32f topY+mat), an object
  presence block mask, and the coarse r8_snorm object volume for shadows.
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
  water-volume marking below `WATER_LEVEL=-0.9`.
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
  `VoxelField` (one anisotropic 2D Gaussian per outer surface cell, mean
  face normal + smoothing, baked CPU sun-shadow/AO/bent normal, chunk
  bucketing + water grid); rebuilt on every world reload (`rebuildSurfels`).
  The store path adds `buildChunkSurfels`/`buildChunksSurfels` (SDF-gradient
  normals, parallel shading) for live edits.
- `src/voxel/heightmap.{hpp,cpp}` — terrain source of truth: 16-bit grayscale
  PNG (`kHmSize=2048`, meters `[-8,24]`); bilinear `sample()` + `gradient()`.
- `src/voxel/worldfile.{hpp,cpp}` — VXW v1 binary reader/writer (header + SVO
  buffers + 16 B voxel records); used by `heightmap_gen` bake and `EditableWorld`.
- `src/voxel/picking.{hpp,cpp}` — `rayPick()` against the records-derived
  `VoxelField` for `Ctrl+LMB` anchor selection.
- `src/app/main.cpp` — window/swapchain/frame loop/HUD/picking wiring.
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
- Surfel layout (64 B, 4×vec4, std430): `pos_rU`, `normal_rV`, `bent_sh`
  (bent normal + baked shadow), `mat_ao` (mat/refl/rough/AO+2·water). Rasterized
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
  `word1=a|refl<<8|rough<<16|(mat|objFlag)<<24`. Empty-cell fallback in
  `map()` is `max(-sdBox(p,cmin,cmax), VOXEL*0.5)` + 6-step bisection.
- Push block `RaymarchPush` (128 B, `svo_pass.hpp`): camPos/Right/Up/Fwd,
  `a=(tanHalfFov,aspect,extentX,extentY)`, `b=(worldSize,voxelSize,gridN,_),
  sunDir` toward sun, **`misc.y=animTime_s`** (the struct comment claims
  `misc.x` — the shader and main.cpp actually use `misc.y`). Don't reuse
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
- ImGui uses `UseDynamicRendering`: the app must wrap `ImGui_ImplVulkan_RenderDrawData`
  in its own `vkCmdBeginRendering/vkCmdEndRendering` against the swapchain view
  (`main.cpp`, LOAD op keeps the blitted world). Omit it and the whole UI
  silently renders nothing - no error anywhere.
- `imgui.ini` persists window positions across sessions; a layout saved by a
  wider display can push panels off-screen. The AI Chat window self-heals its
  position every frame; delete `imgui.ini` to reset all panels.
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
  reload (`App::loadStoreOverlay`) and patches the chunks. Patched chunks lose
  micro/LOD rings until the next full reload; `uHeight`/water stay stale in
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
- Docs: `README.md` (product), `AGENTS.md` (this file), `docs/` (full dev
  documentation, start at `docs/index.md`),
  `docs/history/` (`rework.md` architecture plan, `ImplementationPlan.md`
  roadmap, `THREAD_SUMMARY.md` design log — historical).
