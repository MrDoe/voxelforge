# Tooling & CLI reference

## `voxelforge` CLI

Parsed in `src/app/main.cpp` (`parseArgs`). Defaults: window 1600×900,
sun elevation 34° / azimuth 238°, animtime 0.

| flag | effect |
|---|---|
| `--selftest` | headless acceptance: render 30 frames, assert geometry coverage 3–97 % and blue sky probe; exit 0/1 |
| `--smoke N` | run N frames headless (default 240), report avg/min/max ms |
| `--shot FILE.ppm` | render one deterministic frame (frame 3, no TAA) and exit |
| `--cam X Y Z TX TY TZ` | camera position + look-at target (all 6 required) |
| `--sun ELEV AZIM` | sun direction in degrees (elevation, azimuth) |
| `--animtime S` | fix the water/grass animation clock for reproducible shots |
| `--mode splat\|svo` | render backend: Gaussian surfels (default) or the SVO reference raymarcher; `F` toggles it live |
| `--probe X Y Z` | print field signed distance + material at a point and **exit before Vulkan init** — works without a GPU/window; reads the live layered world incl. ai_edits |
| `--width N` / `--height N` | resolution for headless modes (offscreen render target) |
| `--llm-url URL` / `--ollama-url URL` | chat backend override |
| `--llm-model M` / `--ollama-model M` | chat model override |

Interactive defaults: reference camera `1.0,2.0,1.5 → 5.3,1.0,11.3` (house.jpeg view), TAA on.

### Environment variables

| var | effect |
|---|---|
| `VF_LLM_URL`, `VF_LLM_MODEL` | chat backend (override the flags) |
| `VF_PRESENT=immediate\|mailbox` | present mode. Default IMMEDIATE — MAILBOX deadlocks on NVIDIA+X11. |
| `VF_TRACE=1` | per-frame submit/acquire traces + `layered_world` SVO buffer hash logs (determinism checks) |
| `VF_HUD_SHOT=FRAMES:PATH` | after N *presented* frames, copy the composed swapchain frame (HUD included) to PATH and exit — interactive UI screenshots |
| `VF_GUI_TEST=LAYERNAME` | test hook: toggle that layer like its checkbox at frame 20 |
| `VF_TEST_SELECT=x,y,z` | deterministic anchor selection for highlight shots |
| `VF_TEST_HOVER=x,y,z` | deterministic hover highlight |
| `VF_TEST_BRUSH=x,y,z,carve\|add\|delete\|paint\|smooth` | activate the edit tool at a voxel and render only the hover preview (no edit). Brush size via `VF_EDIT_DIAM`/`VF_EDIT_DEPTH`; Smooth strength via `VF_SMOOTH_STRENGTH` |
| `VF_NO_OVERLAY=1` | ignore a saved `assets/runtime_edits.vxw` live-edit overlay at startup (the test scripts set it so interactive painting cannot pollute reference shots) |
| `VF_TEST_EDIT=x,y,z,carve\|add\|delete\|paint\|smooth` | apply one live store brush stamp after load (all modes patch the store, the surfels, the SVO pool and the edited height-texture columns) |
| `VF_TEST_STROKE=x,y,z,steps[,mode]` | simulate a drag stroke; add `VF_TEST_STROKE_SAVE=1` to persist the overlay |
| `VF_TEST_UNDO=1` | close the pending stroke and undo it (undo path check) |
| `VF_TEST_CLEAR=1` | drop every runtime edit + the overlay (the "Clear live edits" button) |
| `VF_OVERLAY_PATH=<file>` | read/write the live-edit store overlay there instead of `assets/runtime_edits.vxw` (tests keep their edits out of the repo assets) |
| `VF_EDIT_DIAM`, `VF_EDIT_DEPTH` | brush width/depth in metres, **snapped to whole voxels** (`App::quantiseBrush`), so `VF_EDIT_DIAM=0.1` is exactly 1 voxel = the per-voxel mode, where Add/Carve stamp a single cell (`EditableWorld::makeSingleVoxel`) instead of a volume |
| `VF_SMOOTH_STRENGTH`, `VF_EDIT_STRENGTH` | terrain Smooth relaxation strength, clamped to 0..1 (the latter is an alias) |
| `VF_LIVE_NOSPLAT=1`, `VF_LIVE_NOSVO=1` | skip one backend when patching a live edit |
| `VF_SPLAT_NOWATER=1` | skip the splat water-plane draw (A/B for the tests: a dug pit stays dry-looking) |
| `VF_SPLAT_DEBUG=15` | splat debug view: magenta = surfel centre inside the edit-brush volume |
| `VF_IMGUI_DEBUG=1` | dump ImGui draw-data stats at frame 5 |

Build-time injected paths: `VOXELFORGE_SHADER_DIR` (`build/shaders`),
`VOXELFORGE_ASSET_DIR` (`assets/`).

### PPM output

`--shot` writes binary `P6` PPM of the offscreen render (no HUD).
Inspect headlessly with:

```sh
python3 .opencode/skills/voxel-object/scripts/ascii_view.py out.ppm 96 40
```

## `vf_mcp`

stdio MCP server — see [AI editing](ai-editing.md#vf_mcp-protocol) for the
protocol and tool table. Quick smoke test:

```sh
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | ./build/vf_mcp
```

## STL / OBJ mesh import

The in-app **Mesh** section of the editor sidebar (rail `IM`, `Ctrl+5`) lists
`.stl`
and `.obj` files under `assets/models/`, accepts a path typed manually, and
writes a named `.vxw` layer through the same parser/voxelizer as the CLI and
MCP tool. It exposes fit-to-longest-side or explicit scale, source-axis and
winding fixes, material selection, solid-vs-shell mode, and a lattice anchor.
A Ctrl+LMB pick can supply the anchor; for an existing layer, **Use layer
source pivot** reads the untransformed bottom-center. Replacing an existing
layer preserves its `world.json` `pos`, `rot`, `rotX`, and `rotZ` fields, so
re-importing geometry does not change its authored orientation.

The explicit offline converter remains useful for reproducible asset builds:

```sh
ninja -C build vf_mesh2vox
./build/vf_mesh2vox assets/models/Forrest_Hunting_Cabin.stl \
  --out hamlet_cabin --fit 5 --cell 428,507,676 --mat 6
```

`--dry-run` prints triangle/AABB/grid statistics without writing. `--scale`
is for CAD units (millimetres normally use `0.001`); `--fit M` scales the
longest AABB side to `M` metres. The importer defaults to a solid voxel fill;
use `--shell` only for a known-watertight mesh because a thin raster shell can
render hollow after VoxelField loading.

For a deterministic headless exercise of the GUI method, set:

```sh
VF_TEST_MESH_IMPORT='models/Forrest_Hunting_Cabin.stl,hamlet_cabin,428,507,676,5,6,0,1' \
  ./build/voxelforge --smoke 1
```

The fields are `file,name,x,y,z,fit[,material,meshYaw,solid]`; it calls the
same `importMeshFromGui()` path and then rebuilds the layered world.

## `vf_slice`

ASCII cross-sections of the records-derived field (terrain + objects +
AI edits). The fast authoring feedback loop — no GPU, no window.

```
vf_slice --axis x|y|z --center X Y Z --span S [--res N] [--band B]
```

- `--axis` is the plane normal through `--center`.
- Columns ascend along u, rows descend along v.
- Solid cells print their material glyph; empty cells within `--band`
  (default 0.15 m) of a surface print `+`; deep air prints space.
- Defaults: span 12 m, res 61 (range 8–240).

Requires baked assets (`world.json` + layers) like every field consumer.

## `heightmap_gen` (baker)

Run via CMake — never wired into default builds/tests/start.sh:

```sh
ninja -C build world      # = target vf_heightmap
# direct: ./build/heightmap_gen assets/heightmap.png assets/world.vxw
```

The second argv supplies the layer output directory (filename part is ignored;
it's a legacy path). Outputs:

- `heightmap.png` — 2048², 16-bit, stored-deflate PNG written by a hand-rolled
  encoder (`rowBytes = 1 + w*2`);
- terrain shell records into `landscape.vxw` (per column: cells within ±3
  lattice rows of the surface, ±0.20 m band);
- authored object layers swept from the analytic shapes in
  `src/voxel/common.hpp` (`houseAt`, `treesAt`, …);
- `world.json` with only `landscape` (+ non-empty `ai_edits`) enabled;
- **preserves an existing `ai_edits.vxw`** — the baker only consumes it,
  never rewrites it, so chat/MCP edits survive re-bakes.

Terrain height function: `tools/heightmap_gen.cpp :: terrainHeightAt()` —
ridged-fbm hills rising from a meandering river channel (`riverZ/riverW`),
flattened pad under the house. Material classification uses lattice slope so
baked materials match what the shader renders.

## `start.sh`

One-shot launcher: build-if-needed → llama-server orchestration → app.

Environment knobs:

| var | default | meaning |
|---|---|---|
| `LLAMA_BIN` | `/opt/llama.cpp/build/bin/llama-server` (fallback `/usr/local/bin`) | server binary |
| `LLAMA_HOST` / `LLAMA_PORT` | `127.0.0.1` / `8080` | bind address; probes 8080/8088 as fallbacks |
| `LLAMA_CTX` | `8192` | context size (lower it if VRAM OOM) |
| `LLAMA_NGL` | `99` | layers offloaded to GPU |
| `LLAMA_MODELS_MAX` | `1` | resident models (VRAM guard) |
| `LLAMA_SPEC` | `0` | `1` enables speculative drafting |
| `MODELS_INI` | `/home/christoph/models/models.ini` | gemma preset file |
| `VF_LLM_MODEL` / `LLM_MODEL` | `gemma-4-e4b` | requested model id |

Behavior highlights:

- **Never starts a second llama-server**: probes candidate ports, then checks
  the process table (`pgrep`), extracts ports from `/proc` cmdlines, and
  serializes concurrent launches with an flock. If a process exists but no
  endpoint responds, it aborts with diagnostics instead of double-spawning.
- Model selection avoids OOM swaps: prefers an already-loaded model over the
  requested one when VRAM is occupied.
- Refuses to run without baked assets.
- Exports `VF_LLM_URL=http://$LLAMA_HOST:$LLAMA_PORT/v1` + chosen model and
  execs `voxelforge "$@"`.

To skip llama entirely, launch the binary yourself against Ollama or any
OpenAI endpoint (see [getting started](getting-started.md#ai-editing-quick-start)).

## Build targets cheat sheet

```sh
cmake -S . -B build -G Ninja     # once
ninja -C build                   # all binaries + shaders (vf_shaders)
ninja -C build world             # bake assets (explicit!)
ninja -C build test-surfel       # focused surfel group; choose per change
ninja -C build test-live-edit    # focused live-edit group
ninja -C build test-visual       # focused visual group
./build/vf_tests --test-case='*world*'   # single doctest filter
./build/vf_slice ...             # ASCII cross-sections
./build/vf_mesh2vox ...          # STL/OBJ -> named .vxw layer
./build/vf_mcp                   # MCP stdio server
./build/voxelforge               # the app
```

Build trees: `build` (primary), `build-dbg`, `build-asan`. No validation
layers are installed on the dev machine — correctness gates are selftest /
visual_check / unit tests plus `VF_TRACE`.
