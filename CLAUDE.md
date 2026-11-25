# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` (~1900 lines) is the authoritative, detailed convention sheet — read the relevant section before touching a subsystem. Full docs live in `docs/` (start at `docs/index.md`). This file is the short orientation.

## What this is

VoxelForge: a Vulkan 1.3 voxel world renderer/editor. The world is rendered primarily as **Gaussian surfels** (`--mode splat`, default), with a chunked-SVO raymarcher as the pixel reference (`--mode svo`). Invariant: geometry comes **only** from `.vxw` records via `VoxelField`/`ChunkStore` — there is no analytic runtime geometry and no dense reference raymarcher.

## Build & run

Requires Vulkan 1.3, `glslangValidator`, CMake ≥3.24, Ninja, Python3. Deps come via FetchContent.

- `ninja -C build` — builds binaries and compiles shaders to `build/shaders/*.spv`.
- `ninja -C build world` — bakes the default scenery terrain (`heightmap_gen`). Explicit only; required after a clean checkout or the app/tests exit with "run ninja -C build world".
- `./build/voxelforge` (or `./start.sh`) — run the app. Useful flags: `--scenery NAME`, `--shot out.ppm --cam x y z tx ty tz`, `--probe X Y Z`, `--selftest`, `--smoke N`, `--mode splat|svo`, `--width/--height`.
- **Stale objects**: if `ninja` says "no work to do" or a failing test names a line not in your edit, `touch` the file and rebuild.
- **NEVER `ninja clean`**: it deletes tracked authored assets (`scenery/*/heightmap.png`, `world.json`, `landscape.vxw`). Recover with `git checkout -- scenery/…`, not by re-baking.
- After adding a `.cpp`, list it in `CMakeLists.txt` (files are listed explicitly).

## Tests — group-only

Never run bare `ctest` or an all-tests target; every CTest entry is gated by `tests/group_gate.py`. Build, then run the smallest covering group:

`ninja -C build test-unit | test-app | test-surfel | test-store | test-world | test-live-edit | test-visual | test-effects | test-textures | test-fog | test-preview | test-micro | test-smoke` (`test-fast` = smoke).

- Single doctest filter while iterating: `./build/vf_tests --test-case="chunk*"` (not a substitute for a group).
- Brush hover preview changes → `test-preview`; store/stamp/undo/overlay → `test-store`/`test-live-edit`; shader/camera/scene → `test-visual`.
- Python gates live in `tests/*.py` (e.g. `python3 tests/visual_check.py build/voxelforge`, `tests/live_edit_check.py --only preview`).
- `test-live-edit` has known-red `object_smooth`/`object undo` failures on the shipped hamlet (see AGENTS.md); don't read a changed failure count as a fix.
- Headless renders are GPU/display-shared: use `VF_OVERLAY_PATH=/tmp/…` for every scratch render; screenshots meant for humans go in `tests/screenshots/`.

## Data-loss traps (silent)

- A run that stamps **rewrites** `scenery/<name>/runtime_edits.vxw` (gitignored) unless `VF_OVERLAY_PATH` is set. Guards (`App::queueOverlaySave`): with `VF_NO_OVERLAY=1` and no `VF_OVERLAY_PATH`, saving is disabled while that file exists; an overlay that fails to load is moved to `*.unreadable-<ts>`; the first save that would shrink the loaded chunk set copies the file to `*.bak-shrink-<ts>`. Still use `VF_OVERLAY_PATH=/tmp/…` for scratch runs.
- The app rewrites `scenery/<name>/world.json` from its layer list; a test toggling layers must verify its restore (`git diff scenery/` is the check), and must never suppress a feature by deleting a manifest key.
- "Clear live edits" / `VF_TEST_CLEAR=1` moves the overlay to `runtime_edits.vxw.cleared-<ts>` (copy it back to restore).
- Every manifest/`.vxw` writer is temp+rename with a per-process unique temp name (`worldfile::tempPathFor`); never write those files in place.
- Never `rm` globs near files you don't own; never `pkill` processes you didn't start (use `pkill -f "build/[v]oxelforge"` bracket trick for your own). Other sessions share the tree: `git add` exact paths, never directories.

## Architecture (big picture)

- **Scenery = data, not code.** A world lives in `scenery/<name>/` (`world.json` manifest, `landscape.vxw`, object `.vxw` layers, `micro/`, `ai_edits.vxw`, `runtime_edits.vxw`). `vf::voxel::scenery::root()` resolves the active one (`--scenery`, `VF_SCENERY`, `scenery/last.json`, default `LakesideHamlet`). `assets/` is only the shared texture/model library. **Never hardcode material ids, layer names, positions, or textures in C++/GLSL** — read them from `world.json` / shared manifests.
- **Geometry pipeline** (`src/voxel/`): `LayeredWorld` merges enabled layers (first-wins) → `VoxelField` (load-time oracle: terrain columns + object SDFs) → SVO pools; `ChunkStore` is the runtime-explicit sparse store adopted lazily, patched by live edits (`LiveEditor` re-bakes only the edited AABB ± margin and returns per-chunk surfel runs for GPU patching), serialized by `OverlayWriter`. `surfelize.cpp` bakes surfels (shadow/AO baked on CPU); live-edit and full bake must share one normal rule.
- **Rendering** (`src/render/`, `shaders/`): `splat_pass` (paged per-chunk surfel buffer; depth prepass → opaque base → blended band), `svo_pass` (reference compute raymarch), TAA, post. Shared GLSL in `common_base/common_svo/common_splat/common_surfel.glsl`. Surfel layout is 80 B; `common_surfel.glsl` is the decoder contract for forward/cull/tile shaders. SVO handles are chunk-local.
- **Micro-voxel objects** are separate SVO worlds (own lattice, own overlay pass `micro_overlay.comp`, sphere-traced with an eighth-voxel SDF), with a derived rig/behaviours, a skinned mesh pass, and a translucent fur-card pass. They never enter the world's `VoxelField`/`ChunkStore`.
- **App** (`src/app/`): one dir per subsystem (`cli/ rhi/ world/ textures/ edit/ mesh/ ui/ frame/`); `app.hpp` is the root header (map in its comment) and `frame/run.cpp` is the only file that knows frame-loop order. UI is a single docked sidebar built from `ui/ui_types.hpp` + `ui/ui_primitives.*` — reuse those, not ad-hoc ImGui. AI panel is a thin client of a per-scenery OpenCode session; `vf_mcp` (stdio MCP server) is the authoring/inspection API (`write_object`, `add_*`, `render_shot`, …). Don't hand-edit `.vxw`.
- Invariants that bite: `WORLD/VOXEL/GRID_N/BRICK_N` are load-bearing across encoder/meta/shaders; brick sign test is `raw <= 0` = solid; `ChunkPool` root `-2` = solid terminal (don't test `root < 0` for empty); a `WorldLayer` field must also be emitted by `writeManifest`.

## Project tooling conventions

AGENTS.md mandates OpenCodeRAG tools (`search_semantic`, `get_file_skeleton`, `find_usages`, `recall_quirks`/`add_quirk`/`update_quirk`/`delete_quirk`) and `crosstalk_*` peer coordination for sessions in this repo. When those tools are available, recall quirks for the area before work and record non-obvious fixes after. `.opencode/wiki/` holds concept pages referenced throughout AGENTS.md.
