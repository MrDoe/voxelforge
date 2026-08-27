# Testing & verification

Run the gates **in this order** before declaring any shader/world/code change
done:

```sh
ninja -C build && ctest --test-dir build          # 1+2: build, unit + visual
python3 tests/visual_check.py build/voxelforge    # 3: canonical shots (also in ctest)
./build/voxelforge --selftest --width 640 --height 360   # 4: GPU acceptance
```

Single suite during iteration:

```sh
./build/vf_tests --test-case="*world*"            # doctest filter
```

`vf_tests` does **not** depend on the bake target — but every field-consuming
test aborts at runtime with `run 'ninja -C build world' first` when assets are
missing. After a clean checkout: `ninja -C build && ninja -C build world`.

## Unit suites (`tests/*.cpp`, binary `vf_tests`, doctest)

| file | covers |
|---|---|
| `test_authoring.cpp` | authoring primitives (capsule, ellipsoid, coneY, smin), stamp hit/pocket/conservative-distance semantics; baked-field cross-checks (cabin walls solid with carved door, fence rails solid + gate open, alpaca parts) against the analytic truth |
| `test_camera.cpp` | basis orthonormality, pitch clamp/yaw monotonicity, movement key handling |
| `test_chat_tools.cpp` | tool-name normalization onto canonical tools, alias defaults, explicit-arg preservation, stamp cell parsing, JSON extractor tolerance, Ollama *and* OpenAI-shaped tool-call parsing, content-embedded calls |
| `test_editable.cpp` | `importLayer` stamps a foreign layer at the anchor, dedupes, rejects bad meta, clips out-of-bounds |
| `test_picking.cpp` | rayPick vs terrain from the hero camera, straight-down top-cell match, object-layer picking by material, bottom-center anchor contract |
| `test_world.cpp` | layered world SVO synthesis sparsity/determinism, `VoxelField` sign vs analytic probes |
| `test_worldfile.cpp` | VXW roundtrip, CRC/magic corruption rejection, manifest roundtrip + layered dedupe, record-only layers are valid VXW |

## `visual_check` (`tests/visual_check.py`)

Headless regression guard, stdlib-only PPM analysis. Renders three canonical
shots at 480×270 via `--shot`:

| shot | camera |
|---|---|
| hero | `-16 6.5 -14 → 6.5 0.8 11` |
| house | `2.5 1.3 6.0 → 6.8 1.0 12.2` |
| water | `8.5 0.6 8.2 → 4.5 -1.1 6.8` |

Assertions per shot (timeout 300 s each):

- geometry coverage (non-sky pixels) within **3–97 %**;
- near-black pixels (lum < 30) inside the silhouette **< 5 %** — this is the
  check that catches hollow-voxel regressions;
- sky probe: top eighth of the frame >50 % blue-dominant (`b ≥ r`).

## Selftest (`--selftest`)

GPU-side acceptance at frame 30: same coverage bounds (3–97 %), sky probe
pixel at (15W/16, H/8) must be blue-ish, plus a 3×3 grid of average colors on
stderr for quick diagnosis.

## Fast debug workflows

```sh
# point query against the live merged field (no Vulkan init):
./build/voxelforge --probe X Y Z

# ASCII cross-sections (no window):
./build/vf_slice --axis z --center X Y Z --span 10

# deterministic single frames:
./build/voxelforge --shot /tmp/f.ppm --cam X Y Z TX TY TZ \
                   --width 640 --height 360 --animtime 0

# determinism tracing of SVO buffer hashes:
VF_TRACE=1 ./build/voxelforge --smoke 10

# HUD-inclusive screenshot:
VF_HUD_SHOT=30:/tmp/hud.ppm ./build/voxelforge
```

PPM files can be reviewed without an image viewer via
`.opencode/skills/voxel-object/scripts/ascii_view.py <ppm> COLS ROWS`.
Judge screenshots by pixel evidence only — never by asking a vision model.

## What to do when a gate fails

| failure | likely cause |
|---|---|
| unit_tests aborts "run 'ninja -C build world'" | assets missing/stale — bake |
| visual_check black-in-silhouette spike | hollow objects / brick SDF regression — check recent synthesis or shader changes; slice the object |
| selftest coverage out of range | camera sees all-sky or no-terrain: layer enable state or heightfield breakage |
| determinism hash drift under VF_TRACE | unordered iteration crept into synthesis — chunk order must stay deterministic |
| ImGui renders nothing | dynamic-rendering wrap lost in main.cpp frame loop |
