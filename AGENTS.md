# AGENTS.md

> **Scope:** The whole project is a **single-path chunked-SVO voxel renderer**
> with AI-driven world editing. Splats, the dense reference raymarcher and all
> analytic runtime geometry are **gone** — geometry is derived solely from
> `.vxw` records via `VoxelField` (`rework.md`). Keep this invariant in every
> change.

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
- **Default world = terrain only**: world.json ships with `landscape` +
  `ai_edits` enabled; every other baked layer is opt-in via the "World layers"
  panel (a plain .vxw file list). Toggling triggers an incremental rebuild and
  refreshes SVO + terrain texture + shadow volume together (`applyWorldReload`).
  Content tests generate their own all-enabled manifest (`world_all.json`).
- Don't hand-edit derived `.vxw`; edit terrain in
  `tools/heightmap_gen.cpp:terrainHeightAt()`, authored shapes in
  `src/voxel/common.hpp` (baker-side analytics), then `ninja -C build world`.
  Data-only changes can go straight into object `.vxw` layers.
- `assets/ai_edits.vxw` is the live layer for chat/MCP edits; `vf_mcp`
  appends + saves immediately.

## Run
- `./build/voxelforge` — hero cam `-16,6.5,-14 → 6.5,0.8,11`, sun `34°/238°`.
- Keys: `WASD/QE` move, `RMB+mouse` look, wheel speed, `Ctrl+LMB` pick anchor,
  `ESC` quit.
- Headless: `--selftest`, `--smoke N`, `--shot out.ppm --cam …`,
  `--probe X Y Z`, `--sun <elev> <azim>`, `--animtime <s>`, `--width/--height`.
- Chat backend: Ollama defaults or any OpenAI-compatible server via
  `VF_LLM_URL=http://host:8080/v1 VF_LLM_MODEL=… ./build/voxelforge`.
  MCP: `./build/vf_mcp` (stdio), registered in `.opencode/opencode.json`.
- Present quirk: default IMMEDIATE on NVIDIA+X11 (`VF_PRESENT=immediate|mailbox`),
  per-swapchain-image acquire semaphores (`src/rhi/swapchain.cpp:53`).

## Tests & verification — run in order
- `ninja -C build && ctest --test-dir build` runs `unit_tests` (doctest) +
  `visual_check` (headless PPM). Must pass before any shader/world change is done.
- `./build/vf_tests --test-case="*world*"` for a single suite.
- `python3 tests/visual_check.py build/voxelforge` — hero/house/water shots;
  coverage 3–97 %, black-in-silhouette <5 %, blue sky probe.
- `./build/voxelforge --selftest --width 640 --height 360` — sky probe +
  coverage acceptance.
- `--probe X Y Z` reflects the live layered field (loads `world.json`).

## Architecture
- `src/voxel/voxel_field.{hpp,cpp}` — **the geometry oracle**, built from merged
  records: terrain columns (top Y + material), object components flood-filled
  to solids with two-pass Dijkstra signed distance grids stored in a sparse
  hash; emits the GPU height texture (rg32f topY+mat), an object presence block
  mask, and the coarse r8_snorm object volume for shadows.
- `src/voxel/layered_world.{hpp,cpp}` — manifest load/poll/priority merge,
  per-chunk SVO synthesis with resident `ChunkPool`s for incremental rebuilds,
  water-volume marking below `WATER_LEVEL=-0.9`.
- `src/voxel/editable_world.{hpp,cpp}` — `ai_edits.vxw` writer
  (box/cylinder/ellipsoid/stamp rasterizers, bottom-center anchor).
- `src/voxel/common.hpp` — constants (`WORLD=102.4`, `VOXEL=0.1`, `GRID_N=16`,
  `BRICK_N=8`, palette/material tables, spot constants) **and baker-side
  analytic shapes** (`houseAt/treesAt/…`, used by `heightmap_gen` sweeps and
  tests as authoring truth — NOT linked into the renderer path).
- `src/render/svo_pass.{hpp,cpp}` — compute pipeline; `RaymarchPush` (128 B)
  lives here. `taa_pass.*` resolve. `src/rhi/*` Vulkan 1.3 + VMA.
- `src/app/main.cpp` — window/swapchain/frame loop/HUD/picking wiring.
- `src/app/chat_ui.cpp`, `src/ai/*` — chat UI, LLM client/tool parsing, MCP
  server (`vf_mcp`: add_box/cylinder/ellipsoid/stamp, list_layers,
  enable_layer, probe, ground, clear_edits).
- `tools/heightmap_gen.cpp` — offline baker; classifies terrain materials
  against lattice geometry (`latSlope`) so baked materials match what the GPU
  renders. `tools/scene_slice.cpp` — ASCII cross-sections of the field.

## Shaders — data-only rule
- `shaders/svo_raymarch.comp` is the only render shader (+`taa_resolve.comp`).
- Terrain: bilinear `heightAt()` over `uHeight` (**rg32f**: R=top world Y,
  G=material/255); terrain material from `.g` (`heightMatNearest`).
- Objects: brick SDF + material byte; **bit 7 of word1.mat = object flag**
  (set by the bake when the object field wins a cell) drives
  `isObjectSurface()` → SVO-gradient normals and brick materials.
- Shadows march `min(heightfield distance, objDist(uObjVol))` — the coarse
  r8_snorm 256³ object volume (clamped ±1.26 m) keeps AI-added objects casting
  shadows without terraced-terrain self-shadowing.
- Water plane `y=-0.9` bidirectional (`waterHit` above+below), `gUnderwater`
  absorption tint, bed-absorption skip when submerged, fog `0.0012`, ACES.
- Brick packing: `word0=r|g<<8|b<<16|sdfByte<<24` (decode `raw*VOXEL`),
  `word1=a|refl<<8|rough<<16|(mat|objFlag)<<24`. Empty-cell fallback in
  `map()` is `max(-sdBox(p,cmin,cmax), VOXEL*0.5)` + 6-step bisection.
- Push block `RaymarchPush` (128 B, `svo_pass.hpp`): camPos/Right/Up/Fwd,
  `a=(tanHalfFov,aspect,extentX,extentY)`, `b=(worldSize,voxelSize,gridN,_)`,
  `sunDir` toward sun, `misc.x=animTime_s`. Don't reuse `a.w`.

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
- Docs: `README.md` (product), `AGENTS.md` (this file),
  `rework.md` (architecture plan), `ImplementationPlan.md` (history/roadmap),
  `THREAD_SUMMARY.md` (design history).
