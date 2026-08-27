# Getting started

## Requirements

- Linux with **Vulkan 1.3** (dedicated GPU strongly recommended)
- CMake ≥ 3.24, Ninja, a C++20 compiler
- `glslangValidator` (shader compilation)
- Python 3 (test suite)
- Optional: an [Ollama](https://ollama.com) instance or any OpenAI-compatible
  chat server (`llama.cpp` `llama-server`, vLLM, …) for AI editing

All libraries (spdlog, GLFW, glm, VMA, doctest, ImGui) are fetched automatically
via CMake FetchContent — nothing to install manually.

## Build & first run

```sh
cmake -S . -B build -G Ninja
ninja -C build          # binaries + shaders (build/shaders/*.spv)
ninja -C build world    # bake assets — REQUIRED ONCE after a clean checkout
./build/voxelforge
```

The asset bake is an **explicit extra step**, never wired into the default
build or tests. `ninja -C build world` runs the offline baker
(`tools/heightmap_gen.cpp`) which writes:

| asset | role |
|---|---|
| `assets/heightmap.png` | terrain source: 2048², 16-bit, 5 cm/texel |
| `assets/landscape.vxw` | terrain shell records (per lattice column) |
| `assets/house\|tree1..6\|rock1..3\|bushes\|alpaca\|fence1.vxw` | authored objects |
| `assets/world.json` | layer manifest (order = dedupe priority) |
| `assets/ai_edits.vxw` | live layer for chat/MCP edits (**preserved across re-bakes**) |

After a fresh bake only `landscape` (+ `ai_edits` once it has content) are
enabled; every other baked layer is opt-in.

**The app and tests refuse to run without `assets/world.json`** — the error is
`run 'ninja -C build world' first`.

### Alternative: one-shot launcher

```sh
./start.sh
```

Builds if needed, starts/reuses a `llama.cpp` server for the chat backend,
picks an available model, then launches the app with `VF_LLM_URL/MODEL` set.
See [Tooling → start.sh](tooling.md#start-sh) for its environment knobs.
It also refuses to run without baked assets.

## Controls

| input | action |
|---|---|
| `WASD` / `Q` `E` | move / down / up |
| `RMB` + mouse | look |
| wheel | movement speed (`Shift` boost / `Ctrl` slow) |
| `Ctrl+LMB` | pick a voxel → becomes the bottom-center anchor for AI builds |
| `ESC` | quit |

Default camera spawns at `-16, 6.5, -14` looking at the riverside cabin;
default sun is elevation 34°, azimuth 238° (golden hour).

## The "World layers" panel

The app starts as a bare valley. The HUD panel lists every `.vxw` file in
`assets/`:

- **Checkbox** — enable/disable the layer *at its baked location*. Toggling
  persists to `world.json` and hot-reloads the running world within ~0.5 s.
- **Import button** — place a *copy* of that object so its bottom-center lands
  on your `Ctrl+LMB` selection; the copy is appended to `ai_edits.vxw`.
  Layer files store absolute lattice coordinates, so enabling shows the object
  where it was baked while import stamps a moved copy.
- **Rescan assets folder** — pick up files dropped into `assets/` while running
  (they also appear automatically on the next reload).
- The landscape checkbox is always on and disabled — no terrain, no world.

## AI editing quick start

In-app chat (any Ollama / OpenAI-compatible endpoint):

```sh
VF_LLM_URL=http://127.0.0.1:11434 VF_LLM_MODEL=qwen2.5 ./build/voxelforge
VF_LLM_URL=http://localhost:8080/v1 ./build/voxelforge   # llama.cpp / vLLM …
```

Pick an anchor with `Ctrl+LMB`, then ask for *"a 3×3 wood crate"* or *"a small
rock"*. Tool calls execute against the world and reload instantly. Full details:
[AI editing](ai-editing.md).

External MCP clients register the stdio server once:

```json
{ "command": "./build/vf_mcp", "args": [] }
```

Edits land in `assets/ai_edits.vxw`; a running app picks them up within ~0.5 s.

## Headless modes

All of these exit cleanly without a window manager (details in
[Tooling](tooling.md#voxelforge-cli)):

```sh
./build/voxelforge --selftest --width 640 --height 360   # acceptance check
./build/voxelforge --smoke 500                           # N frames + timings
./build/voxelforge --shot out.ppm --cam X Y Z TX TY TZ   # single frame
./build/voxelforge --probe X Y Z                         # field query, no GPU init
```

## Troubleshooting

| symptom | cause & fix |
|---|---|
| `assets/world.json missing - run 'ninja -C build world'` | bake assets once (see above). Deleting anything under `assets/` re-requires this. |
| App exits/crashes on present, NVIDIA + X11 | default present mode is IMMEDIATE because MAILBOX deadlocks on this stack. Override `VF_PRESENT=immediate\|mailbox`. |
| UI panels missing / off-screen | `imgui.ini` persists window layouts; the AI Chat window self-heals each frame, others don't. Delete `imgui.ini` to reset all panels. |
| Whole UI renders nothing | ImGui uses `UseDynamicRendering`; the app must wrap `ImGui_ImplVulkan_RenderDrawData` in its own `vkCmdBeginRendering/EndRendering`. If you touched the frame loop, check `src/app/main.cpp`. |
| Chat says backend unreachable | probe `curl $VF_LLM_URL/models` (OpenAI shape) or `curl http://127.0.0.1:11434/api/tags` (Ollama). `start.sh` diagnostics help isolate llama-server issues. |
| Edits not appearing | edits land on disk; the app polls every ~0.5 s. Check that `ai_edits` shows enabled in the layers panel (`EditableWorld::enableInManifest` flips it on first edit). |
| Shader changed but nothing happens | shaders compile to `build/shaders/*.spv` via ninja — rebuild instead of running a stale binary. |

## Next steps

- [Architecture](architecture.md) for how it all works
- [Testing](testing.md) before your first change
