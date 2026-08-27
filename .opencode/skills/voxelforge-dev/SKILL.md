---
name: voxelforge-dev
description: Edit, build, and verify C++/shader code in voxelforge — build system, architecture, shader parity, asset pipeline, and mandatory test gates. Use when touching any source file, shader, CMake, or tool, or when asked to fix, refactor, implement, or verify a feature.
---

# VoxelForge code editing

Use this skill for any code change in this repo: C++ (`src/**`, `tools/**`, `tests/**`), GLSL (`shaders/**`), CMake (`CMakeLists.txt`), or Python (`tests/*.py`, `tools/*.py`). For voxel *content* (houses/trees/rocks as SDF/stamps in `common.hpp`) prefer `voxel-object`.

## 1. Navigate first — mandatory

ALWAYS before reading or editing:

1. `search_semantic(query)` — find relevant code by meaning
2. `get_file_skeleton(filePath)` — orient before full read
3. `find_usages(symbolName)` — before modifying any symbol
4. `recall_quirks(query)` — check known pitfalls
5. `read` only the needed line ranges, then `edit`

Never `grep`/`glob` when semantic search applies. Never read full files without skeleton. Never edit a symbol without checking call sites.

If no RAG results, run `opencode-rag index`.

Persist discoveries: `add_quirk(content)` for build failures, workarounds, env constraints. Fix stale entries with `update_quirk`/`delete_quirk` — never duplicate.

## 2. Where things live

| Area | Path | Notes |
|---|---|---|
| Constants, palette, SDF primitives + **baker-side** shapes | `src/voxel/common.hpp` | `WORLD=102.4`, `VOXEL=0.1`, `CHUNK_N=64`, `GRID_N=16`, `BRICK_N=8`, `WATER_LEVEL=-0.9`, `kPalette[9]`. NOT linked into the renderer — consumed by the bake and tests only |
| Bake (offline) | `tools/heightmap_gen.cpp` | sweeps terrain + `houseAt`/`treesAt`/… into `.vxw` layers; run via `ninja -C build world`. There is no runtime `scene()` |
| World synthesis | `src/voxel/layered_world.{hpp,cpp}` | merges `assets/world.json` layers (first-wins dedupe), builds `VoxelField`, synthesizes `GpuWorld` |
| Geometry oracle / format | `src/voxel/voxel_field.{hpp,cpp}`, `worldfile.{hpp,cpp}`, `picking.{hpp,cpp}`, `heightmap.{hpp,cpp}` | records-only geometry oracle; EDT flood-fill objects; VXW v1 |
| Rendering | `src/rhi/*` (Vulkan 1.3+VMA), `src/render/*` (SvoPass/TaaPass), `src/app/main.cpp` | `RaymarchPush` 128 B in `svo_pass.hpp`, see §5 |
| Platform/window | `src/platform/window.cpp` (GLFW) |  |
| AI/MCP | `src/ai/*`, `src/app/chat_ui.cpp` | `vf_mcp` (`src/ai/mcp_server.cpp`) stdio, `ai_edits.vxw` highest priority |
| Shaders | `shaders/svo_raymarch.comp`, `taa_resolve.comp` | single render path |
| Tools | `tools/heightmap_gen.cpp` (asset bake), `tools/scene_slice.cpp` (`vf_slice`) |  |
| Tests | `tests/*.cpp` (doctest), `tests/visual_check.py` |  |
| Dev docs | `docs/` (start `docs/index.md`), `AGENTS.md` | architecture, world format, GPU contract, gates |

## 3. Build

- Reqs: Vulkan 1.3, `glslangValidator`, CMake ≥3.24, Ninja, Python3.
- Deps via `FetchContent` — no manual install (`spdlog`, `glfw`, `glm`, `VMA`, `doctest`, `imgui`).
- Build dirs: `build` (primary), `build-asan`, `build-dbg`. Always build via Ninja:
  ```bash
  ninja -C build                 # binary + shaders (vf_shaders, --target-env vulkan1.3)
  ninja -C build vf_slice        # slice tool (scene truth, no GPU)
  ninja -C build vf_tests        # tests
  ninja -C build world           # alias vf_heightmap — regenerates assets
  ```
- Shader dir injected as `VOXELFORGE_SHADER_DIR=build/shaders`, assets as `VOXELFORGE_ASSET_DIR=assets`.
- Incremental rebuild is fast; `glslangValidator` compiles shaders to `build/shaders/*.spv` (target `vf_shaders`).

## 4. Assets — gitignored, regenerated

- `assets/heightmap.png`, `assets/*.vxw`, `assets/world.json` are gitignored. No merged `world.vxw`.
- Generated together: `heightmap_gen assets/heightmap.png` → 2048² 16-bit PNG (5 cm/texel, `h=-8+u/65535*32`) + layer family `landscape.vxw`, `house.vxw`, `tree1..6.vxw`, `rock1..3.vxw`, `bushes.vxw`, `alpaca.vxw`, `fence1.vxw` + `world.json` (order = dedupe priority, first wins).
- Runtime: `LayeredWorld` merges enabled layers, flood-fills object shells to solid, BFS air distance, water below `WATER_LEVEL`. Polls mtimes each frame, `reloadIfChanged()` rebuilds ~0.5 s.
- `assets/ai_edits.vxw` is never regenerated — highest priority live layer for MCP/chat; the baker preserves it across regens.
- No procedural fallback: without `assets/world.json` the app/tests abort with
  `run 'ninja -C build world' first`. After a clean checkout or deleting assets you **must** run `ninja -C build world`.
- Don't hand-edit derived `.vxw`; edit terrain in `tools/heightmap_gen.cpp:terrainHeightAt()`, authored shapes in `src/voxel/common.hpp` (baker-side analytics) then regenerate — or drop/edit object `.vxw` layers directly for data-only changes without recompile.
- Full details: `docs/getting-started.md`, `docs/world-format.md`.

## 5. Edit workflow — one change, verify, next

1. Implement **one** focused change (one file/concern). Keep diffs small.
2. Build: `ninja -C build` — fix compile errors before testing.
3. Fast CPU checks (no GPU/window):
   ```bash
   ninja -C build vf_slice && ./build/vf_slice --axis z --center X Y Z --span 10
   ./build/voxelforge --probe X Y Z   # signed distance + mat, exits before Vulkan init
   ```
4. Headless GPU checks (needs display/Xvfb):
   ```bash
   ./build/voxelforge --selftest --width 640 --height 360
   ./build/voxelforge --shot /tmp/vf.ppm --cam X Y Z TX TY TZ --width 640 --height 360
   python3 .opencode/skills/voxel-object/scripts/ascii_view.py /tmp/vf.ppm 96 40
   ```
5. Full gates before declaring done (see §7).

## 6. Shaders & parity — easy to break

- Push block is exactly **128 B** (`alignas(16)`), lives in `svo_pass.hpp`: `camPos/Right/Up/Fwd`, `a=(tanHalfFov,aspect,extentX,extentY)`, `b=(worldSize,voxelSize,gridN,frameIdx%1024)`, `sunDir` (toward sun), **`misc.y=animTime_s`** — the struct comment wrongly says `misc.x`; the shader and main.cpp both use `misc.y`. Don't reuse `a.w`. See `docs/rendering.md`.
- Brick packing: `word0=r|g<<8|b<<16|sdfByte<<24` where `sdfByte=int8(sdf/VOXEL)`, decode `raw*VOXEL` not `raw/127*VOXEL`; `word1=a|refl<<8|rough<<16|(mat|objFlag<<7)<<24` — bit 7 of the material byte = object-surface flag.
- SVO `map()` empty-cell fallback is `max(-sdBox(p,cmin,cmax), VOXEL*0.5)` + 6-step bisection — `6.0` is load-bearing (tunneling/hollow voxels if changed).
- All geometry is data: terrain from `uHeight` (rg32f: top Y + material), objects from bricks + `uObjVol` (r8_snorm object SDF for shadows). No analytic scene constants exist in GLSL — don't add any.
- Shader compile: `glslangValidator -V --target-env vulkan1.3 -o build/shaders/<name>.spv shaders/<name>` — Ninja target `vf_shaders` does this.

## 7. Verification gates — must pass before done

Run in order; all must be green for any shader/scene change:

```bash
ninja -C build
ctest --test-dir build                         # unit_tests + visual_check
# or single suite:
./build/vf_tests --test-case="*worldfile*"
ctest -R unit_tests -V

python3 tests/visual_check.py build/voxelforge # 3 canonical shots hero/house/water @480×270: coverage 3-97%, black-in-silhouette <5%, blue sky probe

./build/voxelforge --selftest --width 640 --height 360   # asserts sky probe + coverage bounds

# debug traces:
VF_TRACE=1 ./build/voxelforge --shot /tmp/vf.ppm --cam -16 6.5 -14 6.5 0.8 11
python3 .opencode/skills/voxel-object/scripts/ascii_view.py /tmp/vf.ppm 96 40
```

- `vf_tests` does NOT depend on `vf_heightmap`; without baked assets field-consuming tests abort at runtime with "run 'ninja -C build world' first" instead of triggering a bake.
- No validation layers on dev machine; rely on `selftest`/`visual_check` + unit tests + `VF_TRACE`.
- Present: default `IMMEDIATE` (MAILBOX deadlocks on NVIDIA+X11); per-swapchain-image acquire semaphores. Override `VF_PRESENT=immediate|mailbox`.
- More gates & debug workflows: `docs/testing.md`.

## 8. Gotchas — load-bearing, don't touch casually

- `WORLD/VOXEL/GRID_N/BRICK_N` change requires synchronized updates: heightmap encode, `worldfile` meta checks (`worldfile.cpp`, `editable_world.cpp`, `layered_world.cpp`), shader `BRICK_WORDS` — otherwise load breaks.
- `heightmap_gen` PNG writer is hand-rolled stored-deflate; `rowBytes = 1+w*2` (not `(1+w)*2`).
- `nearObject()` in `tools/heightmap_gen.cpp` decides y-sampling band expansion for tall objects outside existing radii — extend for new tall geometry or their baked records get clipped.
- New authored objects need no renderer registration: bake them into a layer and the VoxelField/SVO/shadows pick them up.
- World bounds ±51.2 m; water `-0.9`. Determinism only: use `hash2`, never wall-clock/RNG state. Baker sweeps are hot (~10 M SDF calls/regen) — add cheap reject (distance²/vertical cull) for scatter objects like `treesAt` does.
- Interactive keys: WASD/QE move, RMB look, wheel speed, Ctrl+LMB pick anchor, ESC quit.
- Docs: `README.md`, `AGENTS.md`, `docs/` (start at `docs/index.md`; history in `docs/history/rework.md`).

## 9. Quick reference

```bash
ninja -C build && ctest --test-dir build
ninja -C build world            # regen assets after common.hpp / heightmap_gen changes
./build/voxelforge --probe X Y Z
./build/vf_slice --axis y --center X Y Z --span 8
./build/voxelforge --shot /tmp/vf.ppm --cam X Y Z TX TY TZ --width 640 --height 360
python3 .opencode/skills/voxel-object/scripts/ascii_view.py /tmp/vf.ppm 96 40
VF_LLM_URL=http://host:8080/v1 VF_LLM_MODEL=qwen2.5 ./build/voxelforge   # AI chat
./build/vf_mcp                  # MCP stdio
```

When in doubt, search first, change little, build, and let the gates decide.
