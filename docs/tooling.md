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
| `--probe X Y Z` | print field signed distance + material at a point and **exit before Vulkan init** — works without a GPU/window; reads the live layered world incl. ai_edits |
| `--width N` / `--height N` | resolution for headless modes (offscreen render target) |
| `--llm-url URL` / `--ollama-url URL` | chat backend override |
| `--llm-model M` / `--ollama-model M` | chat model override |

Interactive defaults: hero camera `-16,6.5,-14 → 6.5,0.8,11`, TAA on.

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
ctest --test-dir build           # unit_tests + visual_check
./build/vf_tests --test-case='*world*'   # single suite
./build/vf_slice ...             # ASCII cross-sections
./build/vf_mcp                   # MCP stdio server
./build/voxelforge               # the app
```

Build trees: `build` (primary), `build-dbg`, `build-asan`. No validation
layers are installed on the dev machine — correctness gates are selftest /
visual_check / unit tests plus `VF_TRACE`.
