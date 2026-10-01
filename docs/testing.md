# Testing & verification

Tests are **opt-in groups**. Never run a bare `ctest --test-dir build` or look
for an all-tests target: the group gate makes those invocations skip every
check. Build once, then run only the smallest group that covers the change:

```sh
ninja -C build
ninja -C build test-surfel       # surfelize.cpp / surfel geometry
ninja -C build test-live-edit    # ChunkStore, brushes, live GPU patches
ninja -C build test-visual       # render/camera/shader visual acceptance
ninja -C build test-store        # store foundations and rebuild invariants
ninja -C build test-world        # layered world / SVO / records
ninja -C build test-unit         # broad CPU-only change; still not all render gates
ninja -C build test-preview      # brush hover preview only (fastest useful gate)
```

Additional focused groups are `test-effects` (SSAO), `test-textures`,
`test-fog`, and `test-smoke` (the short cross-area profile; `test-fast` is a
compatibility alias). The `visual` and `surfel` groups include the GPU
`--selftest` check. Choose more than one group only when the changed files
cross those boundaries.

`vf_tests` does **not** depend on the bake target — but every field-consuming
test aborts at runtime with `run 'ninja -C build world' first` when assets are
missing. After a clean checkout: `ninja -C build && ninja -C build world`.

## Unit suites (`tests/*.cpp`, binary `vf_tests`, doctest)

| file | covers |
|---|---|
| `test_authoring.cpp` | authoring primitives (capsule, ellipsoid, coneY, smin), stamp hit/pocket/conservative-distance semantics, analytic object SDFs (fence/alpaca parts) — the baked object layers were removed, so no field cross-checks remain there |
| `test_camera.cpp` | basis orthonormality, pitch clamp/yaw monotonicity, movement key handling |
| `test_chat_tools.cpp` | tool-name normalization onto canonical tools, alias defaults, explicit-arg preservation, stamp cell parsing, JSON extractor tolerance, Ollama *and* OpenAI-shaped tool-call parsing, content-embedded calls |
| `test_editable.cpp` | `importLayer` stamps a foreign layer at the anchor, dedupes, rejects bad meta, clips out-of-bounds |
| `test_picking.cpp` | rayPick vs terrain from the hero camera, straight-down top-cell match, object-layer picking by material, bottom-center anchor contract |
| `test_store.cpp` | canonical chunk index round-trip; `ChunkStore` adoption from the synthesized pools vs the `VoxelField` oracle (sign + object flag, tolerating one-voxel SDF quantisation); clear/set/paint edits + local SDF band; Smooth relaxation (terrain spike/pit, object bump collapse, notch fill, wall/post/staircase preservation, convergence, terrain/object ownership, high live-top lookup, non-finite input rejection); localized rebuild == full rebuild (byte-exact) and surface-density preservation; rebuilt octree pool traversal (sign match vs canonical) and store-based chunk surfels; VXW v2 overlay serialize/save/load round trips; deterministic adoption |
| `test_world.cpp` | layered world SVO synthesis sparsity/determinism, `VoxelField` sign vs analytic probes |
| `test_worldfile.cpp` | VXW v1 + v2 section round trips (records, legacy arrays, opaque store section preserved on rewrite), CRC/magic corruption rejection, manifest roundtrip + layered dedupe, record-only layers are valid VXW, placement Euler/local-axis roundtrips |

## `visual_check` (`tests/visual_check.py`)

Headless regression guard, stdlib-only PPM analysis. Renders three canonical
shots at 480×270 via `--shot`:

| shot | camera |
|---|---|
| hero | `-16 6.5 -14 → 6.5 0.8 11` |
| house | `2.5 1.3 6.0 → 6.8 1.0 12.2` |
| water | `8.5 0.6 8.2 → 4.5 -1.1 6.8` |

Assertions per canonical shot (one batched load, 900 s timeout):

- geometry coverage (non-sky pixels) within **3–98.5 %**;
- near-black pixels (lum < 30) inside the silhouette **< 5 %** — this is the
  check that catches hollow-voxel regressions;
- sky probe: top eighth of the frame >50 % blue-dominant (`b ≥ r`).

The gate also renders two hero-camera `VF_SPLAT_DEBUG=16` ownership masks
with `VF_TEST_ROTATE_LIVE=0,0,0` and `45,15,-10` targeting
`hamlet_cabin.vxw`. It requires selected/other-object/terrain-water pixels in
all three debug colours, then checks that the selected green mask changes
while the other-object red mask remains fixed. This is the visual regression
for exact `.vxw` provenance and selected-layer-only rotation. The same
ownership renders set `VF_TEST_TRACKBALL_PROBE=1`; the app reports the
projected AABB centre/radius, raw bounds/camera pose, and the three ring hit
classes. The script independently projects those bounds with the camera basis,
requires the centre to be inside the viewport and match within 0.75 px, and
requires yaw/pitch/roll/centre to resolve to `1/2/3/0`. It also compares the
terrain/water ownership class before/after and allows at most 1% edge motion.
The preview hook must leave `assets/world.json` byte-for-byte unchanged.
`VF_TEST_MOVE_LIVE="dx,dy,dz[,layer]"` is the corresponding preview-only
selected-owner translation diagnostic; it never writes the manifest. With
`VF_TEST_MOVE_PROBE=1`, the fast visual profile also asserts the three
screen-space X/Y/Z handles resolve independently to 1/2/3.

## Selftest (`--selftest`)

GPU-side acceptance at frame 30: same coverage bounds (3–98.5 %), sky probe
pixel at (15W/16, H/8) must be blue-ish, plus a 3×3 grid of average colors on
stderr for quick diagnosis.

## `live_edit_check` (`tests/live_edit_check.py`)

Live-edit (M1–M3) regression guard. Renders a close view at 480×270 and
compares pairs — untouched vs one live store edit
(`VF_TEST_EDIT="432,509,452,<mode>"`, the edit brush's live-store path):
terrain anchor near the camera is edited, dirty chunks rebuild, their surfels
are regenerated from the store and patched into the paged splat buffer, and
the same chunks are patched into the chunk-local SVO buffers. Checks:

- **raise** (Add) in `--mode splat` and `--mode svo`: the edited run logged
  `live edit: … surfels …`;
- **delete** and **paint** (splat): with `VF_MICRO=0` so the diff is the
  geometry — the store-only brush modes must be visible, not just micro-disk
  noise (a separate check runs the same edit with micros on/off and asserts a
  live-patched chunk keeps its micro tail);
- **smooth** can be exercised headlessly with
  `VF_TEST_EDIT=x,y,z,smooth` and `VF_SMOOTH_STRENGTH=0..1`; the focused
  `test_store` cases pin spike lowering, pit raising, terrain ownership,
  object-column protection, object-bump collapse, notch fill, wall/post/
  staircase preservation, and convergence (a second stamp is empty);
- **object smooth** (full gate): a picked cabin surface cell renders through
  the hero camera (`VF_EDIT_DIAM=4.0`) and must log `smooth object:` (the
  surface-axis relaxation, never the terrain path) plus a bounded but visible
  pixel diff;
- **micro persistence**: the same edit run with `VF_MICRO` on must log
  substantially more run surfels than with `VF_MICRO=0` (the live editor
  regenerates the bake's deterministic children instead of dropping them);
- **carve hover preview** (`VF_TEST_BRUSH`, no edit applied): the tint over
  the affected splats must be visible and warm;
- **water plane**: a 6 m ball delete in flat ground beside the river must read
  as the fixed-level water plane (≥ 1 % of the frame changes vs the same edit
  under `VF_SPLAT_NOWATER=1`, with the newly exposed pixels reading as water); a
  carved channel that reaches the river must match the open water's mean colour
  within 12/255 per channel (one level, one shader - a stale height texture used
  to foam-wash it); a subtractive preview over open water must tint no
  water-plane pixel (the carve-scoop A/B was dropped: the channel carve covers
  the same path and `test_editable.cpp` pins the scoop geometry exactly);
- **undo / Clear live edits**: a raise stroke saved to a temp overlay
  (`VF_OVERLAY_PATH`) must dim the frame centre (mean luma ~51 vs ~92), undo
  (`VF_TEST_UNDO`) and `VF_TEST_CLEAR` must restore it (≥ +20 luma, within 12 of
  the untouched frame) and the cleared overlay file must be gone. Regression:
  an emptied chunk's surfel patch was skipped, so the removed material kept
  rendering;
- every edited frame passes the pixel-diff bounds (visible, not frame-wide:
  > 2 %, < 70 %), coverage (3–98.5 %), black-in-silhouette (<9 %) and
  sky-probe checks.

All runs set `VF_NO_OVERLAY=1` so a saved `assets/runtime_edits.vxw` from an
interactive session cannot pollute the comparison.

## `ssao_check` (`tests/ssao_check.py`)

World-scale SSAO regression guard (`ssao.comp` + `ssao_apply.comp`, H / bit 6;
**off by default**, so the canonical `visual_check` shots never enable it).
For each canonical shot (hero/house/water) and each backend
(`--mode splat`, `--mode svo`) it renders three 480×270 frames:
`VF_RENDER_FLAGS=31` (off), `=95` (SSAO on) and `=95 VF_SSAO_DEBUG=1` (raw AO
view), all with `VF_NO_OVERLAY=1`. Checks:

- sky-classified pixels are unchanged between off and on (mean darkening
  ≤ 1 code, max ≤ 20): SSAO must never darken the sky or halo silhouettes;
- non-sky mean luminance drops 0.1–9 % on the land shots (visible grounding,
  no global dimming); the water shot may legitimately stay ≈ 0 (the water
  plane is skipped on purpose);
- near-black pixels inside the silhouette stay < 5 % with SSAO on;
- on the land shots the debug frame's AO term fires on solid pixels
  (mean > 0.01, p99 > 0.15, 0.5–90 % coverage above 0.1) and stays ≈ 0 on sky
  pixels.

Env knobs: `VF_SSAO_STRENGTH` / `VF_SSAO_RADIUS` / `VF_SSAO_BLUR` /
`VF_SSAO_DEBUG` (see `docs/rendering.md`).

## Test groups

| Group | Use for | Includes |
|---|---|---|
| `test-unit` | broad CPU/core changes | all doctest cases |
| `test-surfel` | `surfelize.cpp`, surfel/radius/geometry | focused surfel doctests, fast visual/live checks, GPU selftest |
| `test-store` | `ChunkStore`, edits, rebuilds | chunk doctests + fast live edit |
| `test-world` | layered world, SVO, VXW records | world/worldfile doctests |
| `test-live-edit` | brushes, live patches, undo/clear | store doctests + full live-edit check + fast variant |
| `test-preview` | brush hover preview: tint hue, Depth marker, SVO parity | `live_edit_check.py --only preview` (renders its own baseline) |
| `test-visual` | camera, shaders, scene geometry | full visual check + fast variant + GPU selftest |
| `test-effects` | SSAO/post effects | full SSAO check + fast variant |
| `test-textures` | atlas/material textures | full texture check + fast variant |
| `test-fog` | volumetric fog | full fog check + fast variant |
| `test-smoke` | quick cross-area sanity | all fast variants; `test-fast` is an alias |

The CTest entries remain individually discoverable with `ctest -N`, but they
are group-gated. A bare `ctest` invocation is intentionally not a release or
iteration command; there is no `test-all` target.

## Fast debug workflows

For quick edit/build iteration, use the short smoke group:

```sh
ninja -C build test-smoke       # `test-fast` is a compatibility alias
```

For a surfel-only change, prefer `ninja -C build test-surfel`; for a live
editing change use `ninja -C build test-live-edit`. The smoke scripts are
selected with `VF_FAST_TESTS=1` internally and never start unrelated full
render matrices.

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
| ImGui renders nothing | dynamic-rendering wrap lost in `frame/record_interactive.cpp` |
