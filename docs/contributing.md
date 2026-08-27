# Contributing / developer guide

## Project invariants — violating these is a bug

1. **Records-only geometry.** All runtime geometry derives from `.vxw`
   records via `VoxelField`. Never add analytic scene code to the renderer or
   the shader; never reintroduce a merged cache file or a second backend.
2. **Single render path.** `shaders/svo_raymarch.comp` (+ `taa_resolve.comp`)
   are the only render shaders.
3. **Load-bearing constants** (`src/voxel/common.hpp`): `WORLD=102.4`,
   `VOXEL=0.1`, `CHUNK_N=64`, `GRID_N=16`, `BRICK_N=8`, `BRICK_WORDS=1024`,
   `WATER_LEVEL=-0.9`. Changing one requires synchronized updates to: the
   heightmap encode, the worldfile meta checks (`worldfile.cpp`,
   `editable_world.cpp`, `layered_world.cpp`), and shader constants
   (`BRICK_WORDS`, water level). Tests will catch mismatches — but only if
   you run them.
4. **Shader data-only rule.** Terrain comes from `uHeight` (rg32f top-Y +
   material), objects from bricks, shadows add `uObjVol`. Palette/material
   tables exist both CPU-side (`kPalette`, `kMaterialReflection`) and as GLSL
   constants — keep them identical when touching materials.
5. **GPU contracts**: 128 B push block, brick word packing with object-flag
   bit 7 of the word1 material byte, handle encoding — all specified in
   [rendering.md](rendering.md). Don't reuse reserved fields (`a.w`).
6. **Determinism.** Variation only via `hash2`-family noise; no wall-clock or
   RNG state in geometry. Headless shots must be reproducible; synthesis
   chunk order must be deterministic (VF_TRACE hash logs guard this).
7. **Explicit asset bake.** Nothing may auto-run `heightmap_gen`. Missing
   assets = clean error telling the user to run `ninja -C build world`.
8. **ai_edits.vxw is sacred.** The baker preserves it; only `EditableWorld`
   writes it.

## Code conventions

- C++20, compiled with `-Wall -Wextra`. Namespaces: `vf::voxel`, `vf::ai`,
  root `vf` for render/rhi/platform.
- Header doc comments: plain `//` block at file top stating purpose +
  invariants (see `voxel_field.hpp`, `worldfile.hpp`, `tools.hpp` for the
  house style). No Doxygen tooling.
- Header-only where it aids unit testing without linking app deps
  (`ai/tools.hpp`, `voxel/common.hpp`) — keep that split deliberate.
- Logging via spdlog; MCP server logs **only to stderr**.
- Dependencies exclusively through CMake FetchContent (spdlog, glfw, glm,
  VMA, doctest, imgui); vendored headers under `external/` (stb) are the
  exception.
- Keep diffs small and single-purpose; match surrounding style.

## Gotcha catalog

- **No validation layers installed.** Correctness gates are selftest /
  visual_check / unit tests + `VF_TRACE`; don't rely on VK_LAYER feedback.
- **ImGui + dynamic rendering**: `ImGui_ImplVulkan_RenderDrawData` must be
  wrapped by the app's own `vkCmdBeginRendering/EndRendering` against the
  swapchain view with LOAD op (keeps the blitted world). Omit it and the UI
  silently renders nothing.
- **Present mode**: default IMMEDIATE on NVIDIA+X11 (MAILBOX deadlocks);
  per-swapchain-image acquire semaphores in the frame loop.
- **`imgui.ini`** persists panel layouts across sessions and can push panels
  off-screen; delete it to reset.
- **Quantized distances**: `VoxelField::sample` returns int8-quantized object
  distances; `uObjVol` is coarser still (0.4 m texels). Neither is valid for
  shading normals — use brick SDFs.
- **misc.y carries animation time** despite the struct comment saying
  `misc.x`; see [rendering.md](rendering.md#push-constant-block--raymarchpush-128-b).
- **Tall authored objects** need a bake-band extension: `nearObject()` in
  `tools/heightmap_gen.cpp` decides which columns get an expanded y-sampling
  band; outside existing radii only terrain ±3 voxels are recorded.
- **Baker sweeps are hot** (~10 M SDF evals per regen): give scatter shapes a
  cheap reject (distance²/vertical cull) like `treesAt` does.
- **Hand-rolled PNG writer** in heightmap_gen: stored-deflate,
  `rowBytes = 1 + w*2`.
- **Manifest key is `"rot"`** even though the struct field is `rotDeg`;
  `pos`/`rotDeg` are informational bake-time metadata, never applied at
  runtime.
- **Layer priority** is manifest array order (first wins a cell); `ai_edits`
  sits first among objects. Landscape changes force full rebuilds; other
  layers rebuild only their dirty chunk range (+2 margin).
- **Stray artifacts to ignore/clean**: `comp.spv` at repo root (leftover
  SPIR-V), `tests/screenshots/cmp_*.png` (removed compare mode).

## Recipes

### Add a new material id (next id = 9+)

1. Append color to `kPalette` and refl/rough to `kMaterialReflection`
   (`src/voxel/common.hpp`).
2. Mirror both arrays in `shaders/svo_raymarch.comp` (`kPalette`, `kMatRefl`).
3. Update the palette list in `src/ai/system_prompt.hpp`.
4. Widen clamps: `std::clamp(mat, 0, 8)` in `src/ai/mcp_server.cpp`,
   `std::min(int(mat), 8)` index guards, and `injectToolDefaults` name→mat map.
5. Consider `materialFromBands()` if terrain should ever select it.
6. Run the full gate chain ([testing.md](testing.md)).

### Add an MCP tool

1. Implement handler in `struct Server` (`src/ai/mcp_server.cpp`).
2. Register in `toolSchemas()` (name/description/inputSchema).
3. Dispatch in `callTool()`; add to the `kNative` list so it bypasses fuzzy
   normalization (unless you want aliases).
4. Document it in README's tool table and [ai-editing.md](ai-editing.md).

### Add an authored world object

Authoring loop (layer-by-layer, verify each step):

1. Write an analytic shape `xxxAt(p) -> ObjHit` in `src/voxel/common.hpp`
   using the existing primitives (`sdBoxF`, `sdCylY`, `sdCapsule`,
   `sdEllipsoid`, `sdConeY`, stamps); cheap-reject early; comment layers
   bottom-up.
2. Sweep it into a layer in `tools/heightmap_gen.cpp` (extend `nearObject()`
   band if tall); run `ninja -C build world`.
3. Verify per layer: `./build/vf_slice --axis z --center X Y Z --span 10`
   and `./build/voxelforge --probe X Y Z` (no GPU needed).
4. Final acceptance: close-up `--shot`s judged via ascii_view.py + full ctest.
   See `.opencode/skills/voxel-object/SKILL.md` for the detailed loop.

Data-only variants: drop/edit a `.vxw` layer directly, or place copies at the
picked anchor via the GUI Import button / `EditableWorld::importLayer`.

### Change world constants

Follow invariant #3's checklist, then regenerate assets and re-run every
gate. Expect test fixture updates (probes use absolute coordinates).

### Touch the reload path

Remember both triggers converge on `applyWorldReload()`: device idle → swap
five SSBOs → re-upload terrain texture + shadow volume → rescan layer list.
Any new GPU-side derived data must be refreshed there too.

## Pre-change checklist

- [ ] Invariants above respected (records-only, data-only shaders)
- [ ] `ninja -C build` clean
- [ ] `ctest --test-dir build` green (unit + visual_check)
- [ ] `--selftest` green after shader/world changes
- [ ] New behavior covered by a unit test where practical (header-only logic is cheap to test)
- [ ] Docs updated when contracts changed (this directory + AGENTS.md + README)
