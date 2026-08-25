# log

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
