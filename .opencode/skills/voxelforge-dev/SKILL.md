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
| CPU truth — world constants, palette, SDF scene | `src/voxel/common.hpp` | `WORLD=102.4`, `VOXEL=0.1`, `CHUNK_N=64`, `GRID_N=16`, `BRICK_N=8`, `WATER_LEVEL=-0.9`, `kPalette[9]` |
| Scene composition | `scene(p)` bottom of `common.hpp` | min-combine `heightmap` + `houseAt`/`treesAt`/`rocksAt`/`bushesAt` |
| World synthesis | `src/voxel/layered_world.{hpp,cpp}` | Merges `assets/world.json` layers (priority dedupe), flood-fills solids, builds `GpuWorld` |
| Fallback/world I/O | `src/voxel/world.{hpp,cpp}`, `worldfile.{hpp,cpp}`, `heightmap.{hpp,cpp}`, `volume.cpp` | `kMaxDepth=3`, `classifyMargin=0.75*cellSize`, VXW v1, dense 256³ @0.4 m |
| Rendering | `src/rhi/*` (Vulkan 1.3+VMA), `src/render/*` (SvoPass/RaymarchPass/SplatPass/TaaPass), `src/app/main.cpp` | `RaymarchPush` 128 B, see §5 |
| Platform/window | `src/platform/window.cpp` (GLFW) |  |
| AI/MCP | `src/ai/*`, `src/app/chat_ui.cpp`, `tools/mcp` | `vf_mcp` stdio, `ai_edits.vxw` highest priority |
| Shaders | `shaders/raymarch.comp`, `svo_raymarch.comp`, `splat.vert/.frag`, `taa_resolve.comp` | Keep raymarch/svo in parity |
| Tools | `tools/heightmap_gen.cpp` (asset gen), `tools/scene_slice.cpp` (`vf_slice`) |  |
| Tests | `tests/*.cpp` (doctest), `tests/visual_check.py` |  |

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
- `assets/ai_edits.vxw` is never regenerated — highest priority live layer for MCP/chat.
- Fallback `World::build()` only if `world.json` missing (slow). After clean checkout or `rm assets/*.png assets/*.vxw assets/world.json` you **must** run `ninja -C build world` or binary exits `heightmap.png missing`.
- Don't hand-edit derived `.vxw`; edit terrain in `tools/heightmap_gen.cpp:terrainHeightAt()`, scene in `src/voxel/common.hpp:scene()` then regenerate — or edit object `.vxw` layers directly for data-only changes without recompile.

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
   ./build/voxelforge --compare --width 640 --height 360   # SVO vs dense, 30 warmup frames
   ./build/voxelforge --shot /tmp/vf.ppm --cam X Y Z TX TY TZ --width 640 --height 360
   python3 .opencode/skills/voxel-object/scripts/ascii_view.py /tmp/vf.ppm 96 40
   ```
5. Full gates before declaring done (see §7).

## 6. Shaders & parity — easy to break

- `RaymarchPush` is exactly **128 B** (`alignas(16)`): `camPos/Right/Up/Fwd`, `a=(tanHalfFov,aspect,extentX,extentY)`, `b=(worldSize,voxelSize/MAX_ENCODED_DIST,gridN,frameIdx)`, `sunDir` (toward sun), `misc=(splatScale,animTime_s,shadingMode,aoEnabled)`. `animTime` is `misc.y`; `frameIdx` legacy in `b.w` — don't reuse `a.w` (was frozen-ripples bug). `kSunDir` is mutable global set from push in both shaders.
- Brick packing: `word0=r|g<<8|b<<16|sdfByte<<24` where `sdfByte=int8(sdf/VOXEL)`, decode `raw*VOXEL` not `raw/127*VOXEL`; `word1=a|refl<<8|rough<<16|mat<<24`.
- SVO `map()` empty-cell fallback is `max(-sdBox(p,cmin,cmax), VOXEL*0.5)` + 6-step bisection — `6.0` is load-bearing (tunneling/hollow voxels if changed).
- **Keep `shaders/svo_raymarch.comp` and `shaders/raymarch.comp` in parity**: water plane `y=-0.9`, fog `0.0012`, ACES, `softShadow`/`calcNormal`/`heightAt`. Both use fixed `WATER_LEVEL` bidirectionally (`waterHit()` visible from above+below), `gUnderwater` volumetric tint, and `shadeTerrain` bed absorption skipped when submerged. Mirror any change in both files or `--compare` fails.
- `splat.frag` is `ONE/ONE_MINUS_SRC_ALPHA` premultiplied — requires `SplatPass::updateSorting()` `stable_sort` back-to-front (`dot(camFwd)`) with `0.005` tie-breaker before each `record` (`src/render/splat_pass.cpp:85`).
- Dense volume mip chain is CPU box-filtered 9-level 3-D with `int8` min-abs SDF + `textureLod(dist*0.015)` at far distance (`src/rhi/resources.cpp`, `shaders/raymarch.comp:47`) — don't remove (distant shimmer).
- Shader compile: `glslangValidator -V --target-env vulkan1.3 -o build/shaders/<name>.spv shaders/<name>` — Ninja target `vf_shaders` does this.

## 7. Verification gates — must pass before done

Run in order; all must be green for any shader/scene change:

```bash
ninja -C build
ctest --test-dir build                         # unit_tests (~32 s) + visual_check (~13 s)
# or single suite:
./build/vf_tests --test-case="*worldfile*"
ctest -R unit_tests -V

python3 tests/visual_check.py build/voxelforge # 3 canonical shots hero/house/water @480×270: coverage 3-97%, black-in-silhouette <5%, blue sky probe

./build/voxelforge --selftest --width 640 --height 360   # asserts sky probe + green terrain
./build/voxelforge --compare --width 640 --height 360    # tight diff: meanDiff <14/255 + |covSvo-covDense|<0.08 (src/app/main.cpp:879)

# debug dumps:
VF_DUMP_PPM=1 VF_TRACE=1 ./build/voxelforge --shot /tmp/vf.ppm --cam -16 6.5 -14 6.5 0.8 11
python3 .opencode/skills/voxel-object/scripts/ascii_view.py /tmp/vf.ppm 96 40
```

- `vf_tests` depends on `vf_heightmap` — deleting assets then running `ctest` without `ninja world` fails at build step, not runtime.
- No validation layers on dev machine; rely on `selftest`/`compare`/`visual_check` + `VF_TRACE`.
- Present: default `IMMEDIATE` (MAILBOX deadlocks on NVIDIA+X11); per-swapchain-image acquire semaphores (`src/rhi/swapchain.cpp:53`). Override `VF_PRESENT=immediate|mailbox`.

## 8. Gotchas — load-bearing, don't touch casually

- `WORLD/VOXEL/GRID_N/BRICK_N` change requires synchronized updates: heightmap encode, `worldfile` meta check (`src/app/main.cpp:424`), shader `BRICK_WORDS` — otherwise load + `compare` break.
- `heightmap_gen` PNG writer is hand-rolled stored-deflate; `rowBytes = 1+w*2` (not `(1+w)*2`).
- `nearObject()` in `tools/heightmap_gen.cpp` decides y-sampling band expansion for tall objects outside existing radii — extend for new tall geometry or splat records will be clipped (raymarch/SVO unaffected).
- `App::buildSplatData` (`src/app/main.cpp`) hardcodes `houseAt/treesAt/rocksAt/bushesAt` for sub-splat normals — add new object functions there if splat mode should shade them.
- World bounds ±51.2 m; water `-0.9`. Determinism only: use `hash2`, never wall-clock/RNG state. `scene()` is hot (~10 M calls/regen + AO taps) — add cheap reject (distance²/vertical cull) for scatter objects like `treesAt` does.
- Interactive keys: `F` voxel↔splat, `B` shading, `O` AO, `+/-` splat scale (also `RIGHT_BRACKET`/`APOSTROPHE`/`SLASH` + keypad, hold slides `exp2(1.6*dt)`).
- Docs: `ImplementationPlan.md` §5 (PBR/WBOIT/HG backlog) and `implementation_plan.md` (minimal raytrace per splat) are the splat roadmap — keep splats derived from `scene()` SDF (`|d|<0.22 m` band + 8-neighbour `spacing*0.25`, radius `spacing*1.25`, cap 512 px).

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
