# Tooling & CLI reference

## `voxelforge` CLI

Parsed in `src/app/cli/args.cpp` (`parseArgs`). Defaults: window 1600×900,
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

Interactive defaults: reference camera `1.0,2.0,1.5 → 5.3,1.0,11.3` (house.jpeg view), TAA off (headless `--shot` never ran it; `N` re-enables).

### Environment variables

| var | effect |
|---|---|
| `VF_LLM_URL`, `VF_LLM_MODEL` | chat backend (override the flags) |
| `VF_PRESENT=immediate\|mailbox` | present mode. Default IMMEDIATE — MAILBOX deadlocks on NVIDIA+X11. |
| `VF_TRACE=1` | per-frame submit/acquire traces + `layered_world` SVO buffer hash logs (determinism checks) |
| `VF_HUD_SHOT=FRAMES:PATH` | after N *presented* frames, copy the composed swapchain frame (HUD included) to PATH and exit — interactive UI screenshots |
| `VF_TEST_PANEL=edit\|world\|render\|textures\|mesh\|ai` | open the sidebar on that section at startup. The sidebar is drawn by the interactive path only, so this is the only way to reach a non-default section from a script — combine it with `VF_HUD_SHOT` to capture a panel. |
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

## Animation studio (`--anim`)

`./animation_editor.sh` (or `--anim --anim-layer micro/alpaca.vxw`) opens the
studio in the sidebar's **Animate** section (`Ctrl+7`). It targets ONE micro
layer, resolved once by name — the panel, the viewport overlay, the clip clock
and the camera all go through that resolved index, because the old
"index 0" binding drove the VASE while the header said `alpaca`.

What it does:

- **Transport.** Play/pause/stop/step, a Time slider and Speed, over the
  target's clip set. Clips are `.anim` JSON files auto-discovered from the
  layer's directory (`assets/micro/walk.anim`, `graze.anim`) whose `layer`
  field matches. `--anim-time <s>` PINS the clock, which is what makes a
  `--shot` of the studio reproducible.
- **Authoring.** *New clip*, *Add body key*, *Add bone key*, *Save clip*.
  `Add bone key` writes a key at the playhead with zero rotation; the pose
  editor below is how a key gets a value.
- **Posing the selected bone.** Select a bone in the **Skeleton** list; three
  rings appear at its joint in the viewport, and **each ring is the PROJECTED
  CIRCLE of that bone's own axis** (local X / Y / Z) rather than a fixed
  screen shape — so a ring always lies in the plane it rotates about, at any
  bone orientation. Dragging a ring intersects the pointer with the ring's
  plane and tracks the pointer's ANGLE around the joint; the delta is composed
  onto the bone's local rotation and **stored as a keyframe at the playhead**
  (the value `MicroClip::sample` poses from, so the pose on screen IS the pose
  stored).
  - The grabbed ring's plane is frozen at the drag's start, so the drag cannot
    jump as the bone turns under the pointer.
  - The first edit at a time with no key SEEDS the new key from the pose
    already authored there, so touching a ring never snaps the bone to rest.
  - Keys are quantised to the clip's frame grid (`1/fps`), so one drag updates
    one key instead of writing hundreds.
  - A numeric **pose readout** is INTERPOLATED, not key-only: the playhead is
    almost never exactly on a key, so it shows the blend the rig is actually
    using, and editing it auto-keys at the playhead. An **Ease out** control
    (Step / Linear / Smooth) shapes the segment that STARTS at that key.
  - No clip exists yet? The first drag creates one (`studio`) and selects it —
    a key needs somewhere to live, and "make a clip first" is a step that
    exists only because of the file format.

**One interpolation rule.** `sampleTrackEuler` (micro_clip.cpp) is the only
implementation of "the bone pose at time t": `MicroClip::sample` poses the rig
with it and the studio's readout displays it, so what the editor shows at the
playhead cannot disagree with the pose on screen. It clamps at both ends
(a lone key holds for the whole clip, never extrapolates) and applies the
starting key's easing across each span.
- **Influence radius and LENGTH.** A per-bone radius slider (0–0.15 m) for the
  capsule test that decides bone OWNERSHIP, and a `length` drag that moves the
  bone's TIP along its own direction while the joint stays put (`a` is the
  pivot a local rotation turns about, so moving it is a different edit).
  Both re-upload the segments (`studioUploadRig`) — they are otherwise static
  after load, and an edit that skipped it would move the drawn skeleton while
  the body kept the old capsule. *Save rig (override)* writes
  `assets/micro/<name>.rig.json` stamped with the layer's size+mtime; *Reload
  rig* re-derives.
  At REST the skinning is the identity, so a length change is only visible once
  the bone is POSED (measured: a leg shortened 0.50 → 0.12 m moves 3,004 px
  when rotated 40°, but only 674 px at rest).

Bones are a **chain**: a bone's world transform is its parent's composed with
its own, so a pose is per-bone LOCAL rotations plus the parent chain — never
absolute transforms. The studio therefore authors `rotDeg` keys, which
`animateBones` composes, and nothing in the UI writes a world matrix.

Headless: `VF_TEST_BONE_DRAG="<bone>:<yaw|pitch|roll>:dx,dy"` drives the SAME
`studioBeginBoneDrag`/`studioApplyBoneDrag` path the mouse drives (a UI
feature only a mouse can reach cannot be gated) and logs the stored key and the
posed bone tip, which must agree. With `--anim` the animal's locomotion is
frozen so it stays put while being posed.

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

## C++ language server (clangd)

`clangd` 18.1.3 provides completion, navigation and diagnostics. It needs
**nothing from the source tree** — all four pieces are already wired:

| Piece | Where |
|---|---|
| server argv | `.opencode/opencode.json` → `lsp.clangd` (for OpenCode) |
| project config | `.clangd` — points clangd at `build/` and sets the ClangTidy checks |
| compile database | `build/compile_commands.json`, from `CMAKE_EXPORT_COMPILE_COMMANDS ON` |
| convenience symlink | `compile_commands.json` → `build/compile_commands.json` (gitignored) |

The build dir must therefore be configured **once** (`cmake -S . -B build -G
Ninja`) before an editor has flags to work from; the symlink is the fallback for
tools that only look in the project root. Any build regenerates the database, so
it tracks source changes — but note the `voxelforge` target's source list is
explicit (no `GLOB`), so a new `.cpp` is invisible to the build *and* to the
language server until it is listed in `CMakeLists.txt`.

`--clang-tidy` is passed in the argv, and clangd finds `clang-tidy` as a sibling
binary in `/usr/lib/llvm-18/bin/` — installing `clangd` alone is **not**
sufficient, and clangd reports no error if it is missing, the checks just never
run. Package it as `clangd-18` + `clang-tidy-18` (plus the unversioned `clangd`
/ `clang-tidy` metapackages if other tooling shells out to the unversioned
names).

The `.clangd` ClangTidy globs (`modernize*`, `performance*`, `readability*`)
are deliberately trimmed by a commented `Remove:` list, because the broad
`readability*` rules conflict with conventions this codebase uses on purpose
(short `lx/ly/lz` names, lowercase `f` suffixes, numeric constants like
`VOXEL=0.1`, unbraced hot-path statements, C arrays in Vulkan push blocks). Undo
individual entries there if you disagree with a specific suppression.

If a diagnostic looks wrong — "no member named X" on code that compiles — the
database is usually stale; **the compiler is the authority**. Confirm with
`ninja -C build` instead of chasing the squiggles. More detail, including how to
verify the setup with a scripted LSP handshake:
[`.opencode/wiki/concepts/clang-lsp-setup.md`](../.opencode/wiki/concepts/clang-lsp-setup.md).

> **Never run `ninja -t clean` in this repo.** `assets/heightmap.png`,
> `assets/world.json` and `assets/landscape.vxw` are declared as *outputs* of
> the `heightmap_gen` custom command but live in the versioned source tree, so a
> clean deletes tracked files (and the compile database with them). Touch a
> source file to force a recompile.

Build trees: `build` (primary), `build-dbg`, `build-asan`. No validation
layers are installed on the dev machine — correctness gates are selftest /
visual_check / unit tests plus `VF_TRACE`.
