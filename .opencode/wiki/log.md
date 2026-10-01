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

## 2026-09-26 lint | src/app was split per subsystem: every main.cpp line number in this wiki is now stale
A `refactorer` session split `src/app/main.cpp` (was ~5470 lines, **now 15**) into
per-subsystem files plus a shared `src/app/app.hpp` (499 lines):

```
src/app/cli/       args.{cpp,hpp}
src/app/rhi/       present_probe.{cpp,hpp} surface.cpp
src/app/world/     store_overlay surfel_stream world_layers world_textures
src/app/textures/  texture_bindings.cpp
src/app/edit/      live_edit.cpp transform.cpp
src/app/mesh/      mesh_import.cpp
src/app/ui/        sidebar, panel_{ai,edit,mesh,render,textures,world}, gizmo_math,
                   ui_primitives, scene_overlays, theme, ui_types
src/app/frame/     run.cpp run_input run_hooks run_hotkeys run_startup run_capture
                   run_brush_preview profiler record_fx selftest frame.hpp
```

> **SUPERSEDED — see the correction entry below.** This entry documented the
> new layout as "verified, not guessed" and listed single call sites. Both were
> wrong: the tree **does not compile at this moment** (`run.cpp` is mid-rewrite,
> stages 2+3 in flight), and the call-site lists were incomplete. Kept unedited
> below as the record of the mistake.

**Consequence for this wiki: every `main.cpp:<line>` reference filed during the
review is now wrong**, and a stale line number is worse than no line number because
it looks checkable. Where the symbols documented here went (verified, not guessed):

| symbol | new home |
|---|---|
| `firstSight`, `reportFrameResult`, `forceFrameResults` | `src/app/rhi/present_probe.cpp` |
| `adoptPickOwnership` | `src/app/edit/live_edit.cpp` (called from `frame/run_hooks.cpp`) |
| `undoEdit` | `src/app/edit/live_edit.cpp` (called from `frame/run_hotkeys.cpp`) |
| `kDragTravelPx`, `m_lastStampWroteCell` (click gate) | `src/app/frame/run_input.cpp` (constant also in `ui/ui_types.hpp`) |
| `m_lastPickObject`, hover-owner label | `src/app/frame/run.cpp`, `src/app/ui/` |

**The fix is to stop citing line numbers at all.** This is the same lesson as the
stale-docstring thread one entry up, in a different costume: a line number is a claim
with an expiry date and a symbol name is not, so the refactor is the moment to convert
every anchor on [[entities/live-edit-brush]] and [[concepts/focused-test-groups]] to
symbols. Deliberately **not** done speculatively while the split is still being
verified against reference renders — the layout may yet move, and a confidently
re-anchored page is worse than one that admits it is mid-migration. Those pages carry a
marker until the refactorer reports the split settled.

**What I owe the refactorer: nothing.** This session made **zero** edits outside
`.opencode/wiki/**` — no `src/`, no `tests/`, no `CMakeLists.txt`, no `AGENTS.md`, no
relink. Their collision checklist (`tests/` is theirs for new files, `CMakeLists.txt`
has an explicit source list with no GLOB, do not relink `build/voxelforge` mid-gate)
is noted and respected.

## 2026-09-26 lint | CORRECTION: I documented a mid-flight layout as verified, and dropped call sites I already had
Correcting the entry above, which made two mistakes worth keeping.

**(1) I filed a moving target as fact.** The refactorer reports the tree **does not
compile right now** — stage 1 (members out of `main.cpp`) is done and *verified*
(builds, `--selftest` PASSED, hero render inside the original binary's own run-to-run
noise floor, p50 delta 0.004–0.022 against a 0.03 floor), but stages 2+3 are splitting
`App::run` into per-frame slices and `run.cpp` is **not yet rewritten**. The page
marker I added therefore said the split "was" done and handed out new file locations as
current. It is intent, not settled fact. The refactorer's own words are the right
standard: a wrong-but-plausible location is worse than a flagged one.

**(2) I compressed a multi-file grep into one "primary" caller and lost data I
already had.** My grep returned `adoptPickOwnership` in **five** files and `undoEdit`
in **seven**; I wrote "called from `frame/run_hooks.cpp`" and "called from
`frame/run_hotkeys.cpp`" respectively. The missing call sites were *in my own output*
and I dropped them. Corrected counts: `adoptPickOwnership` has **three** call sites
(`VF_TEST_EDIT` and `VF_TEST_STROKE` startup hooks in `frame/run_startup.cpp`, plus
`VF_TEST_HOVER` in `frame/run_hooks.cpp`); `undoEdit` has **three** callers (the Undo
button in `ui/panel_edit.cpp`, the `VF_TEST_UNDO` startup hook, and Ctrl+Z in
`frame/run_hotkeys.cpp`). This is the "never summarise a measurement into something
smaller" rule failing in the direction I least expected: not a wrong number, but a
correct number with the evidence dropped.

**What is actually stable, and worth keeping.** The directory-per-subsystem **shape**
is settled and will not move again. From the verified stage 1, the **definitions** are
safe to cite: `adoptPickOwnership` and `undoEdit` in `src/app/edit/live_edit.cpp`;
`firstSight` / `reportFrameResult` in `src/app/rhi/present_probe.cpp`;
`kDragTravelPx` **declared** in `src/app/ui/ui_types.hpp`. **Call sites are in flight
and must not be cited until the refactorer reports the gates green.**

Three structural facts worth carrying, because they explain *why* the layout is what
it is rather than just where things are:
- `kDragTravelPx` is declared with the **UI vocabulary** but **used at exactly one
  site**, the click gate. That single-use property is the reason it lives in
  `ui_types.hpp` instead of next to its only reader.
- `m_lastStampWroteCell` **straddles the split on purpose**: written by
  `edit/live_edit.cpp` (`applyEditLive`, `undoEdit`) and read by `frame/run_input.cpp`
  for the identity test. A page describing the click gate without noting that the
  write and read sides live in different subsystems will mislead the next reader.
- The refactorer is anchoring their own `AGENTS.md` edits to **symbols rather than
  line numbers** — the same conclusion this wiki reached independently, and the reason
  the anchor conversion is the right fix rather than cosmetic tidying.

**Gate note worth recording:** the refactorer rejected a sha256 render gate because
the splat backend is **not bit-reproducible**, and gated on statistics-within-tolerance
against a noise floor measured from the original binary instead. A hash gate would have
been an *instrument* that cannot fire — the exact class of thing this wiki keeps
flagging. Page markers now say the layout is mid-migration instead of naming locations
as current.

## 2026-09-26 lint | incident: tracked assets/ files deleted mid-gate, and assets/ is not read-only anyway
A `refactorer` session reported that three **tracked** files were deleted from the
working tree between ~08:10 and ~08:27 — `assets/world.json`, `assets/heightmap.png`,
`assets/landscape.vxw` — which broke its gate run (the app and tests refuse to start
without `world.json`). It restored them with `git checkout --`; `git status -- assets/`
is now empty, so the restore is complete.

**Not this session.** Zero writes outside `.opencode/wiki/**`; no `ninja`, no binary
run, no test run, no `heightmap_gen`; every git command read-only (`status`, `log`,
`diff`, `show`, `ls-files`, `check-ignore`, `branch`) with no `checkout`/`stash`/
`clean`/`reset`/`commit`. Independently checkable via `git status --porcelain`.

**Narrowing the suspect set (measured).** Those three files are **exactly**
`heightmap_gen`'s complete output set: `assets/heightmap.png` is its default output
path (`tools/heightmap_gen.cpp:235`), `landscape.vxw` is the terrain shell it emits,
and `world.json` goes through `writeManifest` (`:446`). So the candidate set is small —
anything invoking `ninja -C build world` / `vf_heightmap`, or a directory-level
operation over `assets/`. It is **not** the `world_all.json` writer below.

**The standing hazard, which matters more than the incident: `assets/` is not
read-only while tests run.** `tests/test_world.cpp:26` and `tests/test_authoring.cpp:21`
set `dir = VOXELFORGE_ASSET_DIR` and then write `dir + "/world_all.json"` — so
`test-world`, `test-unit` and `test-surfel` **rewrite a tracked file inside `assets/`**.
Two consequences for concurrent sessions: a reference-render verification can be
perturbed by another session's test run, and the write is a bare
`std::ofstream(path) << j` with **no temp+rename**, so an interrupted run leaves a
**truncated** `world_all.json` and every content test then fails on it. The overlay
writer in this codebase already uses temp+rename for exactly this reason, so the fix
pattern exists in-tree. Filed on [[concepts/measurement-discipline]] as a shared-state
hazard.

**Not the cause of the incident** — that would be an unearned inference — but it is the
reason "treat `assets/**` as read-only" is not currently a property the repo enforces
rather than a request.

## [2026-09-26] ingest | clang-lsp-setup — clangd LSP for OpenCode (installed, verified, tuned)
Installed the missing pieces and verified the whole chain end to end. `clangd-18`
18.1.3 was already present; added the `clangd` and `clang-tidy` metapackages (symlinks,
~16 kB each) and `clang-tidy-18`. Headline find: `--clang-tidy` in
`.opencode/opencode.json` plus the `Diagnostics: ClangTidy` block in `.clangd` were
**inert** — clang-tidy was never installed, and clangd accepts the flag with no warning.
`clangd --check` cannot detect this (it never runs ClangTidy), and measuring clang-tidy
directly is a trap: it writes to **stderr**, so `2>/dev/null | grep` yields a confident
wrong `0`. With the engine finally present, the configured
`modernize*,performance*,readability*` globs produced **2343 diagnostics over 4 files**
(1161 run.cpp, 1039 chunk_store.cpp) and **zero real compiler errors** — pure
house-style conflict against `AGENTS.md` conventions, which buried real errors. Tuned
`.clangd` with a commented, measured `Remove:` list. Verified with a real LSP handshake
using the exact opencode.json argv, plus a positive control. Filed
[[concepts/clang-lsp-setup]]; logged quirks for the inert `--clang-tidy`, the `--check`
error-count misread, the stderr trap, and `ninja -t clean` deleting the authored scene.

## 2026-09-26 lint | RESOLVED: the assets deletion was `ninja -t clean`, and the documented recovery is itself a regen
The refactorer session found the cause of the incident above and retracted the
"third session did it" line: **it was itself.** `ninja -C build -t clean` at ~08:20,
run to force a recompile. Verified in the build system: `CMakeLists.txt:130-132` sets
`VF_HEIGHTMAP_PNG` / `VF_WORLD_LAYERS` to **source-tree** paths, and
`build/build.ninja:1143` registers them — plus the legacy layer family — as OUTPUTS of
the `heightmap_gen` custom command. **The build system believes it owns the authored
scene.**

**Why exactly three files, and it is the nicest piece of arithmetic in this log.** The
registered output list is *stale*: it still names `house.vxw`, `tree1..6.vxw`,
`rock1..3.vxw`, `bushes.vxw`, `alpaca.vxw`, `fence1.vxw`, all removed when the baked
object sweeps were dropped. A clean can only delete what exists, so
deletions = registered outputs ∩ files on disk = the three survivors. That count is
what **confirmed** the mechanism rather than merely suggesting it, and it independently
validated the tighter "exactly heightmap_gen's output set" analysis.

**The part that matters more than the incident: `AGENTS.md`'s recovery advice is not a
no-op.** It says the fix is `ninja -C build world`. That fixes the symptom and is
itself a **regeneration** — it re-bakes the terrain shell from `terrainHeightAt()` and
rewrites `world.json`. A regenerated scene can differ from the committed one, and the
result is a working, plausible, *different* scene rather than an error. Bounded by
what survives: `writeManifest` preserves foreign entries whose `.vxw` still exists, so
the runtime-authored `hamlet_*` layers are kept — but the committed
`heightmap.png` / `landscape.vxw` bytes are not preserved. **So the correct recovery
order is `git checkout -- assets/` first**, and `ninja -C build world` only when the
files are genuinely absent from git.

Filed as [[concepts/authored-assets-are-build-outputs]] with the rules: never
`ninja -t clean` here (delete `build/` instead — that directory owns nothing in
`assets/`); identify the mechanism before restoring, because `git checkout` fixes the
symptom while only the mechanism tells you it will recur; and the structural fix if
`assets/` is ever re-plumbed is to stop registering versioned content as outputs.

Also noted: both sessions' audits cleared them, and the reason this closed in one step
instead of a witch hunt is that each could **enumerate its writes** rather than assert
"not me". That is now the recommended form of a denial on
[[concepts/measurement-discipline]] — make it checkable.

## [2026-09-26] lint | build-dir hazard corrected, wiki link check
- **Correction filed after cross-session verification.** A peer reported that
  `ninja -C build world` would recreate 12 stale `.vxw` files and inject phantom
  layers. Checked and **refuted** before it reached the user: `heightmap_gen.cpp:342-343`
  builds only `landscape.vxw`, `ai_edits.vxw` is consumed and never written
  (:406-411), and `writeManifest` gates every preserved layer on an `fopen`
  existence check before pushing it. A regen writes exactly heightmap.png +
  landscape.vxw + world.json and cannot change the hamlet. The real hazard is
  narrower: `ninja -t clean` deletes the 3 tracked assets (verified — 3 declared
  outputs exist, 3 files went missing), and also removes the clangd database.
  The 12 dead `VF_WORLD_LAYERS` names are cosmetic.
- **Lint:** one pre-existing broken wiki link, `[[concepts/water-flooding]]` in
  `log.md` (no such page). Not introduced here; left for its owner.
- Generalised two measurement traps into [[concepts/measurement-discipline]]
  (now eight instances): clang-tidy writes findings to **stderr**, and
  `clangd --check`'s "N errors" counts ERROR-level *log lines* — including
  non-applicable `ExtractFunction` probes — not diagnostics.

## 2026-09-26 ingest | app split verified: wiki re-anchored to symbols, and a new gate proved it can fire
The refactorer reports the split **complete and verified**: tree builds clean, layout
final, no more movement. Time to convert the wiki's line-number anchors, which is the
conversion the two of us independently agreed was the right fix rather than cosmetic
tidying.

**Re-anchoring done.** All `main.cpp:<line>` references on live pages are gone —
`grep` now returns **zero** across `entities/` and `concepts/`. They are replaced by
symbol anchors: `undoEdit` and `adoptPickOwnership` in `edit/live_edit.cpp`,
`reportFrameResult` / `forceFrameResults` in `rhi/present_probe.cpp`, `kDragTravelPx`
declared in `ui/ui_types.hpp` and used once in `frame/run_input.cpp`. New page
[[entities/app-subsystems]] carries the verified map so future anchors have somewhere
to point.

**Three things I verified rather than transcribed**, each of which corrected my
earlier notes:
- `tanHalfFov60()` is read at **five** sites, not the two implied by "gizmo + push":
  `record_interactive`, `record_headless`, `run_brush_preview`, `run_input`. It is the
  enabler behind the gizmo-hits-agree-with-push-agreements invariant, because it used to
  be a local in `App::run` — which is *why* the UI slices could not reach it.
- The `m_lastStampWroteCell` **straddle is real and load-bearing**: written in
  `edit/live_edit.cpp` (`applyEditLive`), cleared there in `undoEdit`, read in
  `frame/run_input.cpp` for the identity test. State belongs to the edit path, the
  decision to the input path.
- The `frame/` file list **changed again** since my earlier note — `run_capture.cpp` is
  now `record_headless.cpp` + `record_interactive.cpp`, and `run_poll.cpp` is new. A
  hand-maintained file list in a wiki page is a claim with an expiry date, so the page
  links the subsystem rather than pretending to be an inventory.

**Gates, and the part worth keeping.** `--selftest` PASSED at 75.9 % coverage identical
to the pristine-HEAD control; hero render inside the control's noise floor; the three
headless hooks emitting **identical log lines** to the control; `--mode svo` still
running. And the new `test-app` group — 34 cases / 192 assertions over gizmo maths,
`parseArgs` and the sidebar vocabulary, i.e. logic that was **untestable** while it
shared a translation unit with `main()`.

**Three of those new gizmo cases failed on first run, and the expectations were wrong
rather than the code**: forward is `+X` at yaw 0 (not `-Z`, which is yaw −90°); a point
at a ring's centre is **one radius** away (not 0); and `+X` on the pitch ring is *nearer*
in normalised terms, so it is not the unambiguous pick. Each was measured with a
throwaway binary before a line of the test changed. Filed on
[[concepts/measurement-discipline]] as **a gate that has never failed is not yet known
to fire** — three same-day reds are the cheapest available proof that a new gate can
tell right from wrong, and a first-run red is the *good* outcome, not the problem.

## 2026-09-26 lint | I counted a definition as a call site, and the frame loop's one real change
Two corrections, one of them mine and small, which is the point of recording it.

**I was wrong about the FOV call-site count.** I wrote "five call sites" for
`tanHalfFov60()`; it is **four** — I counted the *definition* line in `frame/frame.hpp`
as a caller. The refactorer measured four and conceded their own "two". Re-measured
with `grep -v 'inline float'`: four callers (`record_interactive`, `record_headless`,
`run_brush_preview`, `run_input`), all in `frame/`. Their framing of "three
subsystems" is also off — all four are in one subsystem — so the page now says what is
true and why it matters: the gizmo/input hit-test, both recording paths and the brush
preview read **one** function rather than each keeping a copy. A number error inside
the page about numeric discipline, which is exactly the species this log has been
tracking. Cheap to fix, cheaper to record than to leave.

**The one substantive change in the split, now on
[[entities/app-subsystems]].** The frame loop's `continue`/`break` became two sentinels,
`kFrameDone` (frame handled, go again) and `kFrameExit` (leave, run the shutdown
epilogue, return 0) — `frame/frame.hpp:24-25`, dispatched in `frame/run.cpp:130-132`.
The asymmetry being preserved is the non-obvious part and it was **already broken in the
original**: a `break` ran the ImGui/device shutdown epilogue, but a `return` from inside
the loop did not, so a failed submit or a written HUD shot skipped cleanup. Returning a
non-negative status would have collapsed the two cases, because both are non-negative;
the distinct sentinel is what keeps cleanup reachable. Anyone adding an early return to
either recording path must return `kFrameDone`, never a bare `0`.

## [2026-09-26] verify | clang-lsp-setup — tuning confirmed by real LSP session
Replaced the pending claim in [[concepts/clang-lsp-setup]] with a measured result,
using a real `clangd` LSP session on the exact `opencode.json` argv:

| file | before | after |
|---|---|---|
| `src/core/camera.cpp` | 32 | **0** |
| `src/voxel/chunk_store.cpp` | 1039 | **11** |

The 11 survivors are `modernize-use-auto` (7), `readability-use-anyofallof` (2),
`unused-includes` (2). That those two **kept** checks still fire is the load-bearing
part: a *rejected* `.clangd` also yields zero, so "quiet" alone would have proven
nothing. All removed checks are gone. `.clangd` takes plain names in `Remove:`
(no `-`; clangd adds it) — unlike the `clang-tidy` command line, where the same
suppression needs the `-` prefix and a spec built without it silently re-enables
everything.

Instrument bugs found and fixed while measuring (all three produced confident
wrong readings): a probe waiting for a *non-empty* `publishDiagnostics` never
terminates on a correctly tuned file; breaking on the *first* publish instead
reports EMPTY because clangd publishes `[]` during the preamble phase; and a
`-checks` spec assembled without `-` prefixes makes BEFORE and AFTER identical.
Each was caught only by running a positive control.

## [2026-09-26] ingest | m_shots shadowing + "green signal that does not cover the change"
- **[[entities/app-subsystems]]**: the `src/app` split's worst trap, reported by the
  refactoring session. A word-boundary rename promoting a local to a member turned
  `std::vector<ShotSpec> shots = args.shots;` into `std::vector<ShotSpec> m_shots =
  args.shots;`, which *constructs* the member and leaves the local name shadowed.
  `App::m_shots` stayed empty, so `shotMode` (`!m_shots.empty()`) was never true and
  `--shot` looped forever without writing a PPM — clean compile, no crash, no wrong
  pixels, and a **byte-exact body comparison correctly passed** because only the
  declaration differed.
- **[[concepts/measurement-discipline]]**: new section, "A green signal that does not
  cover the change is not weak evidence — it is none". Same incident, process half:
  stages were reported "builds clean, gates pass" on a green compile plus
  `--selftest`, which **never renders a shot**, for a change in the shot-capture
  path. `test-visual` does render `--shot` and would have failed in ~1 minute.
  Complements (does not duplicate) the existing "gate that has never failed" section.
- **De-duplicated**: the `ninja -t clean` hazard now lives in exactly one place,
  [[concepts/authored-assets-are-build-outputs]] (written by another session, and the
  better home). [[concepts/clang-lsp-setup]] keeps only the LSP-specific coupling — a
  clean also removes `build/compile_commands.json`, i.e. the language server's flags.
- Filed quirks for the shadowing mechanism and the non-covering-gate claim.

## 2026-09-26 ingest | the split's one real bug, and what the verification was structurally unable to see
The `src/app` split is **frozen and fully gated**. All groups green when run
**sequentially**: `test-app` 1/1, `test-unit` 8/8, `test-store` 2/2, `test-world` 2/2,
`test-surfel` 4/4, `test-smoke` 7/7, `test-visual` 3/3, `test-live-edit` 3/3; hero shot
within the pristine-HEAD control's noise floor; clean build, zero new warnings;
`assets/` pristine (the test runs rewrite the tracked `assets/world_all.json` — the
hazard filed earlier, and the reason it needed restoring again).

**The headline is a bug the split's own verification could not see.** Promoting the
loop local `shots` to the member `m_shots` by word-boundary rename turned
`std::vector<ShotSpec> shots = args.shots;` into **a local that shadows the member**. It
compiled clean; the body stayed **byte-identical** so the slice verification correctly
passed it; `--shot` hung forever. Fixed as an assignment (`m_shots = args.shots;`,
`frame/run_startup.cpp` — verified). Filed as its own principle on
[[concepts/measurement-discipline]], because it is the cleanest statement of the
problem this whole log has been circling:

> **A verification method's blind spot is determined by what it compares.** The
> artefact it protects and the class of bug it cannot see are the *same axis* — so
> "byte-identical body" is the strongest evidence of a pure move and is **not** evidence
> that the move was pure. The signal that caught it was a **different class**:
> wall-clock, ~11 s → ~600 s. No diff, hash or byte-comparison would have flagged it.

**The second error generalises just as well, and the phrasing is the refactorer's:**
it sent three peers *"builds clean, gates pass"* on a green compile plus `--selftest` —
on a change whose entire surface is **shot capture**, which `--selftest` never renders.
"The sentence was true of the signals I ran and false as a claim about the change."
Filed as **a gate result is evidence about the gate, not about the change**: "2/2 green"
is a claim about a named group on a named revision, and repeating it honestly requires
the coverage argument, not just the tick marks. Both errors are invisible in the
artefacts — a reviewer re-running anything would not detect them.

**Also filed:** the `sha256` render gate that could not fire (the splat backend is not
bit-reproducible — same binary, two runs, different hashes), replaced by
statistics-within-a-floor against a pristine-HEAD control built for the purpose; the
measured split of which statistics are stable (**mean RGB and p99 exact**, p50 of
per-pixel diff varying −0.009…+0.022 against a 0.03 floor), which is reusable for
writing the next render gate; and the `test-live-edit` red that was **four concurrent
instances contending for the GPU** rather than a regression (3/3 alone) — hence "run
groups sequentially on a shared GPU" and "a red that appears only under concurrency is a
contention hypothesis until proven otherwise".

**Explicitly not claimed as verification**, and worth honouring: the clangd diagnostic
reduction (style, not correctness — the compiler was always the authority) and the
byte-exact slice tooling, which protects *bodies* and demonstrably did not protect a
*declaration*. Frozen layout recorded on [[entities/app-subsystems]] with reproducible
commands: 36 `.cpp` under `src/app`, largest 441 (`edit/live_edit.cpp`), largest header
539 (`app.hpp`), `frame/run.cpp` a 153-line orchestrator.

## 2026-09-26 lint | the LSP session was right and my rule was the weaker one
An observation forwarded from the `LSP` session (which deliberately did not edit the
page, correctly — an under-credit is a note for the maintainer, not a licence to write).
Taking it, because the sharpened rule is genuinely better than the one I had.

**What I had written:** "other sessions take the GPU/display with no warning, so a
result that **looks impossible** is usually cross-session state." What the instance
actually teaches is different and more mechanical: the discriminator was **not** the
failure's severity, it was its **provenance** — the red arrived with **no diff
attached**. "Looks impossible" asks the reader to judge impossibility, which is
subjective and gets argued about; "red with no change attached" is checkable.

So the rule on [[concepts/measurement-discipline]] is now:

> **If a gate goes red and nothing has changed since the last known-good, suspect the
> environment before the code — re-run it alone.** Not: trust the red. And not: go
> hunting a regression in code nobody changed.

**The pairing is the best argument for the page, and it was free.** On the *same*
refactor, in the *same* day, both failure modes appeared: a gate went **red with
nothing changed** (the *result* was wrong — four instances contending for one GPU), and
a gate reported a **difference on an unchanged binary** (the *instrument* was wrong —
`sha256` on a splat backend that is not bit-reproducible). Neither is detectable by
re-running the same gate harder, and a reviewer trusting either artefact would have
filed a code defect that did not exist. Hence the operational half: **when both the
result and the instrument are suspect, suspect the instrument first** — it is the only
one of the two that is usually wrong for reasons *outside* the change.

Worth noting the coordination itself worked: the observation reached the maintainer
without either session editing a page it does not own, and without evaporating when
the session ended. That is the intended division of labour and it is now demonstrated
rather than assumed.

## 2026-09-26 lint | full link/orphan check after the split re-anchor: clean, one known-benign dangling ref
Run after converting every `main.cpp:<line>` anchor on live pages to symbols.

- **Dangling links: 1** — `concepts/water-flooding`, referenced only from three
  **historical log entries** for a page that was later replaced. This is a record of
  the past, not a live cross-reference, so the correct treatment is to leave it and
  stop re-investigating it (noted twice before in this log). No action.
- **Pages missing from `index.md`: 0.** The two pages added this session
  ([[entities/app-subsystems]], [[concepts/authored-assets-are-build-outputs]]) are
  both indexed *and* carry a navigation line.
- **Orphans (no inbound link): 0.** Every page is reachable, so the cross-links are
  doing real work rather than decorating each other.
- **`lastReviewed` corrected** on `concepts/focused-test-groups` (said 2026-09-25
  despite substantial edits this session — the staleness lint exists precisely to
  catch that class, and it did).

The one thing a link check cannot judge is whether a page is *true*; that is what the
review trail above is for, and the three self-reported defects this session are
recorded with their sources rather than smoothed over.

## [2026-09-26] ingest | live-edit surfel parity

**Report.** Editing objects in voxel mode (add / delete / smooth) changed the
splat directions of voxels the brush never highlighted — the cabin's wall disks
near the door all came out pointing upwards.

**Root cause (measured, not inferred).** `LiveEditor::refreshRegion` drops and
re-derives *every* cached parent in the edit AABB ± margin, so untouched cells
are re-aimed by the store path's rule while their neighbours keep the bake's.
The store path derived normals from a central difference of the byte-quantised,
nearest-sampled brick SDF; deep inside a thick body `d(x+2) == d(x-1)` exactly,
the gradient cancelled to zero, and the `n = (0,1,0)` fallback fabricated a
straight-up normal. Cabin object surfels vs the bake's final normals:
**27.8 % more than 30° off, mean 22.4°, 1191 fabricated straight-up.**

**Fix.** `collectChunkCandidates` now runs the bake's pipeline on store data:
exposed-face mean (never a fabricated direction) → per-face expansion for
cancelling and thin-corner cells → face-neighbour smoothing over raw normals.
`SurfelCand` gained `rawN` / `exposedFaces` / `faceEntry`; `buildMicroSurfels`
skips repeated keys so a cell still yields one micro set. After: **0.08 %,
mean 0.57°, zero fabricated-up, zero wrong entry counts** (thick cells).
Stamp latency unchanged (33.2 ms vs a 33.0/34.2 ms baseline, 10×add, Ø2 m).

**Guard** — `tests/test_store.cpp` "live surfel normals follow the bake, never a
fabricated up": bake parity for a whole object chunk plus a live stamp 2 cells
out from a wall, asserting the wall's normal is bit-identical after a refresh
that provably re-derived it. Confirmed to fail on the pre-fix code (40
fabricated-up, mean 47.3°, live `dot == 0`).

**Files.** `src/voxel/surfelize.{cpp,hpp}`, `tests/test_store.cpp`,
`AGENTS.md`, `docs/rendering.md`, new page
`concepts/live-edit-surfel-parity`.

**Left standing, deliberately.** The brick's `raw == 0` truncation makes a
small-positive-distance cell read as solid in the store while `VoxelField` calls
it air (0.77 % of cabin cells, one-sided). That is a storage-format property the
SVO DDA depends on (`raw <= 0` = solid), so the guard's angular comparison
excludes those cells rather than "fixing" them. Thin-cell *entry counts* still
differ from the bake on those cells.

**Gates.** test-unit, test-app, test-world, test-store, test-surfel,
test-live-edit (incl. the 269 s `live_edit_check`) — all pass.

## 2026-10-01 ingest | clang LSP status, and a log signal that outlived its binary
Asked whether clang LSP is active. Answer: **configured and healthy, but not
spawned** — no `clangd` process, and no LSP tool in the session toolset, even
after opening a `.cpp` through the read path.

**The setup is fine** (all re-measured): `.opencode/opencode.json` argv
`clangd-18 --background-index --clang-tidy … --compile-commands-dir=build`;
`clangd-18` → LLVM 18.1.3; `build/compile_commands.json` present; `.clangd`
tuned ClangTidy list; and the Trap-1 fix holds — the sibling
`/usr/lib/llvm-18/bin/clang-tidy` is installed, so `--clang-tidy` is live
rather than silently inert. A `--check` run with the exact argv parsed
`src/voxel/picking.cpp` cleanly (0 compile diagnostics; the one reported "error"
is clangd's internal `ExpandDeducedType` tweak, not a source diagnostic).
Note `--check` still does **not** exercise ClangTidy, so it can never verify
Trap 1 — only the sibling binary's existence can.

**The wiki was carrying a dead signal.** [[concepts/clang-lsp-setup]] cited
`enabled LSP servers … clangd` in `opencode.log` as proof the config block was
being read. `enabled LSP servers`, `serverIds` and `all LSPs are disabled` are
**absent from the v2.0.21 binary** (`bytes.find()` → −1 for all three): those
log lines came from an older build. So their disappearance from a current log
proves nothing, and their historical presence never proved a server spawned.
Corrected on the page, with the replacement check named.

**Two measurement traps hit while answering, both worth the detour:**
1. `grep "[c]langd"` matched **my own command line** — the bracket trick only
   defeats self-matching when the *pattern* is the only literal; an echoed
   `clangd` in the same command re-arms it. Match on `comm`, not the full
   command.
2. Counting `enabled LSP servers` per-day reported a hit for today that was
   **my own grep command echoed into the log**. Any log statistic computed with
   `grep` over a file the same session writes to must exclude the searching
   process's own lines — the same shape as
   [[concepts/measurement-discipline]]'s "green signal that does not cover the
   change".

Deliberately **not** concluded: why clangd is not spawned. OpenCode v2 spawns
LSP servers lazily per project, and this session has not opened a C++ file
through a path that triggers it, so absent-process remains ambiguous — the
page's original "an absent process is not a fault" still stands. Untested and
worth a follow-up: whether v2 needs `lsp` re-enabled in the TUI, and whether
`.opencode/`-directory configs still feed the LSP block in v2.

## 2026-10-01 ingest | Brush preview visibility (depth inert, preview blinked out under sidebar hover, SVO had none)

Reported as "size adjustment of brush not always visible in 3d view". Measured
first, and the measurement reframed it: the **noise floor is ~1.25 %** of pixels
for two runs of an *identical* `--shot` (TAA jitter), and three of the four A/Bs
I expected to be signal were inside it.

Three independent causes, all now fixed and gated:

1. **Depth was structurally invisible.** The tint recolours *existing surfels*
   inside the volume, so it can only mark geometry already in the scene. A
   Carve's extra depth goes below the surface into solid material; an Add dome
   grows into empty air. Debug view 15 measured Carve's affected-surfel mask as
   **bit-identical at 2920 px** across depths 0.1/0.5/2.0/12.0 m. Width, which
   spreads across the surface, was fine all along (68–72 %).
2. **The preview vanished exactly while resizing.** Hover picking is gated on
   `!io.WantCaptureMouse` (correct — a slider click must not stamp), but the
   preview read the same gated `m_hoverHit`, so grabbing the Width slider
   erased the highlight for the duration of the drag. Fixed with a
   preview-only `m_latchedHover`; `applyEditLive` still needs a live hit, so a
   latched preview cannot stamp.
3. **SVO had no preview at all** — `setBrush` fed only `m_splatPass`. The
   BrushUBO is now bound at 13 in the SVO pipeline with an identical std140
   layout, and `inBrushVolume` moved to `shaders/common_surfel.glsl` so one
   definition serves both. SVO tests the raymarch hit point, a closer match to
   the CPU cell set than the splat per-surfel approximation.

The instructive part is the **failed first attempt** at the depth marker: drawn
as a world-space band inside the `hitType > 0.5` visible-surface block, it
reached only 1.7 % at depth 12 m, because a Carve's segment runs *into* the
ground and the only fragments it can mark are the sliver at the top. Screen
space, drawn on every fragment, is what the axis needed: depth 0.5→6 m now
moves 8.6 % (Carve) / 10.2 % (Add).

Two instrument traps worth not repeating: debug view 15's magenta is
tonemap+TAA-shifted, so a tight `(255,0,255)` threshold reads a real 2900-px
mask as **zero**; and a **top-down camera foreshortens a vertical depth line to
a point**, which made a working marker read as 0.7 % until re-shot obliquely.

Also fixed in passing: the SVO descriptor pool was sized for 1 uniform buffer
against 3 bindings, with no `COMBINED_IMAGE_SAMPLER` entry at all — it only
worked by driver leniency.

New page: [[concepts/brush-preview-visibility]]. New gates in
`tests/live_edit_check.py`: `check_depth_sensitivity`, `check_svo_preview`.
Not yet re-run: the full `test-live-edit` / `test-visual` groups.

## 2026-10-01 ingest | Narrow `test-preview` group so a preview change stops implying the whole live-edit matrix

Follow-up to the same session. Verification granularity was the complaint, and
it was fair: the preview fix touches `post.comp`, `svo_raymarch.comp`,
`common_surfel.glsl` and the frame's preview plumbing, which by the group table
meant `test-live-edit` *or* `test-visual` — both dominated by ~13 s per-run
world loads for what is really one frame per assertion.

`live_edit_check.py --only preview` now runs `check_preview` +
`check_depth_sensitivity` + `check_svo_preview` against a baseline it renders
itself, wired as CTest `preview_check` behind the same `group_gate.py`
(`SKIP_RETURN_CODE 77`, verified: skipped when the group is off) and exposed as
`ninja -C build test-preview`. It is deliberately **not** folded into any other
group: `test-live-edit` still runs everything, preview checks included, so this
adds a cheap entry point without weakening the existing gate.

**A measurement I had to throw away, and why it matters.** The first
`test-unit` run reported `fast_placement_tests` FAILED — but the same test
passes standalone. Cause: I ran `ninja -C build` *while* `test-unit` was
executing `vf_tests`, so the binary was replaced underneath a running test. That
is the "never edit a file a running process is executing" rule from AGENTS.md
reaching into the build directory: a concurrent `ninja` against a shared `build/`
invalidates any test result in flight. Re-ran clean and chained, and treated
the red as noise rather than debugging a test that was never broken.

Also noted: `src/app/**` is currently **untracked** — the app split landed in the
working tree but was never committed, so `git diff` cannot review changes to
`app.hpp` / `run_input.cpp` / `run_brush_preview.cpp`. Worth committing before
the next session piles more on top.

## 2026-10-01 ingest | The new depth gate caught a too-faint marker, and caught me measuring it wrong

`test-preview`'s first run failed exactly as a new gate should: `depth(carve)`
reported 0.90 % against my hand-measurement of 8.60 %. Two things were true at
once.

**The gate was right; the marker was too faint.** `diff_stats()` defaults to
`thresh=10` — a pixel counts only if some channel moved by *more than* 10
codes — while my ad-hoc A/B used exact byte inequality. My original marker was a
wide `smoothstep` falloff, which changes a large area by 1–10 codes: generous
under an exact diff, invisible under any per-channel threshold, and too washed
out to read on screen. Changed to a 1.5 px fully-opaque core with a 5 px halo.
The general lesson, now in the marker comment: **a thin UI overlay wants a small
opaque core plus a short halo; a broad soft gradient covers lots of pixels and
still reads as nothing.**

**My measurement was the sloppy half.** When hand-checking something a gate also
checks, use the gate's own diff function and threshold. A stricter ad-hoc
comparison does not give more confidence — it gives a different number that
looks like a regression, and the tempting move is to "fix" passing code. Worth
stating plainly because the instinct on a red gate is to distrust the gate.

`test-unit` passed clean on the re-run (287 s), confirming the earlier
`fast_placement_tests` red was the concurrent-`ninja` artefact and not a real
defect. Also removed a `m_hoverLatched` bool that was written but never read —
`m_latchedHover.hit` is the single source of truth, and a parallel flag is
just a second thing to drift.

## 2026-10-01 lint | Closing out the brush-preview session

All three preview causes fixed and gated; `test-unit`, `test-live-edit` (488 s,
including the two new checks) and `test-preview` (111 s) green.

The depth marker took **three** attempts, and the gate earned its keep by
catching the first two:

1. **World-space extent inside the `hitType > 0.5` visible-surface block** —
   1.7 % at depth 12 m. The segment runs *into* the ground, so everything past
   the top sliver is occluded by the very geometry being edited.
2. **Screen-space line, unclamped** — better in principle, and 0.90 % → 0.67 %
   after I "strengthened" it. The frames were byte-identical to the eye. A deep
   brush's far end is metres *below* the surface; with the camera on a shallow
   downward angle that point projects far outside the frustum, so the line left
   the frame within a few pixels and every depth drew the same stub. Clamping
   both projected ends into the viewport fixed it.
3. **Opaque 1.5 px core + 5 px halo** — a wide soft gradient moves most pixels
   by 1–10 codes and is invisible to `diff_stats`' `thresh=10`.

And the constant I set by *guessing* was the fourth failure, hidden behind the
first three: `NOISE_FLOOR = 0.02` came from the unfiltered exact-diff noise
(2.64 %) instead of the `thresh=10` noise (**0.008 %**). It was rejecting a
signal **217× above the real floor**. Measuring the floor with the gate's own
instrument — rather than a stricter ad-hoc one — is what exposed it.

So the honest tally is four wrong turns, three of them caught only because the
gate used a threshold and an instrument I had not been measuring with. A
useful reminder for this codebase: a "focused" test that reuses the hand-rolled
`diff_stats` is only focused if you also calibrate *it*.

Depth now: **1.74 %** between 0.5 m and 6.0 m, against a 0.008 % floor. Verified
by eye as well — a vision pass on both frames reports a clearly visible orange
brush indicator, so the gate is not passing on a sub-visible artefact.

Not done / worth a follow-up:
- `src/app/**` is still **untracked**, so these changes cannot be reviewed with
  `git diff`.
- The depth indicator saturates once the far end pins to a viewport edge — a
  screen-space line cannot exceed the frame. Honest, but it means very large
  depths look alike; a numeric HUD readout would be the next step if that
  matters.

## 2026-10-01 ingest | Both follow-ups closed: depth no longer saturates, and the tree is committed

**Depth saturation.** A screen-space line cannot be longer than the frame, so
once a deep brush's far end clamped to a viewport edge, 6 m and 12 m drew the
same picture. Two channels now cover it:

- in the viewport, the clamped end keeps *moving along the edge* as depth grows,
  and a short **cut-off cap** is drawn across the axis whenever the true far end
  is off-screen — so a clamped terminus never reads as "the hole ends here".
  6 m vs 12 m went from indistinguishable to **3.0 %**, and is gated by a new
  `depth_pair("carve_deep", 6.0, 12.0)`.
- the **sidebar footer** prints the depth numerically (`carve 15vox 1.5m`). A
  number cannot saturate, and it is the readout still visible while the pointer
  is over the panel — where the 3D preview is running on the latched hover.

**The untracked tree is committed** (`0c1449c`, `b1e04f3`, `e6ea1e0`).
`src/app/**` had never been in history, so `git diff` could not review changes to
`app.hpp` / `run_input.cpp` / `run_brush_preview.cpp`. Two honest caveats
recorded in the commit messages rather than papered over:

- the app-split commit necessarily carries this session's app-side edits
  (hover latch, depth feed, footer readout), because they live inside files
  that were never committed and cannot be separated without rewriting history;
- `CMakeLists.txt` carries the `test-preview` entry for the same reason — it
  could not be split out of the source listings by hand.

Three more wiki pages (authored-assets, clang-lsp, live-edit-surfel-parity)
were committed because the already-committed `AGENTS.md` and index referenced
them — the history had dangling links until now.

**Left alone deliberately**, after checking with the peer session rather than
guessing ownership: `surfelize.cpp/hpp`, `tests/test_store.cpp` (mtimes show
another session has them open *right now*), `.clangd`, `docs/architecture.md`,
`docs/getting-started.md`, `docs/tooling.md`, and `.tmp_hole/` (the peer's
scratch). Verified the committed `CMakeLists.txt` references 110 paths and all
110 exist in the committed tree.

**Overlay warning worth keeping.** A peer session reported that headless
`--shot` runs silently overwrote the user's live-edit session: without
`VF_NO_OVERLAY=1` or `VF_OVERLAY_PATH=<tmp>` a run loads `runtime_edits.vxw`,
`loadStoreOverlay` re-derives chunks and flags them edited, and the writer then
re-serializes only that subset. My own renders all set `VF_NO_OVERLAY=1`, but
`gpu_selftest` in CMakeLists runs `--selftest` with neither guard set — worth
hardening that entry.
