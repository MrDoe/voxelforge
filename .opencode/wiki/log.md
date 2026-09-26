# log

## 2026-09-21 ingest | live arcball rotation (GPU transform, no rebuild)
The Rotate drag is now truly live: instead of accumulating angles and
showing only a tint until release, the splat vertex shader rotates the
dragged layer's surfels about the placed pivot every frame (bind 14
`RotUBO`; `SplatPass::setRotatePreview`). No surfel rebuild, no world
reload, and terrain is untouched — a new bit 3 (+8) on the surfel
`mat_ao.w` word tags object-layer surfels (set AFTER the AO smoothing
pass, which averages that channel over face neighbours and would wash
the bit out). Water keeps bit 2 (+2); the two never collide (object
surfels are never water). Both decoders updated (splat.frag,
splat_tile_render.comp: `isWater = w>1.5 && w<5.5`, subtract 8 when
`w>5.5`). On release the preview is cleared and the manifest commit +
incremental rebuild makes it permanent. An on-top cyan gizmo (silhouette
circle + two meridian ellipses) marks the drag sphere while Rotate is
active, and the arcball sphere is now centred on the object's projected
AABB with a radius from its projected half-diagonal (was a full-screen
unit sphere). Headless hook `VF_TEST_ROTATE_LIVE="yaw,pitch,roll"` arms
the preview without committing (verified: 45 deg yaw halved the tagged
area and moved its centroid; green debug mode 16 shows the tag).
Gates: unit (81), visual, texture, live_edit, ssao all green.
A buffer overrun in the descriptor array (`b[16]` written at index 16)
caused stack smashing — fixed to `b[17]`/`bindingCount 17`.
Updated [[concepts/layer-placement]].

## 2026-09-20 ingest | arcball layer rotation (full 3-axis placement)
Added free 0–360° rotation on all three axes to object layers:
`WorldLayer` gained `rotX`/`rotZ` (pitch/roll) alongside `rotDeg` (yaw);
the manifest writes `rot`/`rotX`/`rotZ`; `worldfile::transformRecords`
now composites `Ry·Rx·Rz` about the layer's bottom-center via a
glm::mat3 and its transpose inverse (destination-driven inverse map).
`LayeredWorld` folds all three angles into the placement dirty hash.
`EditBrush::Rotate` brush + `Ctrl+LMB` pick resolve the target layer
by its placed AABB and arcball-drag it (trivially verifiable via
`VF_TEST_ROTATE`/`VF_ROTATE_LAYER`, live AABB tint, `VF_TEST_BRUSH`);
`LayeredWorld::layerBox()` exposes per-layer placed AABBs.
Docs updated (AGENTS.md, contributing.md, world-format.md). Unit tests
extended (3-axis rotation, pivot-centre vs cell-edge at 90°, manifest
roundtrip). Verified the new box tint branch in splat.frag exercises
cleanly under VF_TEST_BRUSH (73k cyan pixels over the target AABB).
Headless `--cam` comma form fixed to accept single quoted tokens
(verified 229k distinct colours vs the flat-grey no-op it silently
rendered before). Created [[concepts/layer-placement]].

## 2026-08-24 ingest | PBR shading pass (ggx + sdf ao + sky ambient + aerial fog)
Implemented the photorealism upgrade: GGX BRDF, multi-scale SDF AO with bent normal,
sky-model-driven ambient, aerial-perspective fog, foliage translucency, two-scale
terrain normals, fbm grass detail, 2-octave water ripples, vibrance grading.
Both backends updated in lockstep; `--compare` 6.76/255, visual_check green.
Created [[entities/svo-render]] and [[concepts/shading-model]].

## 2026-08-24 ingest | realistic grass & foliage field
Added two-layer flora to shadeTerrain (both backends): warped-noise micro-grass texture
(fills gaps, wind-animated) + structured tufts (tapered blade segments, per-blade color /
dry tips / height AO) for grass, and two-sided leaf-cluster facets for canopy/bushes.
LOD 9-26 m with grassDetail fallback. Key finding: sparse analytic blades cover only ~4 %
of area — blade-scale noise layering is what makes it read as grass. Parity 6.76/255
unchanged, visual_check green, ~0 ms/frame. Updated [[concepts/shading-model]].

## 2026-08-24 ingest | high-res grass sprite cards
Replaced grass blade-tufts with ray-intersected alpha-tested crossed cards
(tuftAlpha analytic pattern, 2 decorrelated grids coarse+fine, per-card albedo,
cards occlude terrain). Lessons: a single card grid tiles visibly; 6 blades/card
looked like paper — 9 narrower wavy blades + wispy tips fixed it; cards are
resolution-independent (no assets). Parity 6.76/255, all gates green, +0.05 ms/frame.

## 2026-08-24 ingest | grass disabled by LOD t0 bug + coverage rework
ROOT CAUSE of "no grass sprites": flora/card LOD used `t - t0` (AABB entry); with the
camera inside the world volume t0 < 0 inflated dist by ~70 m -> smoothstep(9,26,d)=0
-> entire grass layer silent in ALL renders (vision judge still reported 'tufts' -
hallucinated). Fixed: `length(p - ro)`. Also: hit-space pattern sampling for steep
near rays, anisotropic vertical streak fill texture, Smith visibility clamp <=6
(fireflies), LOD widened to 12-40 m. Parity 6.70/255, gates green, +1.6 ms/frame.

## 2026-08-24 ingest | voxel-object authoring skill + verification tooling
Added the `voxel-object` skill (layer-by-layer SDF/stamp authoring, CPU-first
verification) with bundled `ascii_view.py`. New: `tools/scene_slice.cpp`
(target `vf_slice`) prints ASCII scene() cross-sections; `common.hpp` gained
sdCapsule/sdEllipsoid/sdConeY/smin + StampCell/stampAt (bucket-indexed);
tests/test_authoring.cpp covers all of it plus cabin wall/door parity.
Key findings encoded: --probe exits pre-Vulkan (pure CPU); the layer loop
needs no world.vxw regen; IQ ellipsoid degenerates at center (guard returns
-min(r)); tall objects need nearObject() bake-band extension in heightmap_gen.
Validated vf_slice against the known cabin (foundation, log courses, carved
door gap at x 5.28-6.32, roof stack). ctest green (unit + visual_check).
Created [[concepts/voxel-object-authoring]].

## 2026-08-24 ingest | layered world files (vxw split + manifest)
World is now data-driven: heightmap_gen emits record-only layers (landscape,
house, tree1..6, rock1..3, bushes, alpaca, fence1) + `assets/world.json`
manifest (order = dedupe priority) + merged cache world.vxw (full-scene SVO +
union). Runtime: splats read live layers via `worldfile::readLayered`; SVO
stays on the merged cache until regen — object edits/inserts need NO recompile
(proven: python-patched alpaca.vxw +10 cells changed the splat render,
pixel-diff verified; 60 fence cells ceded per first-wins priority).
worldfile lib gained WorldLayer + minimal JSON manifest parser/writer (bug
found by test: commas inside pos arrays were never consumed). Tests: manifest
roundtrip, layered dedupe priority, record-only VXW validity. ctest green.

## 2026-08-24 ingest | alpaca paddock at the cabin
fenceAt: post-and-rail perimeter around kPaddockMin/Max (8.2..13.8 x 14.2..19.8),
posts anchored to local ground, slope-following rail capsules at +0.36/+0.70,
gate gap on west side (kGateCenter±0.75, mid-gate posts skipped). alpacaAt:
layer-built L0-L4 (legs mat2, smin-blended wool body mat5, neck/head, muzzle,
tail, banana ears) at kAlpacaSpot facing -x. Bugs caught by slice+probe loop:
mid-gate post blocked the gate; ObjHit mat not reset after legs overwrote it.
Composed into scene(), nearObject() band extended for paddock, splat normal
lambdas include new objects. test_authoring covers rails/gate/materials.

## 2026-08-24 ingest | world-layers GUI (ImGui) + hud capture hooks
Added "World objects" panel to the interactive HUD: checkbox per manifest layer
(landscape locked on), toggles persist straight to assets/world.json
(WorldLayer.enabled + JSON true/false support in the hand-rolled parser,
robust skipValue() for unknown keys) and hot-rebuild splats via
rebuildSplats(). "Rescan assets folder" lists unmanifested *.vxw as "(new)";
enabling one appends it to the manifest. Verified end-to-end via new debug
hooks VF_HUD_SHOT=frames:path (swapchain readback incl. HUD) and
VF_GUI_TEST=<name> (drives the exact checkbox path): alpaca disable ->
manifest "enabled": false -> splat reload 4452423->4451161 records. Panel
rendering confirmed by pixel evidence in splat mode. NOTE: interactive
ray-march mode currently shows no HUD pixels at all (pre-existing /
concurrent-session territory; external gnome-screenshot confirms) - layer
toggles there apply after 'ninja -C build world'. Concurrent session landed
ChatUi/EditableWorld/Ollama sources mid-task; tree now builds with both.

## 2026-08-24 ingest | MCP server (vf_mcp) + gemma tool-bridge fixes
Fixed "unknown tool rock_1": chat bridge now fuzzy-normalizes invented names
(src/ai/tools.hpp normalizeToolCall: verb-prefix/trailing-counter stripping +
noun-family aliasing + injected defaults - rock->create_ellipsoid mat 4 with
boulder radius), wires the previously-missing list_world/probe/create_stamp
handlers, and self-corrects rejected calls by feeding an error message back
to the model (max 2 retries). System prompt rewritten: exact six tool names,
clean JSON examples. NEW: vf_mcp - standalone stdio MCP server (JSON-RPC 2.0,
newline framing) exposing list_layers/enable_layer/probe/ground/add_box/
add_cylinder/add_ellipsoid/add_stamp/clear_edits backed by EditableWorld
(ai_edits.vxw, auto-manifest). Anchors accept ground:[x,z] terrain snapping.
Registered in .opencode/opencode.json (restart opencode to load).
Protocol lesson: spdlog defaults to STDOUT which corrupts MCP stdio - server
swaps default logger to stderr before anything logs. Tests:
tests/test_chat_tools.cpp (normalizer, defaults, stamp cells, extractors).

## 2026-08-24 ingest | AI edits are scene truth now (visibility fix)
Root cause of "no item visible": ray-march renders SVO bricks built by
World::build() marching scene() - data-only ai_edits were invisible to that
march, so neither cache merges nor repacks could ever show them. Fix:
ai_edits cells register into a global registry (common.hpp aiEditsRegister/
aiEditsAt) consulted by scene(); EditableWorld load/append/clear maintain it;
heightmap_gen registers BEFORE the bake and merges the layer first-priority.
Distance semantics mirror stampAt: AABB-bounded conservative outside, exact
cube distance inside - the naive clamp-to-+VOXEL version made the ENTIRE sky
a surface band (world.vxw exploded to 4.3 GB). RACE fixed: packer no longer
writes ai_edits.vxw back (consumes only) - its start-of-bake snapshot used to
stomp edits appended during a ~20 s bake; vf_mcp queueRepack is now a
detached flock-serialized shell child (survives server exit; threads die with
the process). Verified: add_ellipsoid via MCP -> repack -> +8192 SVO words,
2315 px coherent change in ray-march shot vs no-edit baseline; ctest green.

## 2026-08-24 ingest | llama.cpp server support for the in-game chat
OllamaClient now speaks both dialects: sticky chat path (/api/chat vs
/v1/chat/completions), /v1-suffixed URLs normalized (no double prefix),
OpenAI-shaped tool_calls parsed with ESCAPED-STRING arguments unescaped into
clean JSON, null-safe content, and content-embedded {"name":...,"arguments":
{...}} synthesis for models without native function calling (gemma templates
on llama.cpp without --jinja). Verified end-to-end against a mock llama-server
wire format: ping + one chat -> create_ellipsoid with extractable material=4.
Bugs found on the way: mangled multichar char literal ('""') made the JSON
string-skipper a no-op (infinite loop in brace matching); find-by-key loops
required whitespace-skipping after colons (json.dumps formatting). Tests:
4 new parse cases incl. whitespace-formatted responses; ctest green.

## 2026-09-15 ingest | edit-brush hover preview + Delete/Paint modes
The edit tool (C) now has four brush modes: Carve (cylinder scoop), Add (dome),
Delete (clear the brush ball) and Paint (recolour the ball with the new Material
combo). Delete/Paint are `StoreEdit::Clear`/`Paint` and always patch the live
store; Carve/Add keep the record-layer path when "Live patch" is off. Hovering
tints the affected splats before the click: bind 13 `BrushUBO` (centre+radius,
axis+half length, rgba tint), tested against the surfel CENTRE (per-splat, like
the CPU rasterizer) plus a 0.06 m skin so the surfers' `+0.05 m` emitter offset
stays inside; warm orange = carve, red = delete, material colour = paint;
`VF_SPLAT_DEBUG=15` draws the volume as a magenta mask; `VF_TEST_BRUSH` renders
the preview headlessly. Along the way: fixed a pre-existing `LiveEditor::stamp`
bug — a freshly GPU-seeded chunk is the PRE-edit run, so the first stamp in a
chunk uploaded the old surface back and looked like a no-op (single stamps of
diameter <= 2 m changed nothing in the splat backend); stamp() now refreshes
the edited AABB ±3 on top of a fresh seed. Test gaps closed in
`tests/live_edit_check.py` (delete/paint with `VF_MICRO=0` so the diff is
geometry, plus a warm-tint preview check); ctest green. New `VF_NO_OVERLAY=1`
skips restoring `assets/runtime_edits.vxw` (a 36 MB session overlay restored by
the app had silently broken visual_check/live_edit_check); both test scripts set
it. Created [[entities/live-edit-brush]].

## 2026-09-15 lint | stale pages after the surfel rework
`entities/svo-render.md`, `concepts/shading-model.md` and
`concepts/voxel-object-authoring.md` still describe the pre-rework world
(`scene()` analytic geometry, `world.vxw` merged cache, "the single render
path", `--compare` parity runs). Current reality: `.vxw` records only, Gaussian
surfels primary + SVO reference, agent/edit flows. `entities/svo-render.md` got
a role banner; the other two need a full rewrite from `AGENTS.md` + `docs/`
next session. No orphan pages (all linked from `index.md`); the new live-edit
page links back to [[entities/svo-render]].

## 2026-09-15 ingest | edits always live + water level respected when carving
Editing has no bake path any more: the "Live patch (no bake)" checkbox is
gone, `App::applyEdit()` (record layers `carve_edits.vxw` / `raise_edits.vxw`
+ hot reload) is deleted, and every brush stamp goes through
`applyEditLive()` → `ChunkStore` → per-chunk GPU patching. The hover/anchors
always `rayPickStore`, drag painting and the stroke overlay work for all four
modes, and the panel's legacy clear buttons were replaced by one "Clear live
edits" button (drops `runtime_edits.vxw`, then reloads). Second change:
Carve/Delete now respect the water level — a scoop aimed at a cell below
`WATER_LEVEL` (-0.9) is refused outright, no `Clear` edit touches a cell below
the plane (`yMinDry`, lattice y >= 503; the log reports the held-back count),
and the subtractive hover tint clips at the plane via `bFlags.x` in the brush
UBO (A/B verified: 1792 water-plane pixels tinted without the flag, 0 with it).
Tests extended in `tests/live_edit_check.py` (deep-scoop log, refused-scoop
pixel identity, shore-preview water check); gates green. Updated
[[entities/live-edit-brush]].

## 2026-09-15 ingest | edits always live, digs below the water plane are flooded
Editing no longer has a bake path: the "Live patch" checkbox, `App::applyEdit()`
and the `carve_edits.vxw`/`raise_edits.vxw` writers are gone; every stamp goes
through `applyEditLive()`, drag painting and the stroke overlay work for all
four modes, and the panel's legacy clear buttons became one "Clear live edits"
button (drops `runtime_edits.vxw`). Per the follow-up request, carving/deleting
below `WATER_LEVEL` is *allowed* again (the refusal + cell clamp from the
previous pass were removed) and the dug volume is **filled with water**:
`floodNewlyDug` appends water-plane splats over the columns a subtractive stamp
opened below the plane (store-based open-column test, wet-grid dedupe) and
`patchWaterSurfels` re-uploads the ~9k-splat water run (4096 slots of reserved
headroom; identity indirection entries). Verified: an open 6 m pit in flat
ground floods with 694 splats and changes 21.6% of a top-down frame (A/B
`VF_NO_WATER_FILL=1`), the pit reads blue in the flooded render, and a
subtractive preview still never tints the water surface.

Two store/GPU bugs surfaced while testing and are fixed:
1. `rebuildChunk` let the `SolidBox` fill overwrite cells that a brick defines
   as *air* (`kind[k] != 0` only knows solid bricks): a deep clear stayed solid
   (114810 of 154975 cells came back), which also hid the pit and starved the
   flood. Now `fromBrick[]` gates the fill; regression test
   "a deep clear stays air under the terrain boxes" (16/31 cells fail without
   the fix). An all-air region block under a kept straddling box may also not
   drop its brick, or `cellAt` falls through to the box.
2. `patchWaterSurfels` growth destroyed the compaction buffer before the
   replacement existed; a failed allocation left the scene unrenderable (a
   uniform grey frame). The water run now carries headroom and the replacement
   is created before the old buffer is released.

Gates: unit_tests (incl. the new case), visual_check, live_edit_check and
--selftest all green. Updated [[entities/live-edit-brush]] and the docs.

## 2026-09-16 ingest | detail roadmap + micros survive live edits (Phase 1)
Analysed the four detail layers (10 cm lattice, soft base disks, hash-driven
micros, LOD rings/shading) and decided a 5-phase roadmap with the user:
(2) per-chunk object mask for selective micro distances, (3) LOD quality
(material-split blocks, 8³ ring), (4) anisotropic 80 B surfels, (5) 5 cm
lattice as Option A (CHUNK_N=64 → 3.2 m chunks, GRID_N=32) under a strict
budget gate (≤+1 GB VRAM, bake ≤10 s; default stays 10 cm if it fails).
Phase 1 implemented right away: extracted the bake's per-cell micro emitter
into the shared `emitMicroSurfelsForCell`, exposed `buildMicroSurfels(keys,
base)`, taught `LiveEditor` to regenerate a patched chunk's micro tail
(`chunkRun`/`microStartOf`, run cache invalidated on `runDirty` **and**
`microDetail` changes) and extended `SplatPass::patchChunkSurfels` with the
`microOffset` split. Measured: stamp latency unchanged (16–21 ms), patched
run 46651 vs 21364 surfels with micros on/off. Unit tests (determinism,
parent proximity, run cache) + `live_edit_check` micro-persistence check
added; ctest green (unit_tests, visual_check, live_edit_check), selftest
with the real 46 MB session overlay restores 50 chunks incl. micros.
Updated AGENTS.md, docs/{architecture,rendering,testing}.md, fixed the stale
`VF_MICRO_DIST` comment (20 m, not 40). Created [[concepts/detail-pipeline]].

## 2026-09-16 ingest | new default world: lakeside hamlet + old-scenery cleanup
Built a completely new, more detailed scenery with the `vf_mcp` object tools:
nine runtime-authored layers (hall, tower, pier, boat, well, market, garden,
pines, props) on the valley lake's north shore, placed via `ground` anchors and
verified with `vf_probe`/`vf_slice` plus ascii_view shots. Then deleted the old
scenery files (house, tree1..6, rock1..3, alpaca, fence1, bridge, forest, dock,
shore, ferns, bushes, the five winter EM_* assets, carve/raise edits) and the
stale 46 MB `runtime_edits.vxw` session overlay; world.json now lists only
`landscape` + `hamlet_*`. The baker (`heightmap_gen`) was trimmed to emit only
the terrain shell and to **preserve foreign manifest entries** whose `.vxw`
still exists, so `ninja -C build world` no longer resurrects the old layers;
its object sweeps are gone (the analytic `common.hpp` shapes stay as test
fixtures). Tests adapted: `test_world` compares the field against the terrain
heightfield only (the probe path now follows the terrain), `test_authoring`
lost its baked-field cases (cabin/paddock), the surfelize count bound was
raised for the bigger all-layers world, and a chunk-crossing plateau in the
store test now checks the union of both affected chunks. Also fixed a real MCP
bug: `enable_layer` declares `enabled` as boolean but `jsonGetInt` only parsed
digits (silently kept layers enabled) — it now accepts true/false. Gates:
unit_tests, visual_check, live_edit_check, --selftest all green; visual_check's
water shot needed the pier/boat shifted 5 m east (the camera used to sit inside
the new pier). Created [[entities/hamlet-scene]]; updated
[[concepts/detail-pipeline]] (phases 1-4 done, 5 pending).

## [2026-09-17] ingest | Screen-space AO (world-scale) + the normal G-buffer

Reworked SSAO from a fixed-pixel-spiral depth-difference hack into a
world-scale two-band kernel and gave both backends a normal G-buffer.
Motivation: the old kernel's world footprint was resolution/distance
dependent, haloed at silhouettes and only read on chaotic foliage ("looks
great for leaves").

- **`m_gnorm`** (rgba16f) written by `splat.frag` (3rd attachment) and
  `svo_raymarch.comp` (binding 12), tile path binding 21; consumed by SSAO
  and SSR. Water stores the flat plane normal, sky 0.
- **`ssao.comp`** -> raw AO in a new `m_ssaoAo` scratch (two bands, world
  radii from `pc.b.y`/`VF_SSAO_RADIUS` = 0.8 m, out-of-bounds/sky/water
  dilute instead of occluding, distance fade); **`ssao_apply.comp`** ->
  5x5 cross-bilateral denoise + `base*(1-strength*ao)`.
- Pass-local **extra push range**: shared 128 B `RaymarchPush` + one vec4
  (debug/strength/radius/blur) via a second `vkCmdPushConstants` at
  offset 128; no change to the shared struct.
- New `tests/ssao_check.py` (ctest `ssao_check`): hero/house/water x
  splat/svo, off/on/debug; sky guard, bounded mean darkening (0.1-9% on
  land), black-in-silhouette, AO-fires assertions. Docs updated
  (`docs/rendering.md`, `docs/rendering-comparison.md`, `docs/testing.md`).
- Tuning: strength 0.6, far radius 0.8 m, gain 5.0; perf ~1.2 ms @720p,
  ~2.5 ms @1080p. SVO reads ~3-4x stronger than splat at identical
  settings (exact SDF normals on stepped geometry vs smoothed surfel
  normals) - expected, documented on [[concepts/ssao-gbuffer]].
- Gates: unit_tests, visual_check, live_edit_check, ssao_check, --selftest
  all green.

## [2026-09-17] fix | River water reaches carved channels (flood + carve scoop)

Bug: carving a channel below `WATER_LEVEL` never flooded it (the splat
backend stayed dry; reported as "river water is not flowing into carved out
channels"). Root cause was two compounding bugs found by instrumenting
`floodNewlyDug` on a headless carve at the riverbank test spot (cell
512,508,512; delete-pit control flooded 697 splats, the carve only 28):

- `EditableWorld::makeOrientedCylinder`: the carve cylinder's top face sat
  exactly at the picked cell's centre and `glm::rotation(up, axisDir)` is
  degenerate for an antiparallel axis (returns an arbitrarily tilted basis,
  tilt ~1e-7), so an exact `d > 0` surface test dropped half the base-plane
  layer AND the far-end floor layer. The scoop kept a one-cell roof over most
  of the disk. Fix: `kCarveTopMargin = 2*VOXEL` (0.2 m) along the axis + a
  `VOXEL*1e-3` inclusive-boundary epsilon; floor stays at the picked depth,
  Add keeps `[0, length]`, hover preview uses the same margin.
- `App::floodNewlyDug`: the "first unchanged cell above" cap test used
  `.solid` (`sdfRaw <= 0`), but the bake's `int(d/VOXEL)` truncation makes the
  cell above a surface carve store `raw == 0` (the surface skin). Every
  surface-level scoop read as covered. Fix: walk up through `raw == 0` cells;
  only `raw < 0` (real material) caps.

Result: carve floods 687-715 splats (was 28-85) for the same scoop. Tests:
new `tests/test_editable.cpp` case (A/B verified: fails with the rasterizer
fix disabled) and a carve flood A/B in `tests/live_edit_check.py`
(>=400 splats, >1 % changed, >=200 water-surface px). Gates: unit_tests,
visual_check, live_edit_check, ssao_check all green (ssao_check first run hit
a flaky 300 s per-render timeout under load, passed on re-run).

Wiki: new [[concepts/water-flooding]] (page later replaced by
[[concepts/water-plane]]); [[entities/live-edit-brush]] table +
water/tests sections refreshed (stale "stops at the water level" notes gone).

## [2026-09-17] rework | One fixed-level water plane (no flood, no water-grid patch)

Direction from the session: the water should look the same in carved areas —
one fixed level, same material, same shader — and the aim is realistic water
with accurate reflections. The old design (water splats only where the *baked*
terrain was below the plane + `floodNewlyDug` patching the run after a dig)
made digs a special case and read the stale height texture, so carved water
shaded as a thin foam-washed sheet ((30,62,58) green vs the river's
(56,93,110) at the same camera).

Now: the water is the analytic plane, period.

- Splat backend: `buildWaterSurfels` emits a **world-wide** 0.2 m grid of
  coplanar surfels (513² = 263 169; ~+5 % surfels, ~0.2–0.45 ms fill at
  640×360), no terrain test. The water branch already shades the analytic
  plane per fragment and the depth test against the opacity prepass clips it,
  so digs below the level are water automatically. Deleted: `floodNewlyDug`,
  the wet-grid bitset, `SplatPass::patchWaterSurfels`, the 4096-slot water
  headroom, `VF_NO_WATER_FILL` (A/B now `VF_SPLAT_NOWATER=1`, which existed).
- Shading: `App::patchHeightTexture` keeps the terrain height texture
  (rg32f topY+material) in sync with the runtime store for the edited columns
  after every stamp and overlay restore, via the new
  `vf::uploadSubImage3D`. The water's foam/absorption/reflected-bed march, the
  splat shadow/AO marches and the SVO submerged-bed tint all read it.
- Measured parity (channel carved at the shore, same frame): splat existing
  water (93,113,124) vs newly exposed (91,113,125); SVO (89,112,123) vs
  (87,111,122). Base frame unchanged (0.09 %).
- Tests: `check_water_fill` rewritten (pit + carve A/B vs `VF_SPLAT_NOWATER=1`,
  plus the channel colour-parity assertion); the carve-geometry unit test
  stays. Gates: unit_tests, visual_check, live_edit_check, ssao_check green.

## [2026-09-18] fix | Undo + "Clear live edits" (the emptied-chunk GPU patch), Add preview

User reports: "Clear Live Edit doesn't do anything"; "a undo button would be
great"; "highlight in C mode with add doesn't work" (i.e. the Add brush had no
hover preview).

Root cause of both Clear and the missing undo: `SplatPass::patchChunkSurfels`
rejected zero-surfel patches (an empty vector's `data()` is null and tripped
its `!data` guard), so a chunk whose whole run became empty kept drawing the
removed material — the store, the LiveEditor cache and the re-derived surfels
were all correct (my new unit tests show them round-tripping); only the GPU
layer was stale. Fix: accept `count == 0` and zero `m_chunkCount` +
micro-start/LOD flags.

Shipped:
- Undo: per-stroke pre-edit cell map (first occurrence wins, 250k cells / 32
  steps), replayed through the shared `App::commitStoreEdits` (store →
  LiveEditor → GPU patch → height texture → overlay save). Panel button +
  Ctrl+Z. `VF_TEST_UNDO=1`.
- Clear live edits: in-place revert (flush writer → delete overlay → re-adopt
  the baked pools via `LayeredWorld::invalidateStore` → re-seed the touched
  chunks' surfels/SVO/height texture) + an async full reload so the bake's LOD
  and normals return. `VF_TEST_CLEAR=1`.
- Add hover preview: the dome's exact half-ellipsoid as a new shader primitive
  (negative axis half-length), tinting the ground patch the dome buries green
  (carve stays warm orange, delete red, paint material colour).
- `VF_OVERLAY_PATH` so tests keep their overlays out of `assets/`.

New regressions: `tests/test_store.cpp` ("set then clear round-trips the
chunk", "live editor set-then-clear round-trips the cached run") + the
`check_undo_and_clear` and Add-preview cases in `tests/live_edit_check.py`
(which also lost the redundant carve-scoop water A/B and now reuses the splat
edit log for the micro check). Gates green: `vf_tests` (71/71),
`live_edit_check` (6m09, ~20 renders), `visual_check` (56 s).

Load-cost finding (filed as [[concepts/load-time-field-build]]): the 13.2 s
`VoxelField::build` EDT is bbox-bound — 72k components but 361M padded bbox
cells (~680x) — so every test render costs ~18 s regardless of resolution;
batching nearby components is the fix. New quirks: the zero-surfel patch
gotcha and the bbox-bound load cost.

## [2026-09-19] ingest | per-cell texture (phase 2) wired end to end
The record's `reserved` byte now reaches the shader as an atlas-layer
override. Four silent-loss bugs were found and fixed along the chain (the
v1 VXW reader dropped the byte; the object hash is uint32 so bits 48+ were
truncated; `ChunkStore::apply` didn't decode it back; `writeManifest`
rewrote world.json bare and dropped the `textures` table). The live-edit
overlay schema bumped 2 → 3 because the byte used to be uninitialized
filler. Verified with a new `[phase2]` section in texture_check.py (0.41%
diff on a test block) plus two new unit tests.
## [2026-09-19] ingest | texture atlas (phase 1) documented
Material-indexed PNG textures via a `world.json` "textures" table, shipped
earlier this session; page records the 17-layer layout, bindings 22/23, the
triplanar projection tradeoff and the VF_TEXTURES=0 escape hatch.

## [2026-09-19] ingest | photo textures shipped + GUI picker
Replaced the placeholder checker/stripe textures with mean-matched ambientCG
(CC0) photos for all ten natural materials; added `tools/fetch_textures.py`
(download + palette mean-match) and `tools/gen_textures.py` (offline
procedural set, `ninja -C build vf_textures`). Atlas layers 128 -> 256 and
the loader now takes JPEG. New in-app "Textures (material albedo)" picker
writes only the `textures` key via `worldfile::writeTextureManifest`.
Fixed a pre-existing teardown crash (`App::destroy` must release the atlas
before `ctx.shutdown`) and relaxed the live_edit water-tint guard, which was
failing at 2-3 shoreline pixels on the baseline too. See
[[concepts/texture-atlas]] + [[concepts/per-cell-texture]].

## [2026-09-19] decision | assets are tracked now
`.gitignore` no longer ignores `assets/` (textures, layer .vxw, world.json):
the authored scene is not reproducible from code. Only
`assets/runtime_edits.vxw` stays ignored - it is live session state the app
rewrites on every brush stroke.

## [2026-09-19] ingest | surfel holes on walls + stepped roof fixed
User reported walls/roof "look unsolid" with horizontal slots, strongest at
grazing angles (SVO clean => splat artifact). Root cause: the object SDF
marks enclosed air (interiors, hollow roof/wall shells) as solid, and
`buildSurfels` enumerated those buried cells; `meanNormal`'s (0,1,0)
fallback then (a) tilted the smoothed normals of real surface cells and
(b) filled the enclosures with dark up-facing disks that the depth band
blended through at grazing angles. Fix in `src/voxel/surfelize.cpp`:
return a zero normal when no neighbour is air + drop those cells before
pass 2 (matches the live/store path), halve the non-foliage micro facet
tilt/AO contrast (foliage keeps its tilt), and make object disks SHRINK
with local normal disagreement instead of growing (crisper edges/corners;
terrain keeps the sealing growth). Also fixed a crash in the carve path
(free `processComponent` read `s.argtex` without sizing it). Verified:
`vf_tests` 76/76, visual/live-edit/texture checks pass, `ssao_check` only
the pre-existing marginal hero/svo AO threshold; cs10 dark 4.35 -> 3.12 %,
HF 5.98 -> 5.12. See [[concepts/surfel-holes]].

## 2026-09-19 ingest | texture resolution vs relief: measured
Raised `TexAtlas::kTexSize` 256 -> 512 (the only hardcode; mips/staging/blits
derive from it) because the shipped ambientCG sources are already 512 and were
being box-filtered down on upload. VRAM 5.9 -> 23.8 MB. A/B on hero 720p:
mean |diff| 0.37/255, Laplacian HF energy +4.8 % near meadow / +4.0 %
house+roof / +1.8 % mid-field. Conclusion: real but modest - a *sharpness*
fix, not a realism lever. Created [[concepts/texture-resolution]].

## 2026-09-19 ingest | texture detail normals (render flag bit 7)
`detailNormal()` in `common_base.glsl`: a 4-tap Sobel of the material albedo's
luminance over the same dominant-axis triplanar projection, turned into a
tangent-space perturbation (relief in metres, `kDetailRelief` 0.08). Wired at
all four shading call sites in both backends, applied after `detailAlbedo`.
Shading normal ONLY (oGNorm keeps the geometric normal, so SSAO is untouched
and SSR's water plane-stability holds); foliage skipped; no-op when untextured
so `VF_TEXTURES=0` stays bit-exact. First cut used a dimensionless gain of 0.5
which clamped ~51 deg tilts everywhere (embossing, +188 % HF); physical relief
scaling calibrated to +40 %. Bit 7 defaults ON (`m_renderFlags` 255), key B.
Created [[concepts/detail-normals]].

## 2026-09-19 ingest | texture conformance gate + AI texture repair
Built `tools/check_texture.py` (tileable <=1.35x, albedo-only <=0.35x,
no-stamp <=25 %, on-palette <=26), `tools/strip_stamp.py` (high-pass of the
blurred luminance to find the stamp, clone-from-roll repair) and
`tools/prepare_texture.py` (strip -> flatten -> seamless). Applied to 13 AI
candidates from the user's Downloads: 1 passed raw, 10 after repair. Findings:
all 7 Microsoft Copilot images carry a top-right watermark (x~860-1015,
y~0-58 on 1024^2); Gemini images do not; one Gemini image was pre-tiled at an
exact 512^2 period. 10 textures installed to `assets/textures/` as
`ai_*.png` and verified to load (1024^2 -> 512 resample path).
Created [[concepts/texture-conformance]].

## 2026-09-19 ingest | volumetric fog rewrite (J)
Three bugs in `volumetric_fog.comp`: `heightAt()` stubbed to 0.0 and the march
ran a FIXED 16 m from the camera regardless of the surface (darkened ~42 % of
the hero frame below luma 20); `dot(-rd, sunDir)` sign-inverted for forward
scatter; duplicated dead `skyColor`. Rewrite marches only to the G-buffer hit
distance (gpos IS the occluder, no heightfield needed), uses the correct
sign + Henyey-Greenstein Mie, bounded Beer-Lambert extinction, world-locked
fbm patchiness, and a sky-ambient scatter term that makes it aerial
perspective instead of a dimming filter. Measured: +3.1 % mean luma, 0 % new
pixels below luma 20, distant darks 55.3 -> 61.7, toward-sun vs away 4.2x.
Added `tests/fog_check.py` (CTest `fog_check`). Created
[[concepts/volumetric-fog]].

## 2026-09-19 ingest | water caustics + shoreline detail content
Two additive quality passes on top of the shading work.

**Water caustics**: `causticAt()` in `common_base.glsl` (two crossed drifting
sine grids, pow-sharpened) wired into both backends. Key finding: the
submerged bed is invisible past ~0.7 m because `outA = clamp(0.35 +
depth*0.9, 0, 0.97)` saturates at 0.97 — bed-only caustics changed 1.7 % of
the water frame even at 10x strength, while compositing the same term at the
WATER SURFACE changed 10.6 % at 0.85 strength (+1.98 % mean luma, no
clipping). Created [[concepts/water-caustics]].

**Shoreline reeds**: two new runtime-authored layers (`hamlet_reeds`,
`hamlet_reeds_far`, 5086 voxels) placed from `assets/heightmap.png` in the
-1.5..-0.42 m margin. Measured 4.11 % of hero pixels changed, HF +2.19 %.
Two lessons: an olive colour blended into the terrain (and classified as `.`
in ascii_view, so it was unverifiable) — saturated green fixed both; and
`live_edit_check` carves at world (0.05, 2.05)/(0.05, 3.05), where reeds
shifted the carved-water mean 22 codes in blue and failed the colour-parity
guard, so the layers now exclude world x -3..3, z -1..6.
Created [[concepts/world-detail-content]].

**Phase 5 (5 cm lattice) assessed and rejected**: measured 3.31 M surfels /
265 MB, SVO 144.9 MB, load 15.1 s at 10 cm. The 4x scaling gives ~1.11 GB
surfels + ~0.58 GB SVO (gate +1 GB) and ~120 s load, which would make the
test suite unusable. Prerequisite is the component-batching load-time fix.
Verdict recorded in [[concepts/detail-pipeline]].
Full suite green after all of it: 6/6 (unit/visual/live-edit/ssao/texture/fog).

## 2026-09-20 ingest | procedural tree generator (space colonization)
`tools/tree_gen.cpp` (target `vf_trees`, explicit tool, never in the default
build) generates the hamlet stand from a growth simulation rather than
prescribed shapes: space colonization (Runions/Lane/Prusinkiewicz 2007) with
pipe-model radii (r ~ leaves^(1/2.49)), an explicit curved/leaning trunk, then
crown colonization; per-tree parameter variation so the stand is not clones;
leaf clusters at tips and along the outer branches. Deterministic per seed.

Two hard-won findings. (1) **Surface-only emission is wrong for this loader**:
a 1-voxel shell of a rasterised cylinder is not watertight, VoxelField's
flood-fill leaks, and the trunk comes back HOLLOW in both backends (probe
across it reads solid/empty/solid). The generator now emits the full solid
volume. (2) **Bark via the per-cell texture override**: `wood_bark.png` (CC0,
fetched long ago but never bound) is bound to spare atlas slot 9 and the wood
voxels carry `tex=9`, so trunks get bark while the buildings keep material 6's
planks. Close-up check (vision): trunk solid, round, high-detail.

Also fixed: a crease-conditional object shrink I had added made VF_ANISO change
COVERAGE, not just footprint shape (the non-anisotropic build computed a
different radius) - reverted; and tests/test_surfelize.cpp now asserts the
three footprint regimes explicitly (crease / thin side / cap).
Full unit suite 76/76.

## [2026-09-20] ingest | mesh-to-voxel converter
Added [[entities/mesh-to-voxel]]: STL/OBJ import as a .vxw object layer. CLI
(`vf_mesh2vox`) + MCP `import_mesh` share mesh_voxel.hpp (13-axis box/triangle
shell + exterior flood-fill solid) and mesh_import.hpp (STL/OBJ parse + unit
transform). Full solid fill by default - a rasterised 1-voxel shell leaks
VoxelField's flood-fill and renders hollow (the tree_gen quirk); conservative
voxelization inflates the solid by up to one voxel per axis, and placement
keys on the solid AABB so the base lands on the anchor. 4 unit tests in
test_authoring.cpp (11^3 cube, shell-only, leak report, placement bounds).
Note: a second concurrent opencode session is editing src/ in this workspace
(kPalette 17->21 mid-session broke test_worldfile.cpp:369 - pre-existing).


## [2026-09-25] ingest | exact selected-layer rotation + editor dashboard
Reworked [[concepts/layer-placement]] around provenance rather than scene
overlap: stable 8-bit owner IDs survive reloads, Ctrl+LMB reads the winning
`VoxelField` cell owner, and the packed 80-byte surfel carries that ID through
base/micro and full/live paths. Forward, GPU-cull, and tile paths now share the
same `Rnew * transpose(Rold)` transform around the canonical bottom-center
pivot, while preview-time CPU culling is bypassed. The GUI is now a dense dark
dashboard with a searchable World Layers inspector, exact transform editor,
focused mode toolbox, on-demand Materials/AI windows, and selected-layer Rotate
action. `visual_check.py` adds before/after owner masks (green selected moves;
red other objects stay fixed).

## [2026-09-25] fix | preview hook cannot commit; pitch/roll merge parity
`VF_TEST_ROTATE_LIVE` now stays synthetic across frames: it is excluded from
interactive arcball accumulation and the mouse-release `commitRotation()` path;
a preview shot leaves `assets/world.json` byte-identical. `readLayered()` also
now checks `rotX`/`rotZ`, not only yaw, with zero-yaw pitch/roll regression
coverage. Forward GPU-cull, `VF_NO_GPU_CULL`, tile (confirmed via `VF_TRACE`),
and micro-on/off selected-layer renders completed without manifest mutation.

## [2026-09-25] lint | rotation/provenance wiki current
Verified [[concepts/layer-placement]] frontmatter, all 14 sourceRefs, both
cross-links, and its inbound catalog link. No stale object-bit/AABB-selection
wording remains in the maintained rotation docs.

## [2026-09-25] fix | compact dual-panel HUD at 960x540
A HUD-inclusive capture with Rotate + World Layers open reproduced a
SIGSEGV in ImGui: the clipped `BrushModes` BeginTable returned false but
its TableSetColumnIndex calls still ran. The table body is now guarded,
rows advance uniformly, and compact first-use heights keep both right-
side workspaces on-screen. Clean 960x540 capture and manifest hash pass.

## [2026-09-25] fix | click-activated bounds-centered rotation trackball
Rotate now uses a distinct workflow: one plain LMB click activates the exact
picked `.vxw` owner, and a later click-drag on the outer yaw, wide pitch, or
tall roll ring rotates only that layer. The ring surface is centered on the
placed AABB and sized from its projected bounds; it remains active after
commit. The screen projection was corrected to use the same view-space depth
math as `splat.vert` (the previous normalized-direction projection put the
gizmo off-object), and ring hit-testing runs before ImGui mouse capture so an
overlapping Toolbox cannot block it. Entering Rotate hides World Layers.
`tests/visual_check.py` now checks the projected centre and all three ring hit
classes in the ownership renders; the manifest remains byte-identical.

## [2026-09-25] fix | logical input space and panel-safe ring capture
GLFW cursor coordinates and ImGui mouse/draw coordinates are now kept in
logical `DisplaySize` space for both ray picking and trackball projection, so
high-DPI windows cannot split the object from its rings. When a ring overlaps
an ImGui window, the window is rendered with `NoInputs` under the pointer;
the ring therefore consumes the drag without toggling a button underneath.

## [2026-09-25] verification | final trackball gates
Final `ninja -C build`, `visual_check.py`, `--selftest`, the HUD-inclusive
trackball capture, and the complete `ctest --test-dir build
--output-on-failure` run all pass (6/6). The ownership probe reports the
selected cabin centre/radius inside the viewport and ring classes
yaw/pitch/roll/centre = 1/2/3/0; selected motion is 0.44% while other object
motion is 0.00%, and `assets/world.json` remains byte-identical.

## [2026-09-25] fix | camera-aligned gizmo and independent projection oracle
Camera movement now happens before picking, trackball projection, and render
push construction, so the ring cannot lag a moving view. The visual probe logs
raw AABB/camera data; `visual_check.py` independently reproduces the projection
math, checks centre/radius within 0.75 px, verifies all three ring hit classes,
and bounds terrain/water ownership motion. This closes the self-referential
probe gap and guards against terrain movement regressions.

## [2026-09-25] fix | local-axis ring rotation
The three handles now compose rotations on the object's own local axes
(`Rnext = Rcurrent * Rlocal`) rather than adding fixed Euler increments. The
outer/wide/tall rings are labeled local Y/X/Z, and ring distance scoring uses
mean pixel scale so the inner ellipses remain independently clickable.
`placementEuler()`/`rotatePlacementLocal()` round-trip through the existing
manifest convention, with unit coverage in `test_worldfile.cpp`.

## [2026-09-25] fix | staged transforms and Move mode
Ring release now leaves the local-axis pose staged in the GPU preview; the
Toolbox/Dashboard Apply button is the only interactive commit path, with Cancel
discarding it. Move mode grabs the exact picked owner, constrains drag to a
selected world X/Y/Z axis, draws an axis handle, and stages `pos` until Apply
move. Terrain, water, other owners, and unowned live geometry remain outside
both transforms. The Move gizmo now draws and hit-tests all three colored
world-axis handles in screen space, so Y and Z are directly selectable in the
scene. `VF_TEST_MOVE_PROBE=1` asserts the three handle hit classes are 1/2/3.
Escape no longer sets the window-close flag; only the window close button quits.

## [2026-09-25] perf | short smoke test profile
`ninja -C build test-fast` (or the opt-in fast CTest environment) now runs
focused camera/placement doctests, one live splat edit, and one canonical
visual/ownership check in about one minute. The opt-in fast profile also adds
reduced atlas, SSAO, and fog variants; the unlabeled full CTest suite remains
the exhaustive release gate.

## [2026-09-25] verify | final staged-transform and smoke-profile gates
The selected-owner transform now carries both local-axis rotation and the
optional Move translation through forward, GPU-cull, and tile shaders. The
clean exhaustive release gate passed 6/6 (`unit_tests`, `visual_check`,
`live_edit_check`, `ssao_check`, `texture_check`, `fog_check`); the opt-in smoke
profile passed 4/4 in about one minute. A prior texture failure was caused by
an executable relink race and passed when rerun in a quiet workspace.

## [2026-09-25] ingest | edge-aware surfel radius GUI control
Added `SurfelParams::edgeShrink`, a normal-disagreement radius reduction for
opaque object roof edges, corners, and thin spikes. The Dashboard's
**Rendering → Edge shrink** slider and `VF_EDGE_SHRINK` launch override feed
both the full VoxelField bake and the live ChunkStore path; releasing the
slider queues the normal surfel/world rebuild. Added a focused surfel test and
confirmed the fast smoke profile passes 4/4.

## [2026-09-25] verify | edge-shrink release gate
The first full CTest attempt hit the known transient `visual_check` batch-render
failure during a concurrent relink; isolated rerun passed, followed by a clean
full gate: 6/6 full tests passed (unit, visual, live edit, SSAO, texture, fog).
`git diff --check` and the focused edge-shrink surfel test also pass.

## [2026-09-25] fix | edge-shrink live-path anisotropy guard
The live candidate pass now computes edge metrics even when `VF_ANISO=0`, but
skips the thin-footprint probe in that mode, matching the full bake and avoiding
unnecessary live-edit work.

## [2026-09-25] fix | explicit multi-axis roof-edge detection
The edge metric now treats a voxel exposing multiple lattice axes as a full
sharp edge, so averaged 45-degree roof-eave normals receive the intended
shrink alongside the existing neighbour-normal curvature term. Opposite faces
of a thin plate share one axis and are not treated as an edge; the live store
path uses the same test.

## [2026-09-25] verify | refined edge metric fast gate
After the multi-axis refinement, `ninja -C build` completed, the focused
`*edge shrink*` surfel test passed, and `ninja -C build test-fast` passed 4/4.
The exhaustive CTest rerun is the remaining release sign-off.

## [2026-09-25] decision | focused test groups replace all-tests workflow
CTest entries are now guarded by `tests/group_gate.py`; CMake exposes focused
`test-<group>` targets and there is no all-tests target. The workflow selects
only the smallest relevant group (for the surfel change, `test-surfel` passed
4/4), and bare CTest is intentionally non-executing.

## [2026-09-25] verify | focused CTest group wiring
CMake reconfigured cleanly; CTest discovery shows the expected surfel entries,
the gate returns 77 when disabled and 0 only for an enabled group, and
`ninja -C build test-surfel` passed 4/4. `git diff --check` and Python gate
syntax validation pass.

## [2026-09-25] ingest | GUI STL/OBJ importer + cabin reimport
Added the Dashboard/World Layers **Import STL / OBJ** workspace in
[[entities/mesh-to-voxel]]. It shares the parser/transform/voxelizer through
`convertMeshToRecords()`, scans `assets/models/`, exposes fit/scale/axis/
winding/material/solid/anchor controls, and replaces a named layer without
editing its manifest pose. Added `VF_TEST_MESH_IMPORT` as a headless driver
for the same `App::importMeshFromGui()` path. Reimported
`Forrest_Hunting_Cabin.stl` as `hamlet_cabin` with fit 5 m, material 6,
anchor `[428,507,676]`; the 18,352-record layer and the existing
`pos=[0,1.65,0]`, `rot=-113`, `rotX=-89.8`, `rotZ=-63` orientation were
preserved. Added an OBJ end-to-end unit test.

## [2026-09-25] harden | shared mesh conversion + OBJ material paths
Moved CLI and MCP onto `convertMeshToRecords()` so path resolution, fit,
solid/shell semantics, statistics, and placement stay identical to the GUI.
OBJ MTL lookup now starts beside the source OBJ, handles `usemtl` before
`mtllib`, and rejects out-of-range face indices. The GUI refuses to overwrite
landscape or `ai_edits`, and the manifest upsert comment makes the preserved
`pos/rot/rotX/rotZ` replacement contract explicit.

## [2026-09-25] lint | pre-existing missing wiki target
A link check found the older `[[concepts/water-flooding]]` reference in this log
has no corresponding page. It predates the mesh-import work and was left
unchanged rather than inventing unrelated documentation.

## [2026-09-25] fix | MCP import schema JSON
The `import_mesh` schema now advertises its `mat` property. A hand-written
string concatenation briefly emitted a doubled comma; `tools/list` was parsed
with Python JSON validation after the fix.

## [2026-09-25] fix | deterministic GUI mesh-import test hook
`VF_TEST_MESH_IMPORT` now temporarily forces `VF_SYNC_RELOAD` and applies the
rebuilt world before the first frame; the old async request could let a
headless shot render the pre-import field while the rebuild thread was still
working.

## [2026-09-25] fix | surfel-range API compatibility during concurrent edit
The live surfel path was being migrated to `SurfelRange` with separate edge
segments. The app seed callback now returns that range and passes
`set.edgeStart` into `SplatPass::setSurfels`, keeping the GUI build compatible
with the current renderer API without changing the mesh import contract.

## [2026-09-25] verify | final mesh GUI and cabin gates
`ninja -C build` passed; focused `test-world` passed 2/2 and `test-visual`
passed 3/3. The OBJ/mesh filter passed 5 tests with 5,251 assertions. The
synchronous `VF_TEST_MESH_IMPORT` smoke path imported 57,520 triangles into
18,352 records, returned 0, and left the cabin manifest hash and pose
unchanged: `pos=[0,1.65,0]`, `rot=-113`, `rotX=-89.8`, `rotZ=-63`.

## [2026-09-25] ingest | hard-edge-only surfel fit + crease bridges
Normal disagreement no longer shrinks opaque object coverage. Only parents
with two non-opposite exposed lattice faces tighten, and each face pair emits a
small tangent-aligned bridge on the true crease. Bridges inherit all baked
attributes without extra marches and live in an always-on `[base | edge |
micro]` segment. Full bake, LOD, GPU-seeded live editing, and overlay restore
preserve the split. The hamlet emitted 20,252 bridges (+1.8% total records,
+2.2% house-view pre-cull quads); focused surfel/store/live/visual gates pass.

## [2026-09-25] ingest | HUD sidebar (single editor panel)
The five floating ImGui windows (Dashboard, World Layers, Toolbox, Materials,
Mesh Import, AI Assistant) became one docked left sidebar: a 46 px icon rail
(ED/WL/RN/TX/IM/AI, `Ctrl+1..6`), a content pane showing one section, and a
fixed 52 px status footer. `Panel` + `m_panel` replaced three visibility
booleans and `ChatUi::m_visible`; `m_editActive` deliberately stayed tool-armed
state. Verified with an ImGui window-list probe (5 windows, all sidebar
children) and real captures at 1600×900 and 960×540. `test-visual` green.

## 2026-09-25 ingest | X11 input injection for GUI verification
Headless `--shot` renders offscreen, so the ImGui HUD is invisible to every
visual gate. Built `tools/vf_input.py` (python3-xlib XTEST + XWarpPointer) and
a mechanism test to drive the real window, then debugged it to green. The
failures were instructive and are now recorded as
[[concepts/x11-input-injection]]:

- `XQueryKeymap` returns 32 **bytes** (byte N = keycodes 8N..8N+7), so the
  natural `km[keycode]` probe always reads zero. XTEST keyboard injection
  works; the probe was wrong. Mouse buttons are not in the keymap at all.
- `Display.warp_pointer(x, y)` is *relative* — passing a window as the first
  arg raises `struct.error`. Absolute motion is `root.warp_pointer(x, y)`,
  which targets the root's parent (the root).
- `win.translate_coords(root, 0, 0)` returns the **negated** origin; use
  `root.translate_coords(win, 0, 0)`.
- Window lookup by WM_NAME is ambiguous: crashed runs leave stale windows, X
  recycles window ids, and concurrent test scripts spawn several
  `Voxelforge` windows at different sizes. Binding to a stale window captures
  the wrong pixels, so every diff reads ~0.000 and looks like dead input.
  Fixed with a pre-launch id snapshot passed as `exclude`; `_NET_WM_PID` is
  not a usable fallback (X reports a different PID namespace than the shell).
- ffmpeg `blend=difference,blackframe` returned identical numbers for every
  case, masking the real problem; diffs now use numpy on decoded pixels.
- The window exists long before the ~17.6 s world load finishes, so early
  captures are near-black and poison every later A/B. Gate on the app's
  `layered_world: load` log line, then wait for frame luma to stabilise.

Tools: `vf_input.py` (library), `test_inject_iso.py` (authoritative gate,
refuses to run alongside other renders), `test_inject.py` (fuller matrix),
`diag_xtest.py`, `diag_windows.py`.

## 2026-09-25 ingest | demo capture tooling
Added `tools/record_demo.py`: a six-keyframe tour of the default hamlet built
on the app's `--shotlist` (format `path camx camy camz tx ty tz`, `#`
comments, ONE world load for all shots — the reason `visual_check.py` pays
~17.6 s per backend rather than per frame). `--mode live` instead drives the
real window over XTEST and screen-captures it so the ImGui HUD is visible,
refusing to start when another Voxelforge process is alive.

Key constraint recorded in [[concepts/demo-capture]]: `--shot` reads back the
OFFSCREEN image, so the sidebar/HUD/brush tints are never in a headless
frame. For GUI work use the X11 path in [[concepts/x11-input-injection]];
for anything else prefer `--shotlist` plus `--animtime` for determinism.
Never run a capture while another `build/voxelforge` is running (shared
window title corrupts live capture; also GPU contention).

## [2026-09-25] fix | flush, horizontally resizable docked sidebar
The left editor dock is now fully opaque and visually flush: square,
borderless child surfaces share the dock background, so no scene bleed or
inset top/bottom bands remain. An 8 px right-edge grip supports horizontal
resizing; the width persists across sections and Tab collapse, clamps to the
current display, and double-click restores the responsive default. The frame
loop gives the grip priority over scene picking/stamping and trackball rings.
Verified with a real 960×540 XTEST drag (`sidebar width -> 634 px`),
double-click reset to 334 px, and pixel checks for a uniform `(9,14,23)`
surface at the top/left/bottom edges. `ninja -C build` passed. The focused
`test-visual` build and GPU selftest passed, while the existing headless
ownership/trackball probe checks currently fail independently of the HUD
(offscreen shots do not contain ImGui).

## [2026-09-25] ingest | smooth terrain edit brush
Added a seventh Edit toolbox mode, `Smooth`. `ChunkStore::makeSmoothEdits`
snapshots terrain column tops, applies a weighted eight-neighbour height
relaxation with radial falloff, emits terrain-owned Set edits for rises and
Clear edits for drops, and protects object columns. The batch reuses the live
store → surfels/SVO/height-texture → undo/overlay path; the sidebar exposes
Size/Strength and headless hooks accept `smooth` plus
`VF_SMOOTH_STRENGTH`. Added spike/pit/object-protection unit coverage and
verified a real headless edit on a rough terrain column. See
[[concepts/smooth-terrain-brush]] and [[entities/live-edit-brush]].

## 2026-09-25 lint | broken link in log.md
Wiki link check (all wiki links across 29 pages): the only two broken links are
`[[concepts/water-flooding]]` in `log.md`, referenced twice by an earlier
ingest. The page does not exist — that concept was later replaced by
[[concepts/water-plane]] (the one fixed-level water plane; there is no flood
machinery). Left as-is because `log.md` is append-only history and rewriting a
past entry would falsify it; the live pages are correct.

## 2026-09-25 update | input mechanism validated green; demo reel rendered
`tools/test_inject_iso.py` now PASSES end to end on a clear display:

    window 0x4a0000b (32, 64, 1280, 720)   idle noise 0.003
    focus before press: 0x4a0000b  (ours)
    W keymap: before=0 during=1; focus ours=True
    W-forward diff 0.723   look diff 0.556   (threshold 0.200)

The last failing piece was **focus**: `glfwGetKey` reads GLFW's internal
state, fed only by events delivered to the X input-focus window, so a burst
sent while focus had drifted set the global keymap and moved nothing.
Re-asserting `set_input_focus` immediately before each burst fixed it. Guard
hardening that came with it: select the new window **by size** (a DFS `hits[0]`
picked an 80x45 leftover), ignore windows whose owning pid is dead, and
require the display to be clear for 5 consecutive samples — a one-shot check
raced into the gap between `live_edit_check.py`'s sequential renders.

`tools/record_demo.py --mode shots` then rendered all 6 hamlet keyframes in
**22.2 s total** (one world load for the whole reel, vs ~17.6 s per naive
`--shot`). Vision check on `01_establish` and `06_overview`: real coastal
village, river, terrain and mountains, nothing broken. See
[[concepts/demo-capture]] and [[concepts/x11-input-injection]].

## [2026-09-25] fix | Smooth undo height-texture headroom
`App::finishStroke` now derives undo scan headroom from the recorded inverse
Set cells versus the post-stamp terrain tops. A lowering Smooth/Delete stroke
can restore a tall spike (and a multi-stamp drag can accumulate more drop than
one stamp), so the forward-only `riseCells` bound was insufficient; the new
bound keeps `patchHeightTexture` correct on undo as well.

## [2026-09-25] fix | Smooth reload and terrain-texture hardening
`patchHeightTexture` now ignores object-owned solids, overlay restore and Clear
use full-column scans instead of fixed headroom, and `applyWorldReload` flushes
pending overlay writes before re-adopting the store and invalidates stale
cell-state undo history. Smooth top lookup now handles contiguous live raises
past its fast search band; non-finite headless brush values are rejected.
Added a high-raise regression to `tests/test_store.cpp`.

## 2026-09-25 ingest | demo reel: hamlet keyframes + voxel/splat A/B variants
Expanded `tools/record_demo.py` from 6 shots to **10 hamlet-framed
keyframes** and added a **variant matrix** so one command renders the whole
feature story.

The first keyframe set was aimed at the world origin and framed empty terrain.
The hamlet is actually at **x 5-23, z 11-26**. Bounds measured with
`tools/vxw_dump.py --layer`: hall c(10.5,2.0,14.2), tower c(19.9,6.7,17.9)
(12.8 m tall), pier c(13.9,0.5,6.9), boat c(14.1,1.0,4.4), well c(13.9,1.6,12.0),
market c(17.4,1.9,15.1), garden c(6.0,1.6,20.5) via
`world = (cell - 512) * VOXEL`. The terrain also sweeps hard along that axis
(pier 19.7 m, hall 10.9 m, tower 2.4 m, market -3.3 m, garden -5.6 m) against a
water plane of y = -0.9, so the market/garden sit *below* the waterline —
camera heights must be per-shot, not one constant.

Variants (each = one app launch, since backend and splat knobs are read
per-process, not per-shot): `splats` (default), `voxel` (`--mode svo`),
`radius_min` / `radius_max` (`VF_SPLAT_RADIUS` 0.5 / 2.0 — the same
`m_radiusScale` the sidebar slider and `[`/`]` drive), `no_micro`
(`VF_MICRO=0 VF_LOD=0`). `--sheet` writes a per-shot contact sheet.

Rendered 50 frames (5 variants x 10 shots) at 960x540 in ~55 s total, one
world load per variant (~11 s each). Measured: splat-vs-voxel differs on
55-84 % of pixels; radius 0.5 vs default on 35-46 %. Vision check confirms the
radius effect is the expected one — 0.5 reads speckled with gaps between
disks, 2.0 filled-in and smoother. All 50 frames bright (mean 82-130).
See [[concepts/demo-capture]].

## 2026-09-25 ingest | Smooth generalises to object surfaces (surface-position relaxation)
The object branch of Smooth was a 3D six-neighbour majority filter; measured on
the `makePatternStore` 1x3 object spike it pruned the tip and the base and kept
the middle via the opposite-pair rule, leaving a floating voxel (and it could
dissolve a 45-degree wall). Replaced with a generalisation of the terrain path:
the picked cell's *one-sided* (solid/air) face picks the axis — counting
exposed faces is wrong, a rod is air on both sides of every axis — the thin
fallback (1-cell wall, SDF-gradient sign) fires only when exactly one axis is
open so a diagonal rod/staircase is left alone, and the footprint is the 2D
disk in the perpendicular plane. Each row's surface coordinate relaxes toward
the weighted mean of its 8 lateral rows, clamped to their min/max, and the move
is one contiguous `Clear`/`Set` span along the axis: a bump collapses whole in
one stamp, a notch fills, flat faces/walls/posts/staircase steps are fixed
points, a second stamp is empty. Ground stands in for a missing lateral surface
only when the whole 3x3 ring is bare *and* the run is a short grounded bump
(<= 4 cells), so tall posts and overhanging slab edges survive. Fills are
always `terrain = false` and a raise whose span hits terrain is rejected, so
`patchHeightTexture` and the water bed never see object cells.

Splat exactness: `LiveEditor::stamp` gained a refresh `margin`
(`kStampMargin = 3`, `kExactStampMargin = 12` = the store's `kLiveBand`);
`SmoothTerrainEdits::objectSurface` makes the app use the exact one, and
undo/overlay restore always do. Measured object-smooth stroke stamps 11-24 ms
(first stamp 44 ms incl. chunk seeding), inside the documented 5-25 ms band;
the stroke converges after ~3 stamps because repeats are no-ops.

Tests: three new/strengthened `tests/test_store.cpp` cases (grounded spike
collapses whole + idempotent; plate notch fills with object-owned Sets; flat
wall / tall post / middle staircase step produce no edits) and a full-gate
`[object_smooth]` pair in `tests/live_edit_check.py` (cabin surface cell
`557,523,607` from the hero camera, `VF_EDIT_DIAM=4.0`, asserts the
`smooth object:` log and a 5.3 % pixel diff). `check_pair` gained a `cam`
passthrough — it silently rendered the edited frame with the default close-up
camera, which reads as a 97 % diff. Focused suites: smooth 6/6, `chunk store:*`
24/24 (28,808 assertions), live 4/4, full `live_edit_check` PASSED. `test-store`
still fails only on the pre-existing `tests/test_surfelize.cpp:213` edge-bridge
assertion.

See [[concepts/smooth-terrain-brush]] and [[entities/live-edit-brush]].

## 2026-09-25 ingest | Add brush grows the surface along the picked normal
The **Add** edit tool now thickens whatever it is clicked on. Root cause: the
dome rasterizer bounded its cell loop with the *height on world Y* while the
shape test ran in a local frame whose axis is the picked surface normal. That
is only right for an up-facing surface: on a wall it clipped the footprint
(the plane across the axis) to `y >= base.y`, so the stamp covered nothing
below the picked cell and the wall only ever grew a bulge above the click.
Measured on the hamlet cabin's front wall (lattice 562,524,607, normal -Z)
with the default 2 m brush, cells / of those below the pick: 479/0,
1206/0, 1748/0 at depths 0.5/1.0/1.5 m before, against 1726/805, 3027/1408,
4612/2148 after. `makeDome` now projects the reach on `axisDir`
(`half_i = radius + reach*|axisDir_i|`, plus one cell of slack because
`toCell()` floors and the box edge lands exactly on the footprint cell), and
the profile changed from a half-ellipsoid to an **extruded footprint disk
closed by a fillet of `min(radius, depth)/2`** — a wall thickens over its
whole footprint and only the outer lip rounds, instead of bulging at the
middle. The two profile branches meet at `t = depth - c` (both give `radius`),
so the branch is not an fp coin flip: the intermediate "disk + hemispherical
cap" version was rejected because it is *discontinuous* for `depth < radius`
and put the coin flip on the base plane, where it dropped half the footprint
ring. `inBrushVolume` in `shaders/splat.frag` repeats the same profile so the
green hover tint is still exactly the CPU cell set; `live_edit_check` (303 s,
both profiles) passes, as does the new unit gate. A render-diff assertion was
considered and **rejected with numbers**: head-on and oblique before/after
frames differ by 2.50 % (old) vs 2.68 % (new) pixels, so pixels cannot
discriminate — the guard lives in `tests/test_editable.cpp` "add dome grows out
along the picked surface normal" (33 assertions: centre reach, footprint edge,
past the fillet, nothing behind the surface, shallow-slab case, world-bounds
case), with `test_authoring.cpp`'s raise case rewritten from "tapers to the
rim" to the plateau invariants. Filed as
[[concepts/oriented-brush-rasterizer]]; brush table updated in
[[entities/live-edit-brush]], `AGENTS.md` and the `voxelforge-dev` skill.

## 2026-09-25 ingest | tool-calling conventions
Recorded the harness rules that cost turns this session, in `AGENTS.md`
("Tooling conventions"), the `voxelforge-dev` skill (§1b) and quirk memory:
`edit` needs all three of `path`/`oldString`/`newString` (a missing `path`
fails schema validation identically on every retry); `read`/`grep`/`shell`/
`glob`/`edit` are top-level only and not callable from inside `execute`; JS
given to `execute` is parsed first, so an unescaped quote silently drops
every tool call in the block; measure actual values before theorising about
float behaviour; and after an edit `ninja` can report "no work to do" with a
stale object (a failing test naming a line that is not in the file means
`touch` + rebuild, not "fix" the code).

## 2026-09-25 ingest | per-voxel Add/Carve + a brush sized in voxels
The brush is now measured in **voxels** and its minimum is **1 voxel**, which is
a per-voxel sculpt mode: at that width Add and Carve skip the volume
rasterizers and emit exactly one cell via the new
`EditableWorld::makeSingleVoxel`. Carve removes the voxel under the cursor; Add
places the cell just outside the surface, one step along the normal's
*dominant* axis — the pick always lands on solid material, so adding the picked
cell would be a no-op, and `round(n / VOXEL)` is wrong because a smoothed
corner normal like (0.7, 0.7, 0) would step 7 cells on two axes. Two latent
bugs had to go first: `applyEditLive` clamped the radius to a 0.1 m floor (so
even a "1 voxel" brush rasterized 3 cells across) and a 1-voxel *volume* still
takes the neighbour along the normal — measured 2 cells for Add, 4 for Carve,
which is exactly what the new guard catches. `App::brushVoxels /
setBrushVoxels / setBrushDepthVoxels / quantiseBrush` keep the metres value on
the lattice and every `VF_EDIT_DIAM`/`VF_EDIT_DEPTH` override is snapped, so
`VF_EDIT_DIAM=0.1` is exactly 1 voxel; the sidebar Width is a `DragInt` in
voxels with the metre equivalent as a sub-line, the Depth slider is disabled at
1 voxel, `+`/`-` step by one voxel, the HUD prints "1 voxel", and drag stamping
now advances at most one stamp per voxel crossing. Second bug found on the way:
**the hover outline in `shaders/post.comp` was centred on `uSel.xyz`, not
`uHover.xyz`** — with nothing selected the outline was drawn at the world
origin (invisible), and with a selection it outlined the wrong cell, so the
voxel you were about to edit was never marked. Fixed, and the per-voxel preview
tints the exact target cell (a 1-voxel box) instead of a depth-extent volume.
Gates: `check_per_voxel` in `tests/live_edit_check.py` asserts the stamp log
says exactly `1 cells` (a cell count, not a pixel diff — one 0.1 m voxel is at
the render resolution limit); `tests/test_editable.cpp` "per-voxel add and
carve touch exactly one cell" pins placement, the corner normal and the lattice
clamp (29 assertions). Filed in [[entities/live-edit-brush]], `AGENTS.md` and
the `voxelforge-dev` skill.

## 2026-09-26 ingest | a click is one edit (per-voxel brush stamped two voxels)
Reported symptom: at 1-voxel width a single click sometimes added or deleted
**two** voxels. Cause was not the brush: `makeSingleVoxel` emits exactly one
cell (guarded by `check_per_voxel`), but the frame loop's stamp trigger is a
**distance** test with no notion of a click — the same press re-stamps as soon
as the hovered cell differs from the last stamped one, which a click can do
from ordinary cursor jitter or because the Add itself changed what the ray
hits. Fix: a second stamp now also requires the pointer to have travelled
`kDragTravelPx` (10 px). A stationary click is one edit however long the button
is held; the gate can only delay a stamp, never add one; `lmbEdge` sets
`doStamp` directly on press so no click is lost; drag-painting still runs at
frame rate. A time-based debounce was tried FIRST and rejected: it rate-limits
a real drag, and it still double-stamps a slow press whose re-pick landed on a
different cell — the one case that actually mattered. Travel has no hole.
Recorded on [[entities/live-edit-brush]]. Note for the next change here: the
trigger reads glfwGetMouseButton directly, so a click cannot be regression-
tested headlessly; confirm manually (click, then one Ctrl+Z restores).

## 2026-09-25 lint | frozen content names in visual_check (fixed at the class level)
`visual_check` and `fast_visual_check` were red while every shot-acceptance
check passed (hero 70.3 % / house 83.3 % / water 95.6 % coverage, black-in-
silhouette ≤ 0.20 %, sky probes ok). The failures were all stale **fixtures**,
not renderer regressions: the script froze a content name, `hamlet_cabin.vxw`,
which the hamlet re-author removed (`CabinPart1.vxw` + `hamlet_*`). The app
resolved it to layerId 0, so the trackball never armed and the section reported
five misleading messages (probe not emitted, no red, nothing moved).

The name was frozen **twice** — once in `VF_ROTATE_LAYER` for the ownership
masks, once in the 4th field of `VF_TEST_MOVE_LIVE` for the Move-handle probe
(visible only in the FAST profile) — so fixing one left the other red. Both now
come from `visual_check.ownership_layer()`, which resolves the subject from
`world.json` at runtime (first enabled non-landscape, non-`ai_edits` layer,
currently `CabinPart1.vxw`) and reports one clear failure if there is nothing
to select. `ninja -C build test-visual` is now 3/3 green.

The predicate keys on **`role == "object"`** (plus `enabled`), not on a filename:
role is the stable contract — landscape layers carry `role "landscape"` — and
most `hamlet_*` layers are disabled at any moment, so a `hamlet_*` or
`landscape.vxw` name test breaks again on the next re-author. The single name
excluded is `ai_edits.vxw`, the app's own live-edit layer
(`EditableWorld::kFileName`, a code constant rather than world content, and
excluded by the selection logic in `App::applyEditLive` too because it is not a
rotatable object). It mirrors the app's own "first enabled object layer" rule,
so test and product agree on what a rotatable object is. Verified against
synthetic manifests: the live one selects `CabinPart1.vxw`; a manifest with
only `hamlet_hall` enabled selects `hamlet_hall`; landscape-only fails with the
single clear message.

**Lesson: never freeze a content/asset name in a test — read it from the
manifest.** One stale fixture becomes N confusing reds that look exactly like a
regression, and grepping for the name is the fastest way to find *all* the
copies. Prefer a *stable contract* (a `role`, a schema field) over a content
name, and derive the path from the same env the app was launched with. Two
sibling failures remain and are *not* this class:
`test_picking.cpp` "rayPick hits terrain from the hero camera" and
`test_surfelize.cpp` `edgeStart[i] <= chunkRange[i]` (~440 assertions), both
proven independent of the live-edit work by rebuilding with only
`editable_world.cpp` reverted to HEAD.

## 2026-09-26 review | verified the live-edit-brush session's changes; corrected a stale brush page
Reviewed the one running session's work while it held `src/app/main.cpp`, read-only
(leasing a file another session is actively editing would block the only session making
progress). Its settled claims were re-derived from source rather than relayed — the
`edgeStart` story reached me second-hand through two sessions, so I read the producer.

**Verified correct.**
- `shaders/post.comp` hover outline now reads `uHover`, not `uSel`. Confirmed two
  separate branches survive (`uSel` at :109 for the selection, `uHover` at :123 for the
  faint preview), so the fix did not delete the selected-cell outline. Matches the
  AGENTS.md "keep the two feeds separate" rule.
- `tests/test_surfelize.cpp` `edgeStart` bracket: **confirmed a test bug, not a
  renderer bug**, by reading the two interleave blocks in `surfelize.cpp`. The producer
  writes `newEdgeStart[c]` = base-parent range end and then appends bridges before
  `newRange[c+1]`, so `chunkRange[c] <= edgeStart[c] <= microStart[c] <= chunkRange[c+1]`
  holds, with `edgeStart[kChunks] == microStart[kChunks] == total`. The old
  `edgeStart[i] <= chunkRange[i]` therefore contradicted the `>=` on the preceding line
  and was satisfiable only for a bridge-less chunk — failing ~440 times, once per chunk
  that *had* bridges. The last-index bound is also real: both arrays are `+1` with the
  last entry = total, not `chunkRange[i+1]`. The consumer in `splat_pass.cpp` clamps into
  `[start, end]`, so it would render a wrong sub-range rather than crash — which is
  exactly how a producer bug could hide here. Filed the contract on
  [[concepts/detail-pipeline]].
- `tests/test_picking.cpp` second case (`selects an object layer voxel by material`) is a
  genuine **strengthening**: it now exercises the *placed* pose (`pos` + `rot/rotX/rotZ`),
  asserts the pivot contract (`placedPivot == sourceBottomCenter + pos`), round-trips
  ownership (`h.layer == layerId(file)` → `layerFile(h.layer)`), and replaces a ±3 lattice
  window with the placed AABB. All four `LayeredWorld` accessors exist.
- The stamp gate's safety property: `doStamp` is initialised to `lmbEdge && editLmb` and
  the gate block only ever *sets* it true, so a press's first stamp is unconditional and
  the gate can only delay. `m_hasStamp` clears on release and on reload, and reload
  re-syncs `m_lmbWasDown` from the physical button so a held click across a reload cannot
  manufacture a fresh edge.

**Corrected on the page.** [[entities/live-edit-brush]] said travel is measured "since
the last [stamp]". It is measured as **net displacement from the PRESS point** — that is
the whole design rationale, and the old wording described a gate that would fail on a
wobbling hand. Also corrected 10 px → **14 px** (the constant moved after the 00:38 entry
was written), and recorded the consequence I found in the code: a **dead disc of radius
`kDragTravelPx`** — a genuine small circular drag paints nothing. Deliberate, but a real
limit, and the reason a time debounce must not come back.

**Findings raised with the owning session.**
- The gate has **no headless seam** (trigger reads `glfwGetMouseButton`), and
  `check_per_voxel` pins cell count per stamp, not click-vs-drag — so this is
  manual-verify-only and can regress with a fully green suite. That is also how 14 px
  shipped unchallenged.
- `test_picking.cpp` case 1 is now *named* "hits terrain" but accepts an object hit
  (`if (!h.object)` guards the up-normal check), so in the live world it may assert
  nothing about terrain. The `PickHit::object` guard is the right predicate, but the fix
  relaxed the assertion instead of re-aiming the ray at a known terrain column.
- Surfel count bound loosened 2.5M → 8M. Defensible (micro detail on + hamlet re-author
  legitimately exceeds 2.5M), but 8M is ~2.4× the expected value.
- The `VF_TRACE` present/acquire probe is **unverified in the firing direction**: a quiet
  default run cannot distinguish a healthy path from a path never reached.

**Superseded, not propagated.** The implementer session's log entry ended by blaming a
"pre-existing `tests/test_surfelize.cpp:213` edge-bridge assertion". Both that failure and
the picking one were **test-only** fixes by the live-edit-brush session, now resolved
(95/95 cases, 34,289,445/34,289,445 assertions, known-red list empty). Annotated rather
than copied forward.

**Deliberately NOT recorded.** The owning session is actively diagnosing a second,
*undiagnosed* report — Undo appearing to rewrite surfels in untouched parts of a chunk,
narrowed to the live-edit seed/splice path with **no root cause yet**. No page, no AGENTS
entry, no "known issue": a guess here would be the third wrong instrument in a day. It
belongs in the wiki only when a measured cause arrives. `log.md` remains a four-session
merge point, not one author's history.

Filed [[concepts/measurement-discipline]] — the confirm-the-instrument lesson, requested
by the tooling-docs session, which recurred five times in one session (hand-decoded PNG
disagreeing with `--probe`; ffmpeg missing `-y` returning a stale frame six times; a
`pgrep` wait-loop matching its own command line; a "bright" reading latched onto another
session's window; and the author's own capture guard reporting a false obstruction). The
load-bearing rule: **silence is not a clean bill of health** — a probe only ever seen
quiet is untested, not passing.

## 2026-09-26 lint | the uncommitted multi-author tree is now committed under a tooling-only message
Two commits landed at 00:50 while this review was in flight (author: the user, not a
session). `git status -- src tests shaders tools` is now **empty** — the entire
previously-uncommitted mixed-author tree is in git history.

**Attribution is now actively misleading, so do not do archaeology with `git log`.**
`75f4a03` is titled "Add input automation and voxel layer dumping tools" and its message
describes only `vf_input.py` (XTEST input simulation), `vf_tour.py` and `vxw_dump.py` —
but it also swept in the whole of `src/`, `shaders/` and `tests/`: the implementer's
object-surface Smooth work, the live-edit-brush session's per-voxel + Add-dome work, the
texture/detail pipeline, and the render-pass changes. Someone bisecting or reading that
message will conclude the wrong thing about what changed and why. `5ffe80c` then touched
`index.md` alone.

Consequences worth carrying:

- Every peer session reported "nothing committed, commit HOLD by multi-session
  consensus" as of this morning. That is now **stale** — the consensus hold was
  overridden. Sessions must not assume `git diff` still shows their work, and must not
  treat "it's uncommitted" as a safety property.
- **The wiki is now the authoritative record of what changed and why**, which is the
  whole point of this layer. When a commit message and a page disagree, the page wins.
- This is itself an instance of [[concepts/measurement-discipline]]: the commit message
  is an instrument for attribution, and it was never measured against what the commit
  contained.
- These entries were themselves committed inside the sweep, which is harmless but worth
  knowing if a diff of a wiki page looks like it came with a renderer change.

## 2026-09-26 ingest | the click gate's "no seam" is an in-app-only limit — XTEST already reaches it
Correcting my own review entry above, which recommended "an env-injected press point +
travel offset" for the click-vs-drag gate. The trigger reading `glfwGetMouseButton`
directly blocks a *unit* seam, but the repo already ships an **out-of-app** one:
`tools/vf_input.py` has `_send_button()` and `click(button=LMB, hold=0.08, settle=0.10)`
driving a real XTEST `ButtonPress`/`ButtonRelease`, which reaches the same GLFW state
([[concepts/x11-input-injection]]). So the gate is testable **today with no app change**,
and adding an env var first would be building a second seam next to a working one.

The discriminating assertion is the reported bug itself: `click(hold=2.0)` with no pointer
movement must still yield exactly one edit. That separates the travel gate from the
rejected time debounce (which fails it), so it is a real test rather than a snapshot of
current behaviour. Caveats: needs a real X display, so `test-visual`-shaped rather than a
ctest unit test; and the recipe is **unvalidated** — a proposed test, not a working one.
Filed on [[entities/live-edit-brush]].

## 2026-09-26 ingest | Undo/"hollow cabin" filed as UNDER INVESTIGATION; the real finding is a test coverage gap
The live-edit-brush session reported a second, **undiagnosed** defect — Undo appearing
to rewrite surfels in regions nobody touched (hollow cabin in splat mode) — and asked
explicitly that it be recorded as under investigation, never as a cause. Filed that way
on [[entities/live-edit-brush]], with a standing warning not to "fix" the named paths.

It also reported what it had **ruled out by reading**, which is the expensive half of a
search and worth keeping: `readChunkSurfels` clamps correctly to `microStart` and derives
`edgeCount` from `edgeStart`; the seed lambda's parents/edges split is arithmetically
consistent with that; `rebuildRun` assembles `[parents | edges | micros]` with a matching
`microStart`; `patchChunkSurfels` keeps `m_chunkCount`/`m_edgeStart`/`m_microStart`
consistent including after a relocation. Next session should not re-walk those.

**The durable finding is a coverage gap, not a mechanism.**
`tests/live_edit_check.py` only ever edits **terrain** — it has **no case that edits an
object chunk at all**. The cabin is object geometry, and the GPU-seed path's weakest
assumption is there: `keyFromSurfel` recovers a cell by stepping back `0.5 * VOXEL` along
the normal, sound for a base parent but questionable for an **edge bridge**, which sits on
a crease *between* two cells and has no single owning cell. That is a **hypothesis under
test**, explicitly not a finding, and the page says so. The next step is a test (a
headless repro on the cabin wall: baseline, one per-voxel add, undo, compare), not a code
change — terrain-only coverage is the reason this reached a user at all. Noted the gap on
[[concepts/focused-test-groups]] too, since that is the page a session reads when choosing
what to run.

Also filed the shared-resource hazard on [[concepts/measurement-discipline]]: other
sessions relink `build/voxelforge` and take the GPU/display without warning, so an
impossible-looking number is usually cross-session state — check `git status --short --
src/ shaders/` and `--selftest` before believing it. Sixth instance of the same lesson,
and the cheapest to guard against.

## 2026-09-26 lint | CORRECTION: the 8M surfel bound is not the live-edit session's
My review entry above listed "surfel count bound loosened 2.5M → 8M" as a finding
**against the live-edit-brush session**. That attribution is **wrong** and is
superseded here. Its only change in `tests/test_surfelize.cpp` was the two
`edgeStart` bracket lines; `<= 8000000u` predates that session. I inferred
authorship from a `git diff` against a commit three generations back — which
shows *that* a line is new since then, never *who* added it. Exactly the
unearned-inference pattern both of us have been calling out, committed by me.

The number may still deserve a decision — 8M is loose against today's ~1.37M
surfels — but it is **not that session's patch to tighten**, and tightening it on
an inherited hunch is the same error. Whoever set it should be identified first,
and the call should be made knowingly.

## 2026-09-26 ingest | dead-disc finding caused a revert; Undo bug now has a measured signature
Two updates, one of them a design reversal.

**(1) The dead-disc note reversed the click-gate design.** The net-from-press rule
is being **reverted to travel-since-last-stamp**. Kept on
[[entities/live-edit-brush]] as an in-flight block, because the *reason* is the
lesson and outlives the code: the held-click stack has two candidate causes —
hand jitter, and the dominant one, that **the Add changes what the ray hits** so
the next pick is the top face of the voxel just created. Net-from-press fixed
both but was specific to neither, trading a narrow breakage (jitter) for a wider
one (any genuine small drag). The stacking case has a signature jitter does not —
*the new pick is the cell the previous Add created* — so suppressing exactly that
is deterministic, keeps small drags, and needs **no threshold**. Accepted trade-off
to state to the user: a held click can still land one extra neighbour if the hand
moves the threshold distance while held. The class is narrowed, not eliminated.
Marked PENDING in the page because `main.cpp` still held the old form at last
check — the code is the truth, not the page.

**(2) The Undo/"hollow cabin" bug is now reproduced, with numbers.** Previously
filed as undiagnosed with no data; the owning session now has a headless repro on
the cabin wall (`562,524,607`): a 1-voxel add in an **object** chunk moves
0.18 % of pixels and the undo moves **2.57 %**, while the same test on a terrain
cell moves 0.02 %. The undo moves ~an order of magnitude more than the edit it
reverses — a corruption signature, measured. It also puts the earlier coverage-gap
finding on a number instead of an argument. Surfels-per-chunk before/after was
still being measured. **Still no cause**; the page says so and lists the four
things already ruled out by reading.

## 2026-09-26 ingest | Undo root cause MEASURED: a refresh-margin bug, and wider is not safer
The undiagnosed Undo/"hollow cabin" report is now **resolved with a measured cause**, so
the earlier "no cause yet" filing on [[entities/live-edit-brush]] is superseded by a
RESOLVED section.

**Root cause.** `undoEdit` forced `kExactStampMargin` (12 cells) on **every** undo,
because Smooth needs the exact band. So a **one-voxel Add was undone by re-deriving a
25^3 box**. On terrain that is merely wasteful; on an **object** chunk it is *wrong* —
the store's object surface/crease classification does not match the bake's, so the wide
re-derivation produced **1310 parents + 599 edge bridges inside that box against 52
bridges in the whole chunk**. Fix: the undo step records the margin the stroke used
(`m_undo.back().margin`) and replays with it; `kExactStampMargin` is now escalated only
on the Smooth object-surface path. Verified in the source, not just the report:
`undoEdit` reads `m_undo.back().margin` and passes it to `commitStoreEdits`.

**Measured on the cabin wall** (`562,524,607`), add-then-undo vs the untouched baseline:
pixels differing fell **2.571 % → 0.185 %**, and the run split returned to
**4298 + 52**, identical to the add-only state — undo is now a true inverse. The full
`test-live-edit` gate was still running when this was filed.

**The lesson worth more than the bug: a wider re-derivation is not a safer one.** The
instinct that a bigger refresh margin is more thorough is *wrong on object chunks* —
because the store-derived surface disagrees with the bake, widening the region does not
converge on the baked answer, it **diverges** from it. Any future path re-deriving
object geometry from the store over more than the edited region needs the same scrutiny.

Note where the bug actually was: the four paths everyone suspected
(`readChunkSurfels`' clamp, the seed lambda's split, `rebuildRun`, `patchChunkSurfels`)
were all correct. It was one layer up, in *which margin the caller asked for* — a good
argument for checking call arguments before auditing the callee.

Also re-synced the click-gate page, which had been left describing the reverted design:
the landed form is **two independent suppressions** — the stack refused by **identity**
(`m_hoverHit.voxel == m_lastStampWroteCell`, no threshold at all) plus a 6 px
travel-since-last-stamp gate for jitter only. `m_stampPressMouse` is gone, replaced by
`m_lastStampMouse` (refreshed per stamp). The 14 px constant was **deleted, not
retuned**, because only the jitter case is left to tune. Residual, as stated in the code:
a jittery click whose hand moves 6 px can still land one extra neighbour.

Picking coverage was restored by **adding a ray, not renaming**: a second ray straight
down from the hero position must be terrain (`object == false`), `normal.y > 0.9`, and
solid — so terrain direction is pinned unconditionally, and the case is renamed "rayPick
hits the hero camera, and terrain faces up" so the name no longer promises what the hero
ray stopped guaranteeing. 3/3 rayPick cases pass.

## 2026-09-26 lint | margin class now gated — but the gate can pass while testing nothing
`check_object_undo_surgical` landed in `tests/live_edit_check.py` and is wired into
the **full** path (not the fast profile). Verified in the source, including that it is
registered alongside `check_per_voxel` / `check_water_fill` / `check_undo_and_clear`
and not in the `FAST` branch.

It bounds the margin class on **two independent signals**, so neither can mask the
other: the run split must report ≤ 200 edge bridges (measured **52** healthy, **599**
with a wide margin — content-independent), and an undo must not move more than **0.5 %**
of pixels (measured **0.184 %** after the fix, **2.571 %** before). Thresholds are well
clear of the measured values, and the reasoning lives in the check's docstring so it
travels with the assertion — the right place for it, since a future editor will not read
this wiki first. Credit where due: the thresholds were taken from what the pre-fix run
actually produced, not from a guess.

**Correction to my own reading of the 0.184 %:** I initially could not tell whether a
non-zero post-undo diff meant undo was incomplete. It does not — touched chunks keep
live **store-derived shading** until the next full reload, so a small residual against
the untouched baseline is inherent. The invariant that actually proves undo is a true
inverse is the **run split returning to the bake's own 4298 + 52**, identical to the
add-only state. Recorded, because "0.184 % ≠ 0" invites exactly the wrong conclusion.

**The weakness worth acting on.** The wall cell `562,524,607` is a hardcoded constant.
The robustness argument is correct as stated — it **cannot false-alarm** on a
re-authored hamlet — but that is only half the risk. It can also degrade **silently
into an inert check**: if the cell stops being solid/editable the add never happens,
both signals stay low, and the check passes having verified nothing. That is worse than
a false alarm, because a green check gets trusted. Two cheap closes, both offered to the
owning session: assert the stamp actually happened before asserting its outcome (the
in-file `check_per_voxel` "1 cells" pattern is the precedent), or resolve a real object
cell at runtime as `visual_check.ownership_layer()` already does. Filed on
[[entities/live-edit-brush]] and [[concepts/focused-test-groups]] — the latter's
coverage-gap note was corrected from "no case edits an object chunk" (now false) to
"found and closed, with this residual weakness".

The session also parked two honest gaps it is not doing unasked, and the reasons are
worth recording: a click-vs-drag injection seam (needs a mouse-position/LMB path into
GLFW input) and a forced-failure hook giving the `VF_TRACE` probe a positive firing
test. Both are real, neither is cheap, neither blocks a user-visible bug. Queued, not
discarded.

## 2026-09-26 lint | a frozen test constant has TWO vacuity paths, not one
Follow-up to the gate entry above. The owning session accepted the precondition
assert and, in doing so, named the residual I had missed — which is the more
interesting half of the record.

**"Cannot false-alarm" is only half of a robustness argument.** The other half is
that a check can pass **vacuously**, and that is worse: a false alarm gets
investigated, a vacuous pass gets trusted. Two distinct paths, needing different
fixes:

1. **Cell stops being solid/editable** → no add, both signals stay low, green
   having verified nothing. **Closed** by asserting the `live edit: N cells` log
   *before* judging any outcome — the `check_per_voxel` pattern, same file. No
   threshold changes.
2. **Cell drifts onto TERRAIN, still solid** → the stamp *succeeds*, so the
   precondition passes, and the run-split/pixel signals are terrain-small, so the
   check goes green **while testing the one class that provably cannot catch this
   bug** (0.02 % — the terrain number from the entry above). This is a weaker
   guard, not a disabled one, and the page states it as a limitation rather than
   leaving it a hidden assumption. **Open.**

**Path 2's cheap close, identified in the source:** the generic stamp log
(`live edit{what}: N cells, M chunks, R run surfels (P parents + E edges + U
micros), …`) carries counts and timings but **not the ownership class of the
picked cell** — only the Smooth path logs that (`smooth object:` /
`smooth terrain:`). The hover hit's `object`/`layer` is already in scope at that
call site, so adding one field to that existing line would let the check assert it
landed on an object cell, closing path 2 without the probe machinery. Offered as
the proportionate option; runtime object-cell resolution remains the thorough one
and was deliberately deferred. Noted on [[entities/live-edit-brush]].

Worth keeping from the same exchange: the edit was **held until the running gate
finished**, because that gate executes the file being changed — the discipline the
session had been asking others to follow all night, applied to itself. Editing a
file mid-gate would have produced a measurement of two different revisions.

## 2026-09-26 lint | my assertion caught the reviewer's own false premise — and the fix is better than mine
**I was wrong, and my suggested assertion is what proved it.** I proposed
asserting that the stamp landed on an **object** cell. It failed immediately:
cell `562,524,607` logs **`pick terrain`**. So "a solid cell on the hamlet cabin's
outer face" — a phrase I read in a check docstring and **repeated onto the wiki
page as fact** — was never true. A docstring is a claim, not an instrument, and I
filed it without measuring it. The "hollow cabin" reading was the reporter's
inference from a GUI view, not a measurement; the page now says what was actually
measured: the chunk's baked surfels were replaced by store-derived ones across a
region that *includes* object geometry, and **the corruption is chunk-level, not
at the picked cell**. The 2.571 % / 599-vs-52 numbers are untouched — they came
from the chunk's run split and a whole-frame diff, neither of which depends on the
picked cell being an object.

**The replacement assertion is stronger than the one I proposed**, and needs no
ownership knowledge at all: the **post-undo run split must EQUAL the add-only run
split** (4298 + 52 == 4298 + 52; the bug gives 4157 + 602). Threshold-free, immune
to content drift on re-author, and it closes the terrain-drift path I had called
the weak one — if post-undo equals add-only, undo is exact regardless of what was
picked. A lesson in review: my finding was directionally right and
mechanically wrong, and the thing that caught it was a concrete assertion rather
than more reading.

**A negative control found a weakness none of us reasoned our way to.** Running
the check against an **air** coordinate showed that a headless stamp happily
writes a voxel into open air, so a stale coordinate still logs `1 cells` and then
fails the comparisons **for the wrong reason** (it blames the undo: "1+1 vs
0+0"). Hence a fourth premise assert: the region must hold real baked geometry (a
3-digit parent count), so an air control fails naming the stale coordinate instead
of comparing two empty regions. Reasoning about whether a check can pass vacuously
did not find this; a deliberately broken input did. Recorded as the method note on
[[entities/live-edit-brush]] — audit tests with negative controls, not arguments.

The check is now four asserts (stamp happened, region holds real geometry,
post-undo run == add-only run) plus two secondaries (edges ≤ 200, diff ≤ 0.5 %).
The `pick` field stays in the log for diagnostics and is only **printed**, not
asserted. **Not filing the green** until the full gate reports.

Provenance quarantine: the 0.18 % figure on `concepts/focused-test-groups` is now
marked as "a one-voxel add on this cell" rather than an object-class number,
since the cell it came from logs a terrain pick. Someone should re-measure at a
cell that genuinely logs an object pick before that contrast is relied on.

I did not independently re-probe the cell: `--probe` would contend for the GPU
with a gate that is running, and a measurement taken across another session's run
is not a measurement.

## 2026-09-26 lint | RE-CORRECTION: the cell is object, the reader was broken, and a new log field is a new instrument
Supersedes the previous two entries on this bug. The sequence is the record, and it
is kept whole because the second wrong turn was more instructive than the first
right one.

**The cabin-wall premise was right.** Cell `562,524,607` logs **`pick object`** and
always did. The intermediate correction — that it was a terrain cell — was **wrong**,
and so was my publishing it.

**Root cause of the wrong turn: a broken *reader*, not a wrong world.** The headless
stroke hook built `m_hoverHit` by hand — `m_hoverHit = {}; hit = true; voxel = p;
normal = storeNormalAt(p)` — and never set `object`/`layer`, so a default-constructed
`PickHit` (`object = false`) made **every** headless run report terrain no matter what
it stamped. Verified in source. Fixed by `adoptPickOwnership()`, which resolves the
owner from the **load-time oracle** (`m_layers.field().sampleWorld`) and is called at
all three hook sites. Post-fix: `546,527,642` → object, `562,524,607` → object,
`432,509,452` → terrain with **0** edges against the object cell's **52**.

**My assertion was vindicated, and my correction was the error.** I proposed asserting
`pick object`; it "failed" for a reason that was itself a bug in the instrument
reading it. Filed on [[entities/live-edit-brush]] as a history note, with the cabin
framing restored and the three vacuity paths now closed.

**The terrain-drift path was real, and the control proves it.** Terrain cell
`432,509,452` passes the true-inverse comparison **silently** — `5875+0 == 5875+0`,
diff 0.011 % — and only the `pick object` premise fails it. So the true-inverse
assertion is *necessary but not sufficient*, which is the opposite of what I wrote
when I claimed it closed that path on its own. Three premise asserts now run before
any outcome: stamp happened, region holds real baked geometry, pick is an object
cell. Primary assertion unchanged and still threshold-free: post-undo run ==
add-only run (4298+52 == 4298+52 against 4157+602).

**The numbers never moved, and that is the transferable part.** 2.571 % and 599-vs-52
came from the chunk run split and a whole-frame pixel diff, neither of which ever
touched the pick hook. The broken instrument invalidated the **label** attached to the
measurement, not the measurement. A broken instrument does not always spoil the
number, and reflexively distrusting a number because a nearby instrument is broken
throws away good data — say precisely which measurement the broken instrument was
*in*. The 0.18 %-object vs 0.02 %-terrain contrast I had quarantined is established
and the quarantine is retracted.

**The lesson that actually generalises, now on
[[concepts/measurement-discipline]].** Not "check the instrument" — that was said all
night and did not prevent this, by the person citing it. The sharper form: **a NEW log
field is a NEW instrument, and its first reading deserves no more trust than any
other.** An uncalibrated field produces the least reliable data in the system and
gets read with the most confidence, because it is newest and appears to answer
exactly the question being asked; here that meant believing a one-hour-old field over
a world already measured and mapped. **Calibration needs a case where the new field
and an independent source must disagree** — two obviously-wood cells logging terrain
is what exposed the hook, and no amount of re-reading the code that produced it would
have, because that code was self-consistent.

**Not filing the green** until the gate reports.

## 2026-09-26 ingest | vf_slice was never broken: opposite failures, one cause — the mistake is in the reading
A retraction of the retraction, and the pair is the most useful thing this review
produced. **vf_slice is NOT broken.** Its `--axis z` plane was declared "blank" because
the output had been piped through `sed -n '4,26p'` — and those rows are **above the
terrain**, because the grid prints **row 0 as the top of the span with rows
descending** (50 of 102 rows carry content, including rock and soil). The tool
**announces this in its own header** (`cols: … asc, rows: … desc`,
`tools/scene_slice.cpp:77`) and again in the loop comment at `:85`. Verified in source.
The instrument told you; the reading missed it. Filed as a practical gotcha on
[[concepts/voxel-object-authoring]].

**Opposite failures, one cause.** The `pick` field read a plausible **constant**
(`terrain` everywhere, from a hook that never set it) and was believed over an
already-mapped world. `vf_slice` read as plausibly **blank** from a partial view.
Re-reading the producing code found neither fault, because in both cases the code was
self-consistent and correct — **both mistakes were in the reading, not the code**.

So the rule on [[concepts/measurement-discipline]] is no longer "check the
instrument", which was already on the page and demonstrably did not prevent either
error. It is: **before believing *or* condemning a reading, look at the WHOLE output
rather than a convenient slice, and cross-check against a source that cannot share
the failure mode.** The second half is what makes it a check and not a habit — the
world has a second way to ask what material is at a cell, so a disagreement costs one
command. A cross-check against something sharing the code or the author's assumptions
is not a cross-check. Corollary recorded: a `sed`/`head`/`tail` window over a tool's
output is itself a *new instrument* with its own failure modes, added casually.

**Watch item raised by the owning session, and it is not a log-only change.**
`adoptPickOwnership()` sets `object`/`layer` on **synthetic** headless picks where they
were previously `false`, and more than the log branches on that field:
`m_hoverHit.object` feeds the selected-owner label (`main.cpp:3001`,
`layerFile(m_hoverHit.layer)`) and the hover-owner path at `:3335`, plus
`m_lastPickObject` at `:1993`. So headless runs that previously took the *terrain*
branch now take the *object* branch — a real behaviour change in the rotate/move and
owner-label paths, not merely a better log line. `test-live-edit` is 3/3 green
(`unit_store_tests`, `live_edit_check`, `fast_live_edit_check`); `test-visual` is being
run before it is called done, which is the right gate for this class of change.
**Green not filed** from either side.

## 2026-09-26 lint | FINAL: both crossed groups green; I declined test-surfel, and here is the evidence for that
`test-live-edit` 3/3 and `test-visual` 3/3. Filed as green on
[[entities/live-edit-brush]] with the **coverage argument**, not just the tick marks.

**The group-selection call was delegated to me; I declined `test-surfel`, on
evidence rather than agreement.** The only edit outside `App` is the
`getenv("VF_TRACE")`-gated region log in `live_editor.cpp:154-160` — a log plus a
move of a temporary, which cannot change the run. The question that decides it is
not "is it inert" but **"has that code ever executed"**, and a guarded block that
no test enables is untested code regardless of how inert it looks. Checked:
`tests/live_edit_check.py` sets `VF_TRACE=1` in three places (lines 250, 254,
722), so those lines **do** run under the green live-edit group. The claim held
up, so the group is not needed. `test-surfel` would re-cover surfel extraction,
which neither change touches.

Worth generalising, because it is the same instrument question as everything else
tonight: **"this code cannot matter" and "this code has run" are different
claims, and only the second one is evidence.** A branch behind an env guard with
no test enabling it is the classic untested path, and it is invisible in a
coverage number that counts lines rather than paths taken.

**One coupling now recorded:** the check parses the run split out of that
`VF_TRACE` diagnostic line, so the gate depends on a **log** existing. The failure
direction is right — a missing or reworded line fails loudly ("no run-split log to
bound the refresh") rather than passing quietly — but a cosmetic edit to that
`spdlog` line is now a test change. That is a fair trade for failing loudly, and it
should be a conscious one.

**Final state of the branch.** Undo replays the margin the stroke used; the click
rule suppresses the stack by cell identity and jitter by 6 px of travel since the
last stamp; the canary is gated by a threshold-free true-inverse comparison behind
three premise asserts, with an air-cell and a terrain-cell negative control both
failing loudly and correctly. Two gaps remain, and remain honestly gaps rather than
resolved-by-omission: **no headless seam for click-vs-drag**, and **no
forced-failure hook for the `VF_TRACE` present/acquire probe** — the second is
also what would give the `live_editor.cpp` guard a *positive* firing test, so it
earns its place twice.

## 2026-09-26 ingest | the forced-failure test found a real bug; and "never taken" beats "behind an unset guard"
The queued forced-failure hook landed (`VF_TEST_FORCE_PRESENT_ERR`, documented in
AGENTS.md) and immediately earned its place twice.

**It found a live bug in the existing probe.** The log-once latch remembered only
the **last** code, so it meant "log whenever the code *changes*", not the
"log-once per distinct code" its comment claimed. Two failures **alternating**
produced 60 lines in 30 frames — exactly the flood the latch exists to prevent. It
survived review because a real *persistent* failure repeats one code and so behaves
correctly: the bug is invisible to the only failure mode anyone reasoned about.
Now a bounded, saturating per-slot set (`seen[2][8]`, `live_editor`-independent, in
`main.cpp:1072`), with `check_present_probe` asserting both severity
(device-lost / out-of-host-memory at `error`, rest at `warning`) and one line per
distinct code, using a **repeated code in the list** to make the latch observable.
Verified in source. This is the whole *silence is not a clean bill of health* thread
paying out: the one diagnostic nobody could test turned out to be broken.

**My principle, corrected upward — and the correction is the valuable part.** I had
written that "a path behind an unset guard is invisible to coverage". Stronger and
worse: the hook was first placed beside `vkQueuePresentKHR` and **fired zero times
while looking perfectly correct in source**, because a headless render never
acquires or presents the swapchain at all — it submits its command buffer and reads
back the **offscreen** image. So the lines were **covered** (they compile, they are in
the binary, coverage counts them) while the code **never executed**. No coverage
number shows that and no source review finds it.
**"Has this run?" has to mean "can it run in the configuration the tests use?"** —
a strictly stronger question than whether a guard is set. Filed on
[[concepts/measurement-discipline]] with the fix pattern: give a windowed-only
diagnostic a synthetic driver **and call it from the headless frame body**, not only
at the real call site.

**Also filed — cheapest lesson of the night, and it cost a build.** A scripted
string-replace patch **silently did nothing** because the anchor had wrong leading
whitespace and nothing complained. The compiler caught it by accident; a docstring or
log-line edit has no compiler at all. Rule: assert the replacement happened (count
before and after). A no-op edit looks exactly like success.

**One stale comment found in review, for the owning session.**
`check_present_probe`'s docstring still says *"Needs `--smoke`, not `--shot` … so the
probe is only reachable from the real frame loop."* That is the **pre-fix** reasoning
and it is wrong twice over: `forceFrameResults()` is called from the **headless** frame
body (right after `headless submitted`, `main.cpp:5367`), so the probe is reachable
from headless *because* of the fix, and `--shot` shares that same headless path so it
reaches it too. Left as-is, that comment invites the next person to move the hook
back. Filed on [[concepts/focused-test-groups]].

**Unchanged honest gap:** no headless seam for click-vs-drag — still the only way that
class of bug gets caught without a human clicking.

## 2026-09-26 ingest | paired lesson: the expensive silent failures are the ones with no compiler behind them
Supersedes the "stale comment to fix" finding in the entry above — the docstring is
fixed and `test-visual` is 3/3 on it. What is worth keeping is *why it was wrong in a
way that mattered*, and it is the same failure as the no-op edit with a different
trigger.

**A comment that records _why_ is a claim with an expiry date.** "X is unreachable
from every test, so the hook lives here" was true when written; a later change made
it false; the stale sentence then read as a **live constraint** and would have
invited the next person to move the hook back to `vkQueuePresentKHR` and re-break it.
Nobody flagged it because **reading a comment feels like reading code** — prose has no
type checker, and a justification in the present tense will eventually be a lie that
gets believed. Hence the rule now on [[concepts/measurement-discipline]], covering
both cases: **the expensive silent failures are the ones with no compiler behind them
— a no-op edit and a stale comment are both silent and both look exactly like success,
so the defence is the same for each: _assert it_, or _date it_.** Either state the
invariant in the present tense ("reachable **because** the headless body calls it") or
mark it as history ("the first version shipped this way, firing zero times").

**A second-order trap in the same family, worth keeping separately.** The justification
had quietly changed *identity* rather than just truth value: the check kept using
`--smoke` for a reason that was no longer reachability (both headless modes reach the
probe; `--shot` measured 1 line, correctly latched) but **statistical power** — over
`--shot`'s three frames the latch would pass *even if it were broken*. Switching to
`--shot` would look identical in the source and silently destroy the property the
check exists to pin. **Merging two live justifications into one sentence is how the
stale version survives**, because a reader can only check the sentence, not the two
reasons behind it. Verified the corrected docstring now separates them and attributes
the reachability half to the bug history.

That closes this thread. The one honest gap is unchanged: no headless seam for
click-vs-drag, which remains the only way that class gets caught without a human
clicking.
