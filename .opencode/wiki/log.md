# log
Append-only timeline — 164 entries, 4577 lines. Entries are appended
in **session order, not date order**, so this index is the only chronological view.
Do not infer causation from vertical position in the file itself.

> **A log entry is a pointer with provenance, not a restatement of the page** —
> whatever lives on a content page does not belong again here. See
> [[concepts/measurement-discipline]].

## Index

### 2026-10-08
- **ingest** — Dynamic sun shadows: the 13 s stall is not the sun's fault
- **ingest** — baked-sun-shadow-contract — the splat sun-shadow bake rule (CPU shadowMarch, backface skip, neighbour averaging), plus why bit 1 widening to point lights changes enclosure
- **lint** — catalog/link sweep: 1 missing Pages entry found and fixed, 1 benign dangling link
- **decision** — Sun shadow-map pass chosen for dynamic shadows; "Shadows" (bit 1) widened to gate point-light occlusion
- **lint** — Hedge sweep across content pages: zero asserting hedges; one rule was missing, not one habit
- **ingest** — Emissive-derived point lights landed (seam closed); night ratio band marked STALE; verified shader A/B harness
- **lint** — `test-night` was missing from the group table, and group independence was never stated
- **finding** — `VF_TEXTURES=0` was not bit-exact on the per-cell override path; `ninja -k1` masquerades as a skip
- **lint** — Systematic sweep: 44/52 pages reference changed files, 2 real defects found and fixed
- **correction** — The night band is NOT stale — re-derived unchanged, and now empirically confirmed blind
- **ingest** — Renderer improvement roadmap filed; EDT-batching state verified in code
- **query** — Per-object voxel size for detailed objects
- **ingest** — irradiance-volume page — emitter-lit indirect, and why frame means cannot verify it
- **correction** — Night band re-derived unchanged and empirically confirmed blind; surfel counts reconciled
- **ingest** — GPU-perspective analysis: per-object finer voxels have three independent blockers
- **coordination** — Ownership map settled across five sessions
- **correction** — My bridge line to the normalizing layer-load was wrong; filed the correction
- **finding** — Per-object voxel sizes: analysis consolidated, the "minimal honest change" landed
- **fix** — irradiance volume: origin-centred cell frame (51.2 m / 32-cell misregistration)
- **lint** — Structural-lint rules filed (three axes); log ordering caveat
- **rule** — A guard defined relative to its feature disappears with the feature (standing review question from the micro removal)
- **finding** — Only the carve could have caught the rim class (monotonicity vs exact count); dome/sphere boundary-count proposals authorized
- **status** — Victor Phase 1 reported green on its own cases (429 assertions); slicesWithSky attribution routed to Vega
- **verify** — Falloff-curve contract implemented-and-verified (13/13 cases, 3013/3013 assertions); dome graded-column-kill filed, sphere structurally exempt, 3053→3013 explained
- **verify** — Slice-stride lead promoted: vf_core green via ninja, irradiance 7/7-180/180, test-world 2/2 (110.66 s), md5 untouched; labelled current-tree until Vega's 4 files commit
- **verify** — Dome/sphere boundary gap closed (15/15, 3037/3037); every tapered boundary proven
- **status** — Victor Phase 1 settlement part one: 5 port files + importLayer landed in worktree uncommitted; slicesWithSky + texture-header still open
- **finding** — Checkout-loss recurrence instance two: TextureBinding::emissive taken by port checkout, re-implemented from usage sites; attribution prime-suspect-unconfirmed, durable gate open
- **lint** — lastReviewed bumps owed and paid: detail-pipeline + uncommitted-edit-is-not-yours both substantively edited 2026-10-08 but still stamped older
- **decision** — Emissive-lights spec change ordered (every emissive voxel a shadow-casting point light, supersedes thinning + 16-cap); filed pending, effective on Wiki landing
- **decision** — Full dynamic lighting ordered as priority after micro green (live re-derive, no baked shadow/stall/16-cap); owners Victor shadow-map / Wiki cells-emission, both greener paths are prerequisite gates
- **verify** — Micro removal landed in tree (app surface deleted, bake emitter retained off, guard replaced micro-free); banners down, cost table restated as history; labelled current-tree until commit
- **status** — Dynamic lighting in progress (N=64/K=4 unbounded-adjustable, sole priority); visibility proof required on user zero-visible-change report
- **lint** — Wiki cleanup sweep: micro-as-current scrubbed (11 files), 16-cap/thinning marked interim, layout contract rewritten two-live-ranges, N/K verified in-tree at worldfile.hpp:147; two items routed back unresolvable
- **lint** — Quirk hygiene: 10 quirks updated (5 rewritten, 5 amended) for micro landing + N/K flip; demo-capture variant corrected to tool truth (`no_lod`)
- **lint** — Ad-hoc scanner promoted to `wiki/lint.py` (multiline sourceRefs, index uniqueness, self-test controls, staleness as ranked-advisory); first run removed 2 build-output sourceRefs, gate green

### 2026-10-07
- **fix** — the day/night switch page claimed "async reload" — it is a stall
- **decision** — editing lights needs a writer that does not exist yet
- **gotcha** — writeTextureManifest is not atomic — the precedent's failure mode was the trap
- **lint** — svo-render.md: schema bug plus two stale claims
- **measure** — sunset/sunrise: the gradient is right, the magnitude is not
- **verify** — sunset lane closed green; wiki lint clean at 50 pages
- **gotcha** — a shader edit does not touch the binary — verify identity, not health
- **ingest** — Day/night switch implemented (uncommitted); measurement-provenance page filed
- **ingest** — Night gate: test-night group + night-gate-thresholds page; sun_angles.hpp hoist

### 2026-10-06
- **ingest** — Enclosed-space shading and light sources
- **ingest** — The visual_check sky probe is a camera + content assertion
- **lint** — Wiki link check: 42/42 resolve; the one dangling link is closed by decision
- **ingest** — sun-direction-pipeline — kSunDir production & consumers
- **ingest** — overlay-silent-write-trap — harness can destroy ignored state
- **ingest** — sun-key A/B measured — which gate catches day/night
- **ingest** — heightfield-blindness-enclosure + uncommitted-edit-is-not-yours

### 2026-10-04
- **ingest** — + / - bound by keycap, not by key position
- **update** — Tab is the only View/Edit switch
- **ingest** — decision-driven navigation (vf_nav) + ascii_view
- **ingest** — Tower/cabin rebuilds + nature layers + the surfel thin-rule roof bug
- **lint** — visual_check house sky-probe failure is pre-existing
- **ingest** — Rebuilds re-filed as new layers (hamlet_cabin, hamlet_tower_v2)
- **ingest** — Dense forest + tower moved into it
- **ingest** — Denser forest, gravel path, forest-floor litter
- **ingest** — Soil watermark stripped; Designer textures imported
- **ingest** — Second wood, cabin->tower path, cobblestone river street

### 2026-10-03
- **ingest** — Smooth brush quality + proportional Add/Carve
- **ingest** — Falloff curves, and what the surfel-normal measurement actually says
- **ingest** — edit-mode hotkeys + bottom hotkey bar
- **ingest** — Contention voids a measurement in BOTH directions; interactive UI is untested

### 2026-10-01
- **ingest** — clang LSP status, and a log signal that outlived its binary
- **ingest** — Brush preview visibility (depth inert, preview blinked out under sidebar hover, SVO had none)
- **ingest** — Narrow `test-preview` group so a preview change stops implying the whole live-edit matrix
- **ingest** — The new depth gate caught a too-faint marker, and caught me measuring it wrong
- **lint** — Closing out the brush-preview session
- **ingest** — Both follow-ups closed: depth no longer saturates, and the tree is committed
- **ingest** — splat base-seal architecture + the silhouette-fade frontier

### 2026-09-26
- **ingest** — a click is one edit (per-voxel brush stamped two voxels)
- **review** — verified the live-edit-brush session's changes; corrected a stale brush page
- **lint** — the uncommitted multi-author tree is now committed under a tooling-only message
- **ingest** — the click gate's "no seam" is an in-app-only limit — XTEST already reaches it
- **ingest** — Undo/"hollow cabin" filed as UNDER INVESTIGATION; the real finding is a test coverage gap
- **lint** — CORRECTION: the 8M surfel bound is not the live-edit session's
- **ingest** — dead-disc finding caused a revert; Undo bug now has a measured signature
- **ingest** — Undo root cause MEASURED: a refresh-margin bug, and wider is not safer
- **lint** — margin class now gated — but the gate can pass while testing nothing
- **lint** — a frozen test constant has TWO vacuity paths, not one
- **lint** — my assertion caught the reviewer's own false premise — and the fix is better than mine
- **lint** — RE-CORRECTION: the cell is object, the reader was broken, and a new log field is a new instrument
- **ingest** — vf_slice was never broken: opposite failures, one cause — the mistake is in the reading
- **lint** — FINAL: both crossed groups green; I declined test-surfel, and here is the evidence for that
- **ingest** — the forced-failure test found a real bug; and "never taken" beats "behind an unset guard"
- **ingest** — paired lesson: the expensive silent failures are the ones with no compiler behind them
- **lint** — src/app was split per subsystem: every main.cpp line number in this wiki is now stale
- **lint** — CORRECTION: I documented a mid-flight layout as verified, and dropped call sites I already had
- **lint** — incident: tracked assets/ files deleted mid-gate, and assets/ is not read-only anyway
- **ingest** — clang-lsp-setup — clangd LSP for OpenCode (installed, verified, tuned)
- **lint** — RESOLVED: the assets deletion was `ninja -t clean`, and the documented recovery is itself a regen
- **lint** — build-dir hazard corrected, wiki link check
- **ingest** — app split verified: wiki re-anchored to symbols, and a new gate proved it can fire
- **lint** — I counted a definition as a call site, and the frame loop's one real change
- **verify** — clang-lsp-setup — tuning confirmed by real LSP session
- **ingest** — m_shots shadowing + "green signal that does not cover the change"
- **ingest** — the split's one real bug, and what the verification was structurally unable to see
- **lint** — the LSP session was right and my rule was the weaker one
- **lint** — full link/orphan check after the split re-anchor: clean, one known-benign dangling ref
- **ingest** — live-edit surfel parity

### 2026-09-25
- **ingest** — exact selected-layer rotation + editor dashboard
- **fix** — preview hook cannot commit; pitch/roll merge parity
- **lint** — rotation/provenance wiki current
- **fix** — compact dual-panel HUD at 960x540
- **fix** — click-activated bounds-centered rotation trackball
- **fix** — logical input space and panel-safe ring capture
- **verification** — final trackball gates
- **fix** — camera-aligned gizmo and independent projection oracle
- **fix** — local-axis ring rotation
- **fix** — staged transforms and Move mode
- **perf** — short smoke test profile
- **verify** — final staged-transform and smoke-profile gates
- **ingest** — edge-aware surfel radius GUI control
- **verify** — edge-shrink release gate
- **fix** — edge-shrink live-path anisotropy guard
- **fix** — explicit multi-axis roof-edge detection
- **verify** — refined edge metric fast gate
- **decision** — focused test groups replace all-tests workflow
- **verify** — focused CTest group wiring
- **ingest** — GUI STL/OBJ importer + cabin reimport
- **harden** — shared mesh conversion + OBJ material paths
- **lint** — pre-existing missing wiki target
- **fix** — MCP import schema JSON
- **fix** — deterministic GUI mesh-import test hook
- **fix** — surfel-range API compatibility during concurrent edit
- **verify** — final mesh GUI and cabin gates
- **ingest** — hard-edge-only surfel fit + crease bridges
- **ingest** — HUD sidebar (single editor panel)
- **ingest** — X11 input injection for GUI verification
- **ingest** — demo capture tooling
- **fix** — flush, horizontally resizable docked sidebar
- **ingest** — smooth terrain edit brush
- **lint** — broken link in log.md
- **update** — input mechanism validated green; demo reel rendered
- **fix** — Smooth undo height-texture headroom
- **fix** — Smooth reload and terrain-texture hardening
- **ingest** — demo reel: hamlet keyframes + voxel/splat A/B variants
- **ingest** — Smooth generalises to object surfaces (surface-position relaxation)
- **ingest** — Add brush grows the surface along the picked normal
- **ingest** — tool-calling conventions
- **ingest** — per-voxel Add/Carve + a brush sized in voxels
- **lint** — frozen content names in visual_check (fixed at the class level)

### 2026-09-21
- **ingest** — live arcball rotation (GPU transform, no rebuild)

### 2026-09-20
- **ingest** — arcball layer rotation (full 3-axis placement)
- **ingest** — procedural tree generator (space colonization)
- **ingest** — mesh-to-voxel converter

### 2026-09-19
- **ingest** — per-cell texture (phase 2) wired end to end
- **ingest** — texture atlas (phase 1) documented
- **ingest** — photo textures shipped + GUI picker
- **decision** — assets are tracked now
- **ingest** — surfel holes on walls + stepped roof fixed
- **ingest** — texture resolution vs relief: measured
- **ingest** — texture detail normals (render flag bit 7)
- **ingest** — texture conformance gate + AI texture repair
- **ingest** — volumetric fog rewrite (J)
- **ingest** — water caustics + shoreline detail content

### 2026-09-18
- **fix** — Undo + "Clear live edits" (the emptied-chunk GPU patch), Add preview

### 2026-09-17
- **ingest** — Screen-space AO (world-scale) + the normal G-buffer
- **fix** — River water reaches carved channels (flood + carve scoop)
- **rework** — One fixed-level water plane (no flood, no water-grid patch)

### 2026-09-16
- **ingest** — detail roadmap + micros survive live edits (Phase 1)
- **ingest** — new default world: lakeside hamlet + old-scenery cleanup

### 2026-09-15
- **ingest** — edit-brush hover preview + Delete/Paint modes
- **lint** — stale pages after the surfel rework
- **ingest** — edits always live + water level respected when carving
- **ingest** — edits always live, digs below the water plane are flooded

### 2026-08-24
- **ingest** — PBR shading pass (ggx + sdf ao + sky ambient + aerial fog)
- **ingest** — realistic grass & foliage field
- **ingest** — high-res grass sprite cards
- **ingest** — grass disabled by LOD t0 bug + coverage rework
- **ingest** — voxel-object authoring skill + verification tooling
- **ingest** — layered world files (vxw split + manifest)
- **ingest** — alpaca paddock at the cabin
- **ingest** — world-layers GUI (ImGui) + hud capture hooks
- **ingest** — MCP server (vf_mcp) + gemma tool-bridge fixes
- **ingest** — AI edits are scene truth now (visibility fix)
- **ingest** — llama.cpp server support for the in-game chat



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

## [2026-10-01] ingest | splat base-seal architecture + the silhouette-fade frontier

Follow-up to the user report "splats are white at their edges, they should be
transparent to the edges". Added [[concepts/splat-base-seal]] and
[[concepts/splat-edge-fade-measurement]].

Key findings, all measured on the reference view against a 0.18/255 same-binary
noise floor (`interior` = pixels >=6px inside the splat footprint):

- The opaque base pass (PASS_MODE 4) is not just "the surface colour" - it also
  stands in for every fragment the depth band rejects behind it, since nothing
  occluded is ever drawn. So a translucent seal reveals the SKY, never the real
  background, and the prepass cannot answer "is this fragment supported?"
  (its depth includes the overhang).
- Radius-keyed fades (the shipped `VF_SPLAT_SIL_FADE` and my first two attempts)
  weaken the seal across all 3.4M surfels: interior +3.2 to +7.8/255,
  blue-dominant = the sky washing in. Radius-keyed bands cannot win.
- Split into two independent knobs and measured each: the edge window
  (`VF_SPLAT_EDGE`) costs +3.19/255 neutrally and buys NOTHING on its own
  (bleed 1.00x) because the base seal still covers the faded band; the seal
  decline (`VF_SPLAT_SEAL_ALPHA`) is the only thing that dissolves a silhouette
  (1.28x) and the only thing that costs interior fidelity (+5.6/255, 57%
  blue-dominant).
- The seal threshold is not tunable away: 0.65/0.8/0.9 render the SAME frame,
  and `VF_MICRO=0` does not help (5.96 vs 5.60) - the sub-centimetre grains
  were the obvious suspect and are innocent. The base pass only sees the single
  nearest fragment at a pixel.
- Found and fixed a real bug found on the way: the seal test was reading the
  WINDOWED alpha, which made the threshold a radius test in disguise.

Shipped DEFAULT OFF (both knobs 0), verified bit-identical to the pre-change
build: interior 0.1818/255 vs the 0.18 noise floor, bleed 1.00x. Gates:
test-surfel 4/4, test-visual 3/3, test-unit 8/8 green on an idle GPU.
The structural fix (a coverage count in the already-allocated D24_S8 stencil,
whose `recreateDepth` comment names this exact use) is staged, not implemented.

## [2026-10-03] ingest | Smooth brush quality + proportional Add/Carve
- Diagnosed the Smooth brush as a *despeckler*: its smoothing average was a fixed 3x3, so the brush radius never changed the averaging scale. Measured proof before any edit — `smooth centre sample: top=71 avg=71.000`, i.e. a ridge crest's whole neighbourhood is at its own height, so `lround` returned the current cell and the batch held zero edits at the crest. Every existing smooth test used a 1.5-voxel radius, which is why it survived.
- Replaced it with a separable Gaussian (sigma = clamp(radiusCells/2, 1, 16 cells)), cap justified by cost (`radius^2 · sigma^2`) rather than taste. Three coupled rules came out of the measurement, not from theory: a **constant** kernel normaliser (valid-weight renormalisation amplifies the pull toward whichever side survived an obstacle — the erosion artifact), a **step cap** replacing a clamp that was dead code but would have clipped the wide kernel back to 1 cell, and a **coverage gate**. The cap had to be raised 2 → 4 because the measured deviation of a brush-sized feature is 2.53 cells.
- A long-standing unit test caught a bug in my own volume-preservation term: a constant Taubin `mu` makes the gain negative where the brush falloff is small, so the tool *inflated* the rim of every bump into a moat (measured `dev -2.514 × gain -0.516 = +1.30`). Fixed by applying re-inflation as a fraction of the smoothing step, and shipped **off by default**. Recorded as a quirk: the right move was to fix the code, not to relax the test.
- Gave Add and Carve a radial falloff so the brush has proportional influence. Two measured traps: a decreasing exponent **inverts** the control for a base in [0,1], and a linearly-dropping rim puts the emit decision on a floating-point knife edge. The Add dome's *deliberate* flat top was reversed on purpose, so the header comment was rewritten to stop the next session restoring it.
- Also shipped: `VF_SMOOTH_PROBE` per-column dumps, a footer readout (columns / widest run / rise) so a sub-voxel relaxation is legible, a Smooth "Preserve volume" checkbox, `VF_EDIT_FALLOFF`, and the preview profile on `BrushUBO.bMeta.y`.
- Gates: `test-store`, `test-preview`, `test-live-edit` all green; `assets/runtime_edits.vxw` and `assets/world.json` verified byte-identical (md5) after every render run.

## [2026-10-03] ingest | Falloff curves, and what the surfel-normal measurement actually says
- Replaced the falloff *scalar* with six named curves (`Constant/Sphere/Root/Smooth/Linear/Sharp`). Diagnosed the real complaint — "inert/confusing" — as a parameterisation cliff, not a shape problem: `falloff <= 0` returned exactly 1.0 while `falloff = 0.001` was already 0.5 at half radius, so a 0.001 nudge jumped from "no taper" to "halved" and the rest of the range only steepened toward a spike. `Constant` is now a real curve, which deletes the discontinuity, restores the legacy flat-top Add, and enables flat-bottomed Carve.
- Threaded the curve through all four rasterizers (Delete/Paint now grade the radius), the panel (curve combo + `f(q)` plot + the effective reach in metres, because the tapered mark is genuinely smaller than Width), the footer, `BrushUBO.bMeta.y` and both shader backends.
- Also carried the Smooth dead zone into the UI rather than hiding it with a magic floor: a stamp moves `round(|deviation| x strength)` cells, so a 1-cell step (deviation ~0.5) needs **strength 1.0** to move at all. Stated in the panel, because a floor that turned a 0.1-cell intent into a whole cell would be a worse lie.
- **Surfel normals: measured, then partly fixed.** Found the store path was missing the bake's terrain-heightfield normal stage *entirely* — a parity gap by omission. Ported it against the store's own live tops. Measured on a 1-in-4 staircase ramp: 19.09° mean error with the stage absent, 17.90° at the bake's weight, 16.93° trusting the gradient outright, worst 65.2° in all three. The weight is not the limiting factor, so further tuning is not the fix: the residual is dominated by correct riser normals on a staircase, plus the neighbour pass diluting the blend to ~1 term in 5. Recorded the real fix (derive from the float pre-quantisation relaxed height, weighted by the same falloff, smoothed region only) rather than shipping a guess.
- Two fixture/instrument bugs found the hard way and recorded: a hand-built octree helper comparing a chunk-local top against a leaf-local `y` (made a "ramp" fixture solid to the ceiling, so a measurement silently measured a flat plane), and a world→cell conversion that dropped the `+WORLD/2` origin so every probe sampled 51.2 m away and read as valid air.

## [2026-10-03] ingest | edit-mode hotkeys + bottom hotkey bar

Added [[concepts/edit-mode-hotkeys]]. User request: `+`/`-` for edit tool size,
plus `M`=Move `A`=Add `C`=Carve `D`=Delete `S`=Smooth, and a bottom-of-screen
reminder of the currently available hotkeys.

Findings worth keeping:

- **`+`/`-` already existed** (`GLFW_KEY_EQUAL`/`KP_ADD`, `MINUS`/`KP_SUBTRACT`)
  and act only while armed; disarmed they are exposure. No change needed - the
  request was already satisfied, which is worth saying out loud rather than
  "implementing" it again.
- **`A`/`D`/`S` were WASD camera keys.** Confirmed with the user: mode keys win
  while armed, flight keys otherwise. Implemented as
  `Camera::moveKeysYieldsToBrush`, set every frame (never latched) so disarming
  restores flight instantly.
- **`updateCamera` runs BEFORE `handleHotkeys`.** Gating on `m_editActive` alone
  lets the camera strafe ~6 cm on the press frame before the tool arms. Fixed
  with a second edge-latch in `updateCamera` over the same keys (Tab included).
- **The `edge()` latch is stateful and single-read** - reading `edge(C)` once
  for the arm key and again in the mode loop makes the second call return false
  forever, so C would never select Carve. Caught it in my own first draft.
- **Mode switching had to be extracted** from the sidebar's `chooseMode` lambda
  into `App::chooseEditMode`, because that lambda refuses switches that would
  orphan a staged rotate/move or race a committing preview. A hotkey writing
  `m_editBrush` directly would have silently discarded state the panel protects.
- **`Tab` now toggles edit mode** (user's explicit instruction, which also
  explained their earlier "(tab)" reference). Tab was the *only* way to collapse
  the sidebar, so that moved to **`Ctrl+B`** rather than being dropped.
- **`M` (micro-surfel detail) moved to `U`** with the user's agreement, keeping
  the feature on a hotkey.
- **`ImFontAtlas::GetFont` does not exist** in this ImGui version - use
  `io.FontDefault` or `io.Fonts->Fonts[0]`.
- **Verification is expensive and partly environmental.** Headless `--shot`
  omits the ImGui HUD entirely, so the bar needs the X-display route (xwd +
  ffmpeg + python-xlib XTEST). Two of my own instrument bugs looked like app
  bugs: `keysym_to_keycode` returns the SAME code for '=' and '0' (must read the
  server's keyboard map), and vision captions of a 1600px-wide strip hallucinate
  (used numeric masks: text-cluster count, blue key-label pixels, amber
  active-chip pixels instead). A misdirected keystroke also closed the app twice
  mid-verification, invalidating two measurement rounds.
  resolution; the visual result is explicitly NOT claimed.
- **Caught three of my own bugs by reading rather than rendering**: `edge()` is
  single-read (C would never have selected Carve); `updateCamera` runs before
  `handleHotkeys` (camera strafed on the press frame); and the plate's layout put
  its lower edge ~3.5 px off-screen while overflowing the right edge at 960x540
  with a 300 px sidebar (found by replicating the arithmetic in python - no GPU).
- **A wrong comment, not a wrong line**: I wrote "unmodified B is unbound" for
  Ctrl+B, but bare B toggles detail normals. The code was right (the Ctrl block
  runs first and takes the edge, same as Ctrl+1..6 vs bare 1..5); only the
  comment was wrong. A comment that explains WHY survives a code change
  silently - the dangerous kind.
- **Two process failures, both mine, both recorded**: (1) I chained a GPU test
  group into a background command and it contended with a peer session,
  voiding THEIR render verdicts; (2) killing a ninja leaf does not stop a
  backgrounded wrapper - the wrapper just launches the next group. Correct order
  is wrapper, then ninja, then orphans; and `pgrep -f` matches the invoking
  shell's own command line.
- **The corrected measurement rule** (agreed with the peer session): contention
  invalidates a render measurement in EITHER direction - the same check measured
  137.9 s PASS and 198.4 s FAIL on one binary - so a contended PASS is not
  evidence either. Discard and re-run; judge by `pgrep`, never by timing.
- Added [[concepts/interactive-ui-coverage-gap]]: `drawHud()` runs only from
  `record_interactive.cpp`, so NO headless gate can see the sidebar or the hotkey
  bar. Two features landed today with zero coverage for that reason alone. The
  actionable fix is named in the page (`VF_TEST_PANEL=edit` calling `drawHud()`
  once) so it is a task, not a regret.

## [2026-10-03] ingest | Contention voids a measurement in BOTH directions; interactive UI is untested
- Corrected the verification rule in [[concepts/focused-test-groups]]. The old wording ("a red under concurrency is a contention hypothesis") quietly permitted banking a contended PASS, which is the more expensive error because it gets believed later. Measured proof that contention crosses the verdict boundary: the same check at 137.9 s PASS / 198.4 s FAIL on one binary. The rule is now **discard and re-run on a confirmed-quiet machine**; timing is the tell, `pgrep` is the evidence. Fingerprints recorded: unit_store_tests 37→96 s, fast_live_edit_check 23→78 s, unit_surfel_tests 85→150 s, preview_check 136.6→406.8 s.
- One session voided its own test-preview PASS after the runtime gave it away, and voided a test-surfel 4/4 for the same reason. Three different renderers shared the GPU this session: the user's own interactive app (which left a 40.6 MB runtime_edits.vxw — never touch it), a peer's GPU group chained into a backgrounded command, and a peer's splat-edge measurement app.
- Recorded a **standing repo-level coverage gap**: `drawHud()` → `drawSidebar()` is called only from `record_interactive.cpp`, so no headless gate can draw the sidebar. Two independent sessions each landed new panel UI with zero automated coverage for that one reason, and neither noticed until both looked. Named the cheap fix (`VF_TEST_PANEL=<section>` + a `drawHud()` call in the shot path) so the gap is a task rather than a regret; until then new panel UI is manually-verified-only.
- Process lessons from the same round: killing a backgrounded job's `ninja` leaf does not stop it (the wrapper bash starts the next group) — kill wrapper, then ninja, then orphaned children; and `pgrep -f "<pattern>"` inside a kill loop matches the invoking shell's own command line (one session killed its own shell mid-command). Bracket-escape or use exact PIDs.
- A peer's negative result changed the plan usefully: their interactive app window composites **black** into this X session, so an interactive smoke test can only crash-detect, not verify appearance — the panel appearance check was dropped rather than attempted.

## [2026-10-04] ingest | + / - bound by keycap, not by key position

User report: "+ and - shall change brush size in edit mode" — the second time
they asked, which was the tell that it did NOT work for them even though the
code read correctly.

Root cause: **GLFW reports physical X positions, not the symbol on the key.**
On this machine's German layout (`setxkbmap` = de,pc105):

| keycap | X keycode | XKB name | GLFW reports | old behaviour |
|---|---|---|---|---|
| `+` | 35 | AD12 | `GLFW_KEY_RIGHT_BRACKET` | grew splat disks |
| `-` | 61 | AB10 | `GLFW_KEY_SLASH` | nothing |
| numpad `+`/`-` | 86/82 | KPAD/KPSU | `KP_ADD`/`KP_SUBTRACT` | worked |

So the only "+"/"-" that worked were the numpad ones. Fixed with
`keyShowsChar()` (glfwGetKeyName + glfwGetKeyScancode): bind the key whose
*keycap* is `+`/`-`, wherever the layout puts it. The US `=`/`-` positions are
still matched, but only when the layout really shows those characters, so a
German dead key in the `=` position cannot spuriously resize the brush.

Two traps found on the way:
- **`io.InputQueueCharacters` cannot be read from the hotkey handler.** ImGui
  fills it from its event queue during `NewFrame()`, which runs in the *render*
  step of the frame (`record_interactive.cpp`), i.e. AFTER `handleHotkeys`.
  The first attempt at this fix read the queue there and always saw it empty —
  it looked correct and did nothing.
- The German `+` key **also** grows splat disks (it is the `]` position), so one
  press would have changed brush size and splat size together. The splat handler
  now yields that key while editing, and only when the layout puts `+` there.

Verified interactively (XTEST, keycode-accurate): Tab → armed;
German `+` `+` `-` → `brush size -> 19/20/19 vox` with no `splat radius` lines.
Brush/depth/smooth changes now log at all, which is how the check was possible —
previously they were silent, which is why the first round could not be verified.

## [2026-10-04] update | Tab is the only View/Edit switch

User report: "Tab should switch between View mode and Edit mode. In View mode,
other keys than tab should never switch the mode." The uncommitted mode-key
work let `C`/`A`/`D`/`S`/`M` arm the brush when disarmed (and `C` disarm on a
second press), so a stray `A`/`D`/`S` while flying dragged the user into Edit
mode. Fix in `src/app/frame/run_hotkeys.cpp`: the mode keys act only while
`m_editActive`; arm-on-press and the `C`-disarm special case removed, so Tab is
the single switch both directions. All five mode-key `edge()` latches are still
read unconditionally, so arming cannot make a stale latch phantom-fire. UI text
updated to match: hotkey bar (View mode shows only `Tab → Edit mode`, no mode
keys), Edit panel hint, sidebar footer tooltip, `AGENTS.md`, and
[[concepts/edit-mode-hotkeys]].

Verified live (XTEST, `tools/test_inject.py` updated): in View mode `C`/`A`/`M`
produced zero edit logs and the sidebar strip was pixel-identical; `Tab`
switched the panel to Edit (0.217 strip diff) and back. A second injection run
confirmed the armed half: `Tab` arms, `C` selects Carve and a second `C` stays
armed (no disarm), `A` selects Add, `Tab` disarms. `tools/vf_input.py` gained
the `tab` keysym; `tools/record_demo.py` now opens/closes the Edit panel with
`Tab` (both previously assumed `C`).

## [2026-10-04] ingest | decision-driven navigation (vf_nav) + ascii_view

User-driven session: "Start voxelforge. Use make_decision to navigate", then
"develop a fast decision routine based on make_decision and the ascii view".
Two tools now live in the workspace: `tools/ascii_view.py` (PNG → ASCII block
map + luma/blue/green stats; `block_map()` is importable) and `tools/vf_nav.py`
(`look`/`move`/`scan`/`run`; capture → digest → decide → XTEST move).
Decisions call the same tev1:0.8b model as the `make_decision` tool via a
direct POST to Ollama `/v1/systemone`: measured 0.10–0.68 s warm vs ~1.9 s
through `opencode-rag decide` (node startup). No vision model is in the loop.
Whole runs: 6 stop-heavy steps 25.8 s, 6 forward-heavy steps 37.2 s; after two
`forward`s through a dark close-up the camera emerged over open water (blue
20–38 %) and the model then chose `forward` steadily. Gotchas filed as quirks:
`wmctrl -i -a` before x11grab (a stacked terminal was captured as a "dark
frame" for minutes), tev1 stop-bias + anti-stuck guard. Also measured: a plain
interactive launch with `VF_OVERLAY_PATH=<copy>` left the copy byte-identical
(mtime unchanged) over ~17 min — the rewrite-on-load shrink did not fire
without an edit/save trigger. See [[concepts/decision-driven-navigation]].

## [2026-10-04] ingest | Tower/cabin rebuilds + nature layers + the surfel thin-rule roof bug
Rebuilt `hamlet_tower` (hollow round watchtower: arched doorway, six glazed
windows, wooden decks, continuous stone spiral stair on a central newel,
corbelled head, crenellated parapet, solid shingled spire, emissive
lanterns, interior props; ~92k voxels) and `CabinPart1` (log cabin: stone
foundation, stacked logs with chinking + corner end grain, gable roof with
rafters/coursed shingles/moss, chimney, porch, woodpile, lit hearth,
interior furniture; ~36k voxels) via new scripts `tools/hamlet_tower_v2.py`
/ `tools/cabin_v2.py`, plus two new layers `hamlet_nature` (bushes, ferns,
flowers, mushrooms, boulders, logs, stumps, tufts, canoe, standing stones)
and `hamlet_forest` (7 deciduous trees) via `tools/hamlet_nature.py`.
Root-caused the "sparse field of splats" roof report: the surfelizer's
thin-structure rule classified 2–3 cell shells (cone, roof skin) as thin and
emitted 5.5–7.5 cm ellipses that cannot seal staircase steps; fixed by
authoring both roofs as solid/thick masses (50 % → 3 % thin; cabin 43 % →
2 %). New page [[concepts/surfel-thin-rule]]; quirks filed for the rule and
for `write_object` preserving manifest placement.

## [2026-10-04] lint | visual_check house sky-probe failure is pre-existing
`test-visual` came back 2/3 (gpu_selftest + fast_visual_check pass); the
failure is `visual_check` -> "house: sky probe not blue-dominant" (top 1/8
strip 41.9 % blue, needs >50 %). A/B with the committed layer file
(`git show HEAD:assets/hamlet_tower.vxw`) renders 42.38 % — the same failure —
so the rebuilt tower (~0.5 pt wider plinth) is not the cause. With the tower
disabled entirely the strip is 49.92 %, i.e. still below the threshold: the
probe is a composition/camera issue (tower shaft + conifers fill the strip;
the tower top is above the frame, so tower height is irrelevant). Hero 96 %
and water 100 % pass. Quirk filed; the fix belongs to the camera/probe, not
the content.

## [2026-10-04] ingest | Rebuilds re-filed as new layers (hamlet_cabin, hamlet_tower_v2)
Per user request the rebuilt cabin/tower now live in NEW layer files and the
originals are disabled instead of overwritten: `assets/CabinPart1.vxw` and
`assets/hamlet_tower.vxw` were restored to their committed content
(`git checkout --`) and set `enabled:false`; `tools/cabin_v2.py` now writes
`hamlet_cabin.vxw` (36,367 records) and `tools/hamlet_tower_v2.py` writes
`hamlet_tower_v2.vxw` (86,344 records, pos [-8.65,-0.80,-7.40] copied from
the old tower entry). `hamlet_cabin` was moved to the first object-layer slot
so visual_check's ownership subject (first enabled object layer) stays the
cabin. This also makes both rebuilds visible as new rows in the sidebar World
list.

## [2026-10-04] ingest | Dense forest + tower moved into it
`tools/hamlet_nature.py` now samples the terrain from `assets/heightmap.png`
for every item (no more hardcoded ground heights) and grows a **dense
forest**: 26 deciduous trees (was 8) — a ring around a clearing at (15, 14)
plus west/north/south-east belts, with 60 forest-floor items (ferns, bushes,
mushrooms) scattered under the canopy; `hamlet_forest` is now 96,812 voxels,
`hamlet_nature` 11,182. `hamlet_tower_v2` moved from (11.25, 10.5) into the
clearing via `pos [-4.90, -0.51, -3.90]` (base sits on terrain -0.11).
Placement guards: skip keep-out (x -3..3, z -1..6) and terrain < -0.5 m;
overlap checks now show 0 forest/tower cells and 4 nature/tower cells (a
flower patch edge, invisible). Verified with probes (shaft/deck/cone solid,
interior air) and the forest_tower render — the tower rises out of the
canopy. Position/enabled-layer quirks updated to the new state.

## [2026-10-04] ingest | Denser forest, gravel path, forest-floor litter
`tools/hamlet_nature.py`: trees 26 -> **38** (second ring + west/north infill,
guards: keep-out, underwater, tower clearing r 4.4 m, >=2.6 m trunk spacing);
159 forest-floor items (5 per tree); dark moss/leaf-litter patches flush with
the terrain under the canopy (so the forest floor reads darker; tree shadows
from the CPU shadow bake do the rest); and a **gravel path** (~10.7k cells)
following two polylines - a loop around the tower clearing, a tail north and
a branch west to the standing stones - routed around the cabin/garden/boat
boxes, the tower plinth and hard props, flush with the terrain (mean +0.04 m).
New `tools/gen_gravel.py` writes a tileable pebble texture; world.json binds
it as mat 15 (`textures/gravel.png`, 0.9 m/tile; path cells keep mat 5 +
tex=15, so nothing emissive). Also bound `textures/stone_plaster.png` at
mat 20 in the same write (replaces proc_plaster; asset-fixer session supplied
the file). Forest 155,352 voxels, nature 29,991.

## [2026-10-04] ingest | Soil watermark stripped; Designer textures imported
`assets/textures/sand_highres.png` (bound to mat 2 "soil") carried a "Made
with AI" pill badge at y 9–50 / x 858–1013. `strip_stamp` detection missed it
(blurred-luma high-pass peaked at 0.094–0.118, under the 0.12 threshold, and
even 0.06 found nothing) — the conservative fallback box is what removed it;
`check_texture` corner share went 56% → 2%. The pre-existing seam (1.50x) is
untouched. Three more user drops from `~/Downloads` were prepared and placed:
`gravel.png` (user's Designer.png, replaces the world-builder's generated
placeholder; mat 15, 0.9 m/tile), `stone_plaster.png` (mat 20, replaces
proc_plaster) and `linen_designer.png` (Designer(2).png; woven ~6.5 px
period; no binding). All PASS `check_texture`. A referenced `linen.png` does
not exist on disk yet. See [[concepts/texture-conformance]].

## [2026-10-04] ingest | Second wood, cabin->tower path, cobblestone river street
`tools/hamlet_nature.py`: new species in `KINDS` (beech, pine with layered
tiers, willow, poplar, alder, ancient_oak) and a second layer
`hamlet_forest2` (17 large trees, 80,715 voxels; two layers keep each write
under the 200k cap). Tree placement now also skips the cabin/garden
footprints. Path reroute: the gravel way now leads **from the cabin door
(5.2,14.8) to the tower door (13.0,12.0)** - nearest gravel cell 0.14 m from
the door point - then the tower-clearing loop, a north tail, a branch to the
standing stones, and a ring->street connector. New **cobblestone street**
along the river (tex=14 -> `textures/light_rock.png`, 0.8 m/tile, bound in
world.json): 4,949 cells, world x -11.2..22.8, z 6.2..12.8, wider (r 1.0-1.3
m) than the gravel path and with no verge plants. Guard updates: tower
clearance 3.3 -> 2.95 m so the path can reach the door, cabin box z -> 14.5,
canoe obstacle 2.6 -> 1.8. nature 37,426 / forest 124,061 / forest2 80,715
voxels; no keep-out cells; forest2/tower overlap 0.

## [2026-10-06] ingest | Enclosed-space shading and light sources
Caves read as open sky because `skyIrradiance` + the IBL cubemap carry **no**
occlusion term and baked AO only reaches 0.6 m. Added a bit-8 one-ray
enclosure test (`skyVisibilitySvo`/`skyVisibilitySPlat`) that scales sky
ambient to 0.16 and IBL to 0.04 in enclosed space, plus an albedo-scaled cave
fill — without the fill the `house` shot went to 7.21 % black-in-silhouette and
failed the 5 % gate; with it, 4.26 %. The splat sky test uses `objDist` only
(`heightAt` calls every point below the terrain surface solid, so hillside
interiors read as underground); the per-light shadow test does march terrain.
New light-source lane: top-level `"lights"` in `world.json`, parsed by
`worldfile::loadLightManifest` into a 528 B std140 UBO at binding 25 and
consumed by `applyLights()` in `common_base.glsl` for both backends. Measured
with one cabin lamp: mean luma 42 → 87, pixels < 30 44.9 % → 11.0 %, splat↔SVO
delta unchanged (-16.7 → -14.3). First light test was a silent no-op on the
splat path because `setLights()` ran before `m_splatPass.init()`; the single
writer `App::uploadLightSources()` now runs after every pass init.
`test-unit`/`test-app` green; `test-visual` black-in-silhouette green. The two
`visual_check` sky-probe failures (house, water) are **pre-existing**: they
reproduce identically with `VF_RENDER_FLAGS=255`, i.e. with the new shading
disabled, and come from the current scene content, not this change.

## [2026-10-06] ingest | The visual_check sky probe is a camera + content assertion
Re-measured the two `visual_check` sky-probe failures (house, water)
independently of the shading session that first hit them, with one
`--shotlist` process per configuration at 480x270 (`VF_NO_OVERLAY=1`,
`VF_OVERLAY_PATH=/tmp/opencode/skyprobe/...`). Default vs the bit-exact
off-state `VF_RENDER_FLAGS=255`: hero 100.0 % / 100.0 % PASS, house 44.0 % /
44.7 % FAIL, water 45.2 % / 45.2 % FAIL — the whole shading change is worth
**+0.6 pp** on house and **0.0 pp** on water against a 6 pp shortfall, so the
probe is not measuring shading. It is not even measuring sky: the assertion is
`b >= r` for >50 % of the **whole top eighth** (top 33 rows, 15840 px), a
weaker test than the `is_sky` classifier used for coverage 20 lines above it,
so it really asks "is the top of this camera's frame mostly blue". Classified
those strips instead of captioning them: house 46.1 % warm geometry (the
hillside behind the cabin) + 9.8 % green + 22.3 % blue-but-not-strict +
21.7 % strict sky; water 41.0 % warm (far bank) + 13.8 % green + 34.0 %
blue-but-not-strict + 11.2 % strict sky; hero 97.6 % strict sky. Filed as
[[concepts/sky-probe-is-a-camera-assertion]] with the two honest fixes
(gate the probe on a real sky floor, or re-aim the shots) and the cheap
refutation rule — take the off-state control before blaming a lighting change,
because the escape hatch makes that refutation one render. Same run also
confirms the direction of the peer's black-in-silhouette claim: house 4.29 %
default vs 2.92 % off (they measured 4.26 % / 2.70 %; drift = the hamlet
content commits since).

## [2026-10-06] lint | Wiki link check: 42/42 resolve; the one dangling link is closed by decision
Ran a full wiki-link resolution pass over all 43 pages (double-bracket links,
script at `/tmp/opencode/wiki_lint.py`, outside the repo). Result: **zero
broken links outside `log.md`, zero orphan pages, zero `sourceRefs` paths that
no longer exist.**

The single dangling target is `[[concepts/water-flooding]]`, referenced from
historical entries here (2026-09-15 and 2026-09-17) and it is **accepted, not a
defect**: the 2026-09-17 rework deleted `App::floodNewlyDug` and the per-column
water bookkeeping entirely, and `concepts/water-plane` replaced that page. The
link may not be repaired by inventing a `water-flooding` page — that concept no
longer exists in the code, so a page about it would be a page describing a
removed design. `log.md` is append-only, so the historical references stay.
Closing this here so future lint passes do not re-open it as new work (this is
the fifth time it has been re-reported: log.md:754, :873, :1901, :2108, and now
here — a lint finding nobody closed was re-discovered every few days).

Caveat on the staleness half of the lint: "sourceRefs newer than the page"
currently flags **35 of 43 pages**, which is noise, not signal — the working
tree has uncommitted edits across `src/`, `shaders/` and `assets/` from three
concurrent sessions, so nearly every source file has a newer mtime than any
page. That check is only meaningful on a clean tree, or when re-based against
git commit dates instead of mtimes. Left as-is: it is a heuristic, and this
entry is the note.

## 2026-10-06 ingest | sun-direction-pipeline — kSunDir production & consumers
New page `concepts/sun-direction-pipeline.md`: maps the full sun direction
lane from CLI `--sun` / manifest `"sun"` block through `App::m_sunDir` and
the push constant to every shader consumer (skyColor, shadeTerrain/shadeSurfel,
shadowMarch, water glint, fog, sky irradiance, SSR). Includes the
reference-shot gate warning (a `"sun"` key shifts every baseline) and the
loadSunManifest trap-and-fix (half-written block used to silently park the
sun at azim 0; now requires gotElev && gotAzim, pinned by test_worldfile.cpp:785).
Cross-linked from index.md concepts list + navigation.

## 2026-10-06 ingest | overlay-silent-write-trap — harness can destroy ignored state
New page `concepts/overlay-silent-write-trap.md`: the VF_NO_OVERLAY-vs-VF_OVERLAY_PATH
data-loss trap. VF_NO_OVERLAY=1 suppresses only the load; stamping renders still write
assets/runtime_edits.vxw unless VF_OVERLAY_PATH is set. The 2026-10-06 64MB→980KB loss
as the positive firing test. Fix landed (render() defaults VF_OVERLAY_PATH into tmp),
env-level proof verified, test-live-edit proof still owed. Cross-linked from index.md
concepts list + navigation.

## 2026-10-06 ingest | sun-key A/B measured — which gate catches day/night
`concepts/sun-direction-pipeline.md` upgraded from an unmeasured warning to a
measurement. Three arms at 640x360 on the hero cam (splat): (a) no `"sun"` key
/ CLI default 34/238 = luma 120.43, dark% 0.01, blue% 32.1 (null control —
must reproduce the current shots exactly); (b) elev 4 azim 240 = 100.60 /
0.03 / 46.1; (c) elev -30 azim 96 = 32.92 / 23.49 / 56.7.

The finding: the two `visual_check` assertions split. The **sky probe cannot
detect day/night** — the blue share *rises* (32.1 -> 46.1 -> 56.7 %) so it
passes at elev -30, because `b >= r` is brightness-independent by construction
and tests warmth, not daylight (night sky `(12.5,23.4,37.0)` satisfies it as
strongly as day `(127.8,137.9,133.9)`, by 6 codes). **Black-in-silhouette is
the assertion that moves** — 0.01 % -> 23.49 %, past the `< 5 %` gate by
~4.7x. So a night frame fails visual_check on silhouette, not sky, and a gate
pinned only on the sky probe reports a night frame as healthy. This is the
strongest form of the claim in concepts/sky-probe-is-a-camera-assertion.

Numbers are labelled George-reported, not measured by this session; mixing
arms across trees inherits content drift (cf. the 4.29/2.92 vs 4.26/2.70
drift), so the null control must be re-measured as an arm of the same A/B.
Recorded separately by page owner: the b>=r percentages and the two-outcome
framing went into concepts/sky-probe-is-a-camera-assertion, the elevation and
silhouette delta here. Also pinned by the day/night work: elev -14/azim 96 is
a valid night (below-horizon elevation deliberately not clamped), and a
half-written `"sun"` block now returns false with a warn.

## 2026-10-06 ingest | heightfield-blindness-enclosure + uncommitted-edit-is-not-yours

Two new concept pages.

**concepts/heightfield-blindness-enclosure** — the splat backend had two
independent occlusion blind spots, both fixed in-tree, both of which returned
"no-op" rather than an error. (a) Enclosure: `aoShEnclosure` was pinned to 0
for a carved terrain cave because the object volume cannot see terrain that was
carved away AND the bake skips the shadow march on backfacing surfels
(`sh=1`), which is most cave walls — so the one field that knew about the carve
was forced to 1 exactly where it was needed. 45.26 ON vs 45.84 OFF (a no-op)
became 36.51 ON vs 45.84 OFF, with the OFF path byte-identical so
`VF_RENDER_FLAGS=255` is now honest. The fix adds a heightfield term to
`skyVisibilitySplat` testing only the START column (a heightfield has no
overhangs, so terrain cannot hide a ray that already left the surface) with a
0.35 m clearance, and switches `shadeSurfel` to the RAW baked shadow `shRaw`
rather than the caller's gated one. (b) Per-light march: a lamp inside its own
carved cave moved the splat frame by 0.00 mean luma while SVO moved 0.51,
because underground `sHf` is negative at every tap so the first tap returned 0.
Fixed by skipping the terrain tap while BOTH endpoints are below their local
column (a segment that never crosses the surface cannot be crossing terrain);
the step size switches to the object field alone so the march still arrives.
Lamp at a verified in-cavity point: splat 36.51 -> 54.12 (warm% 0 -> 17.1),
SVO 48.58 -> 100.85 (warm% 0 -> 77.7); an out-of-range lamp changes nothing in
either. OPEN: magnitudes still differ (splat +17.6 vs SVO +52.3 mean luma).
The page states the pattern — the bug class is a test whose BLIND case returns
the same value as its PASS case.

**concepts/uncommitted-edit-is-not-yours** — a leading ` M` means the file is
not yours to restore. `assets/world.json` (md5 `dac9f059`) was a pre-existing
uncommitted edit by an unknown session (keys are only version/layers/textures,
so the edit is inside the layer list or the textures table); a stray
`git checkout` destroyed it and it was recovered byte-exact from a scratch copy.
Attribution recorded as UNKNOWN on purpose: if a later session believes the
shading session owns it, it may feel free to clobber it.

**Attribution finalised (later the same session).** Re-verified md5
`dac9f0592bd8d408879b269676e2109e`, valid JSON, still ` M`, byte-identical to
what was found. The framing is now **settled in state, unresolved in
ownership** — the session that destroyed it never saw the author, so there is
nothing to resolve it with. Do not upgrade it to an attribution and do not
read it as still-in-progress. The load-bearing lesson is the scratch copy, not
the hashing: the restore would NOT have worked without the
`cp -r assets/. /tmp/opencode/scene/` made minutes earlier for an UNRELATED
reason. The md5 verified the recovery; it did not achieve it. Prevention is
now "copy the tree aside before you touch it", not "compare hashes".

Also corrected in concepts/sun-direction-pipeline: the earlier "no moon/night
code exists / the worldfile.hpp comment overstates" caveat is STALE and removed —
the night lane is real (`nightFactor`, `sunFade`, `moonDir`, `kMoonCol`,
`moonLight`, `applyNight` in common_base.glsl, called from both sky variants
with `moonLight` reaching both splat paths). Recorded verbatim: `kMoonCol` was
0.62 first, gave mean luma 46 and read as dusk — 0.14 is settled, do not
"restore" 0.62. And a provenance note: assets/world.json is SUNLESS, so the
A/B arms came from CLI `--sun` flags, not a manifest edit; reference shots are
unchanged.

## [2026-10-07] fix | the day/night switch page claimed "async reload" — it is a stall

Robin implemented the Day/Night switch (Render panel, hotkey **P**, `App::setSunPhase`
snapping `m_sunDir` between day 34/238 and night −30/96) and caught an error in
my page: it said the reload was asynchronous. **That was wrong, and it was mine
— inferred from plumbing rather than traced.**

The wrong inference: `LayeredWorld::kick` really does thread the SVO rebuild on
a worker (unless `VF_SYNC_RELOAD`). But the frame that *applies* it,
`App::applyWorldReload` (src/app/world/world_layers.cpp), calls
`vkDeviceWaitIdle` and then `rebuildSurfels()` — and the per-surfel sun shadow
is CPU-baked at surfelize time. The stall lands on the applying frame, so a
toggle costs a hitch of roughly the same order as the `U` micro-detail toggle.
`requestWorldReload()` setting a flag means *queued*, not *converging*; nothing
chases it. Sky and direct light move next frame because `m_sunDir` rides the
push constant; the baked shadows do not move until the rebake finishes.

Recorded as the page's load-bearing warning, plus the two facts that make both
arms checkable without the GUI: `VF_TEST_SUN_PHASE=day|night` drives the same
`setSunPhase()` path as the buttons, and `--shot` **cannot** prove the buttons
at all — `drawHud()` runs only from `record_interactive.cpp`, the standing gap
in [[concepts/interactive-ui-coverage-gap]]. So the switch's *rendered result*
is headlessly provable; its *UI* is manually verified only.

Also pinned here: the switch is **session-only, deliberately**. It never writes
the phase to `assets/world.json`. That file still has no `"sun"` key, which is
what keeps every reference shot at 34/238.

## [2026-10-07] decision | editing lights needs a writer that does not exist yet

User request: add a light at the selected voxel, and move lights in Move mode.
Investigating first turned up the reason this is not a button: **there is no
`writeLightManifest`.** Only `loadLightManifest` exists, and
`App::uploadLightSources()` **re-reads the manifest** — so the sole way to add
or move a light today is to write `assets/world.json`, the tracked file that
still carries an unknown session's uncommitted edit
([[concepts/uncommitted-edit-is-not-yours]]).

George's contract for the writer, now on
[[concepts/enclosed-space-lighting]] and gated before any code:

- **(a) preserve every top-level key it does not own** (`layers`, `textures`,
  `sun`) — copy `writeTextureManifest`/`writeManifest`, never a fresh
  serialize; a parsed-struct reserialize silently drops keys the parser does not
  know about.
- **(b) temp + rename**, never a bare `std::ofstream` — `tests/test_authoring.cpp`
  truncates `world_all.json` exactly that way.
- **(c) a round-trip unit test in `test_worldfile`** writing a manifest with an
  unknown key and asserting it survives byte-for-byte. **Loader-only until this
  exists**, because (a) and (b) are invisible in a screenshot.

A moved light is **staged, not written through** — preview live, commit on
Apply, matching the trackball and Move. The load-bearing part is that the
preview patches `LightUBO` in RAM (binding 25, the same in-RAM lane the sun
uses), which makes Apply the **only** call site of the writer, so no drag path
can reach disk and the UBO cannot drift from the manifest.

Move mode has no lane for lights: it drags a selected `.vxw` layer owner, and
picking resolves to a voxel plus its winning `.vxw` owner (never an AABB), so
lights need a second, non-voxel selection answer. `VF_TEST_*` runs must never
reach the writer.

Implementation is Robin's (they hold the `panel_render.cpp` / `run_hotkeys.cpp`
/ `app.hpp` leases); George reviews before build. This session is on docs only.

## [2026-10-07] gotcha | writeTextureManifest is not atomic — the precedent's failure mode was the trap

Robin caught the mistake before it shipped into
[[concepts/enclosed-space-lighting]]: the page said "copy
`writeTextureManifest`'s behaviour", which is right about **key preservation**
and dangerously wrong about **how the bytes land**. Verified in
`src/voxel/worldfile.cpp:813` — both manifest writers do:

```cpp
std::FILE* f = std::fopen(path.c_str(), "wb");   // truncates HERE
... fprintf the whole document in place ...
return std::fclose(f) == 0;
```

`"wb"` **truncates the live manifest before the first byte of the replacement
exists**, and `fclose(f) == 0` cannot detect a partial write — `fclose` succeeds
on whatever it flushed. An interrupt, crash, or full disk therefore leaves a
**truncated `world.json` with no error reported anywhere**. That is precisely how
`assets/world_all.json` got mangled today (`test_world.cpp` /
`test_authoring.cpp` rewrite it through a bare `ofstream`), and it is why
`OverlayWriter` uses temp + rename.

The precedent therefore splits: reuse `scanTopLevel` + `emitPreserved`
(balanced-brace aware, so nested objects and odd spacing survive verbatim),
reject the write mechanism. `writeLightManifest` writes `path + ".tmp"`, checks
`fclose`'s return and removes the temp **before** the rename — renaming first
and checking after would promote a partial temp over a good manifest, trading a
truncated file for a differently truncated one. It also clamps to `kMaxLights`,
because `loadLightManifest` drops the excess: emitting 17 would persist a lamp
that silently never renders, leaving the manifest disagreeing with the UBO with
no error anywhere.

Recorded on the page with the transferable lesson: **verify a precedent's
failure mode, not its shape.** This is the second time this session that a
plausible-looking precedent was the wrong thing to follow — the first was
calling a CPU-baked rebake "async" because the surrounding plumbing was
threaded.

## [2026-10-07] lint | svo-render.md: schema bug plus two stale claims

Robin found the schema bug and George handed me the refresh. `entities/svo-render.md`
carried `name:` in frontmatter where all 46 other pages use `tags:` — the only
page in the wiki with it. Fixed, and it was the only occurrence.

Refreshing it then turned up two claims that had been wrong for a month, which
is the real finding: **a schema bug hides content rot, because nothing was
reading the page's structure closely enough to notice the body.** Both are now
recorded on the page as "no longer true" so they are not re-added:

- **"ACES"** — the raymarcher outputs **linear HDR pre-tonemap** and the post
  pass applies exposure, **AgX**, bloom and the outline. The `aces()` in
  `common_base.glsl` is dead code on this path.
- **"shadows march `uObjVol`"** — `softShadow` is a **16-tap PCF over
  `exactSVOHit`**, exact DDA traversal of the sparse octree; terrain uses
  `softShadowTerrain`. The coarse r8_snorm 256³ object volume is only marched on
  the GPU for the **splat water** path now, and splat surface sun shadows are
  baked per-surfel on the CPU. No `objDist`/`uObjVol` reference exists in
  `common_svo.glsl` at all.

Also: `sourceRefs` now includes `shaders/common_svo.glsl` (the traversal
actually lives there, not in `svo_raymarch.comp`, which is only the entry
point), and the `RaymarchPush` note now carries the `misc.y = animTime_s`
correction. Added the day/night parity pointer.

## [2026-10-07] measure | sunset/sunrise: the gradient is right, the magnitude is not

New page `concepts/sunset-dimming.md`. Measured at 320×180, camera `y=40`
pitched 20° up so the frame bottom (−10°) meets ground at `40/tan(10°) ≈ 227 m`,
outside the 102 m world — every sampled pixel is sky.

**Finding.** The golden-hour gradient *exists and has the right sign* — warm at
the horizon toward the sun, blue above — but it is roughly **3× too weak to
read**. Peak horizon `R−B` toward the sun is **+12.6/255** at elev 2, against a
high sky of `R−B = −38` directly above it. A warm smudge a third the strength
of the blue above it is haze, not a sunset; a convincing one wants +60…90.

**Second finding, and the more serious one: the sky never dims.** Away-from-sun
luma runs 164 (elev 34) → 170 (12) → **172 (4)** → 161 (0) → 141 (−2). It is as
bright at elev 4 as at noon and merely desaturates to neutral grey; warm share
away from the sun is **0 % at every elevation**. So the frame gives no sense of
the day ending.

Three causes, all in `skyColor`/`skyColorFast`: (1) `base` mixes **fixed**
`kHorizon`/`kZenith`, so nothing scales with sun elevation; (2) `horizBand =
pow(1-cosTheta, 3.0)` is cubic and confines the tint to a thin strip; (3)
`applyNight` starts at `kSunDir.y = 0.06` (elev ≈ 3.4°), so the whole 12°→3°
window where a real sunset happens has **no transition in it** — the only
low-sun change there is an additive tint, never a dimming.

Proposed to George (his lane, not landed here): a daylight factor on `base`,
`mix(0.55, 1.0, smoothstep(-0.02, 0.35, kSunDir.y))`; a wider `horizBand`; a
stronger `sunsetTint`; and a dusk ramp separated from `applyNight`.

**No reference shot frames a sunset** — the hero cam looks `+x,+z` while the sun
sits at azim 238 (`−x,−z`), so `visual_check` is blind to all of this and stays
green regardless. Same class as the sky-probe gap, one step further out.

**Probe hygiene (took three runs; two were void).** A *partial* `--cam` token
silently discards the target — three values fill `camx/camy/camz` and leave
`tx/ty/tz` at defaults — so "toward sun" and "away from sun" rendered
**byte-identical**, aimed at terrain. The tell was that two viewpoints which
must differ produced *every row equal*. And `describe_image` confidently
mislabelled the frame as a "top-down aerial view… with a blue river". What made
run 3 trustworthy: a camera height that puts the ground out of frame regardless
of angle error, plus a **falsifiability gate** (day34 anti-solar sky must be
blue or the script exits non-zero) so a broken probe fails loudly instead of
printing a clean table.

## [2026-10-07] verify | sunset lane closed green; wiki lint clean at 50 pages

George implemented and measured the sunset change; `visual_check` green and
**byte-identical to the pre-change run** on the daylight gates (coverage
70.8/94.4/96.6, black-in-silhouette 0.18/4.29/2.25 %, ownership + present probe
unchanged; only the two known pre-existing sky-probe fails on house/water).
`night_check` PASSED with references unchanged. Byte-identical daylight is the
outcome that matters: the change moved exactly the frames it was meant to and
nothing else.

Sunset gates, all met: away-from-sun luma strictly monotonic
155.25 → 151.41 → 143.90 → 133.50 → 110.43 → 89.92 (34/12/8/4/0/−2), ratio
**0.711 < 0.75**; toward-sun saturation **0.168 ≥ 0.16** at elev 2 with
`R−B = +63.3` (target +60…90); high sky blue-dominant at every elevation.

**Gate 5 — the one worth remembering — is now measured rather than argued.**
`night_check` on the post-sunset tree gave hero 22.96/0.185, house 15.11/0.143,
water 25.60/0.229, *identical* to the pre-sunset references. That gate existed
to force exactly this: a daylight factor that touched the moonlit night would
have failed there instead of being noted in review and forgotten.

Wiki lint (50 pages): frontmatter complete on all 48 non-index/log pages, **no
dangling links**, **no orphans** other than `log.md` itself (nothing links *to*
an append-only timeline — correct), and **every `sourceRefs` path resolves**. The
one known dangling target, `[[concepts/water-flooding]]`, remains accepted by
decision: the concept was deleted in the 2026-09-17 water rework and `log.md` is
append-only, so repairing it would mean inventing a page about a removed design.

## [2026-10-07] gotcha | a shader edit does not touch the binary — verify identity, not health

Relayed by Robin via George after a ~5 min shader window (`kMoonCol` briefly
`0.62` to revalidate the night-gate tripwire, since the `0.323` figure had been
measured on the **pre-sunset** sky and a stale figure must not read as current).

**A `.glsl`/`.comp` edit does not change `build/voxelforge`.** The binary's
md5 was **identical** across both windows (`07306f1c…`) — shaders compile to
`build/shaders/*.spv` and load at runtime, they are not linked into the
executable. So during a shader window `md5sum` on the binary is clean, its mtime
is old, `strings` finds nothing new, `ninja` may say "no work to do" — and the
**render is wrong anyway**. The only tell is `build/shaders/*.spv`.

This is worse than an ordinary stale-build trap: every cheap verification a
person reaches for returns a clean answer, and a stale `.spv` yields *plausible*
pixels from the previous shader, so nothing downstream flags it. Recorded on
[[concepts/measurement-discipline]] as **confirm the instrument's identity, not
its health**, with the required proof shape for closing a shader window:
`md5sum -c` on the `.spv`, a diffstat matching the expected baseline (222
insertions / 10 deletions for the post-sunset tree), and a grep of the **source**
file for the reverted constant (`kMoonCol` reads `0.14`) — never of the binary.

Session did not render inside the window, so no measurement here is affected.



## [2026-10-07] ingest | Day/night switch implemented (uncommitted); measurement-provenance page filed

**Implemented** (builds clean, 33/33; approved by the shading session, no
changes requested; **not yet committed** — the four files carry peers'
uncommitted hunks and cannot be committed in isolation):

- `App::setSunAngles(elev, azim)` — the app's single elevation/azimuth →
  direction conversion. `--sun`/manifest startup, the Render-panel sliders, the
  preset buttons and the `P` hotkey all funnel through it, so a convention change
  cannot be applied to some controls and forgotten in others.
- `App::setSunPhase(bool night)` — snaps day 34/238 or night −30/96, then one
  `requestWorldReload()`. The reload is **required, not cosmetic**: each
  surfel's sun shadow is CPU-baked at surfelize time, so without it the direct
  sun moves and the shadows do not. Same deliberate stall as the `U` micro-detail
  toggle.
- Two buttons above the sliders as the coarse switch; the sliders stay the fine
  control. Active state is derived from `m_sunDir.y` with the same −2.0°
  threshold as the hotkey, so button and hotkey cannot disagree.
- `P` toggles, reading the phase back out of `m_sunDir` rather than keeping a
  parallel bool.
- `VF_TEST_SUN_PHASE=day|night` drives the **same** `setSunPhase` path as the
  buttons, so the switch is provable headlessly instead of resting on "the GUI
  probably calls it".

**Session-only by decision.** It does not write the chosen phase back to
`assets/world.json`, because a runtime toggle rewriting the shared manifest is
exactly the failure mode that destroyed an unknown session's uncommitted edit
earlier today. The sun figures therefore came from the switch and `--sun`, not
from a manifest key, and `assets/world.json` remains sunless (`layers`,
`textures`, `version`) — so the reference shots keep elev 34 / azim 238 and their
coverage and black-in-silhouette numbers are unchanged.

**Verified** by two arms through `VF_TEST_SUN_PHASE` (640×360, reference cam,
`VF_OVERLAY_PATH` isolated per arm): day 113.84, night 31.63 mean luma; night is
clear of the ~46 band a `kMoonCol` regression to 0.62 would produce, and
`assets/runtime_edits.vxw` md5 `a7daecd5` (24,636,160 B) was unchanged across
both. Full provenance, metric definitions and the refuted overlay hypothesis now
live on [[concepts/sky-probe-is-a-camera-assertion]].

**New page — [[concepts/measurement-provenance]].** Generalised from the
retraction below: a number is not a gate until it carries its command, its metric
definition and its tree state.

**Corrected my own page.** The sun-arm table's camera attribution
("640×360, hero cam `1.0 2.0 1.5 → 5.3 1.0 11.3`") was written from memory with
no artifact, and a later session chose its own camera *by reading that line* —
so the two pairs were never matched. It is now marked **unsourced, not
disproved** (the shading session could not quote the camera either; its scripts
were in `/tmp` and a reboot erased them, and the tuple is plausibly the
`AGENTS.md` reference camera). The overlay explanation I had recorded as the
leading hypothesis for the dark-%/blue% gap is **refuted** — all three sets were
overlay-suppressed. Residual is camera + tree state and gates nothing.

### Addendum — the residual, settled (2026-10-07)

The overlay hypothesis is **refuted** (all three sets were overlay-suppressed:
this session's arms and the clock session's curve arms ran `VF_NO_OVERLAY=1` with
an isolated `VF_OVERLAY_PATH`, md5-verified; the shading session recalls pointing
`VF_OVERLAY_PATH` at a non-existent `/tmp` scratch file, which loads nothing).

The unresolved cause is **camera + tree state**, and the honest split is:

- **Corroborated** — mean luma on *both* arms (day 113.84 vs 120.43, night 31.63
  vs 32.92), each pair measured by its own author, plus the night/day ratio to
  2 % (0.278 vs 0.273).
- **Disputed and unresolved** — dark % and top-eighth blue % on *both* arms
  (night: 69.68/84.7 vs 23.49/56.7, roughly 3x apart).

Retracted along the way: an earlier reading that the discrepancy was confined to
the day arm. It was conditional on the night columns converging; they did not, so
the framing does not hold.

**Second provenance incident, logged as its own failure mode.** Mid-exchange the
shading session wrote *"my settled night frame is dark% ~70 and blue% ~85"* —
those are the numbers from the pair above, not its own, which it confirmed on
request. It had attached its name to another session's measurement while arguing
against that measurement. This is the inverse of the unsourced-camera mistake and
is easier to miss, because an omission is visible while an over-attribution
presents as a citation. The page now carries the inverse question: **"did you
measure that, or are you quoting it?"** alongside "what was the command?" Neither
incident was caught by re-reading the numbers — both needed someone to ask who ran
the command.

**Standing down on the GPU.** The night gate for `visual_check` is agreed in
principle (own thresholds, not the daylight budgets — a night frame at dark% 23.49
is 4.7x the 5% black-in-silhouette budget, so the arm fails by construction
whichever measurement is right; modelled on `fog_check`'s off/on structure so the
default gate run does not pay two extra 17.6 s loads; the floor that keeps
`kMoonCol` from drifting back to 0.62 is the most valuable assertion it would
hold). It waits on ~14 uncommitted files in the shading path, because reference
values measured against an in-flux tree are falsified by the next commit without
anyone touching the test.

**Still uncommitted:** the four source files of the switch, now under another
session's lease — `git commit <file>` would sweep in peers' in-flight hunks plus
the separate German-layout keycap work.

## [2026-10-07] ingest | Night gate: test-night group + night-gate-thresholds page; sun_angles.hpp hoist

Cleared by the shading session after a green `visual_check` on the combined tree
(coverage 70.8 / 94.4 / 96.6, black-in-silhouette 0.18 / 4.29 / 2.25 %,
byte-identical to the pre-change run). No shader edit queued by me — confirmed
positively rather than by silence.

**`src/app/sun_angles.hpp` (new, pure/header-only).** Hoists the preset angles
and both phase thresholds out of the code that used them, for two reasons:

1. The `-2.0` night threshold was written as **four literals across
   `panel_render.cpp` and `run_hotkeys.cpp`**. They agreed only by convention —
   nothing stopped someone editing one, and then the Night button highlight and
   the `P` key would disagree about the current phase. I had told the shading
   session they "cannot disagree"; that was true by coincidence of having typed
   the number the same way four times, and I should have grepped before
   asserting it. Now one `sunIsNight()`.
2. `setSunPhase`'s angles were function-local `constexpr`, so a test asserting
   "the preset is -30/96" would have been asserting its own copy — the same
   failure mode as copying a non-atomic writer as a precedent.

Verified by grep: zero literals remain outside the header.

**New invariant pinned:** `kSunDayElev`/`kSunDayAzim` must equal the CLI
defaults in `args.hpp`, because 34/238 is what every reference shot was
calibrated to *and* what snapping back to Day must restore. If they drift,
every gate's reference moves and **nothing fails** — both values are
individually correct. Found immediately after the first fix, which is the
pattern paying off.

**Three new `test-app` cases** (12 cases / 110 assertions pass): the two-suns
distinction on angles (clock 00:00 = -60/0 vs preset -30/96, 30 deg apart),
`sunIsNight` as the single predicate, and the preset-equals-CLI-default
invariant. The two-suns claim is deliberately **not** an image metric: the two
measured arms differ by 1.7 mean luma, inside the 1.3 spread that made the raw
means unusable.

**`tests/night_check.py` + `night` CTest group** (mirrors `fog_check`;
`fast_night_check` alias; deliberately **not** in `smoke` — two extra world
loads). Three `visual_check` metrics fail on a **correct** night frame, verified
in source at `visual_check.py:69` and then measured:

- `is_sky = b > r+12 and g > r+4 and **b > 120**` — the last term is a daylight
  term. Scores **0.00 %** on all three night shots, so nothing is sky,
  `obj_frac → ~1.00`, and coverage fails out of range as **too little** sky.
  Without that term: 40.1 / 8.7 / 10.0 %.
- `black_in_obj` uses **absolute** `lum < 30`; the night frame's own mean is 23,
  so two thirds of a correct render reads black. Measured **66.0 / 86.5 / 78.3 %**
  against a 5 % gate — the worst of the three, off by more than an order of
  magnitude, and the one that would have been misread as "the renderer broke".
- `ownership_classes` needs `g >= 100` / `r >= 100` / `b >= 90`; night matches no
  class and the mask comes back empty.

`sky_probe_ok` (`b >= r`) **passes** at night, so the one metric that survives
cannot testify that the others are wrong — the same brightness-independence
problem as `visual_check`'s probe, reached from the other side.

**Thresholds are ratios to the day arm at the same camera**, because the same
preset measures **22.96 at the hero camera vs 31.63 at the reference camera** —
37 % from framing alone. An absolute luma limit here would be a claim about a
camera, not about night. Measured ratios 0.185 / 0.143 / 0.229 in a 0.10–0.32
band; sky ≥ 5 %; top-eighth blue ≥ 60 %; dark share **relative** (the absolute
daylight value is printed but explicitly not a gate).

**Positive control run:** thresholds made impossible → all three assertion types
fire with actionable messages and rc=1, so the assertions bite. Reproduces the
reference exactly across separate processes. That validates the instrument, not
the tripwire.

**The `kMoonCol` tripwire is NOT yet validated** — the ratio ceiling is
extrapolated from the 32.9 → 46 measurement taken at a *different* camera, which
is the exact mistake this session has been correcting. Tightening it needs one
render with `kMoonCol` at 0.62, a shader edit on a tree other sessions build
from; awaiting the shading session's explicit window rather than doing it
quietly. Until then 0.32 is *loose*, not *proven*.

Filed [[concepts/night-gate-thresholds]]. Wiki lint after the edits: 47 pages,
0 broken links, 0 orphans, frontmatter clean.

### kMoonCol tripwire VALIDATED in-window (2026-10-07 23:33)

Measured under an explicit build window granted by the shading session, with the
restoration proof agreed in advance:

| arm | night hero mean luma | night/day ratio |
|---|---|---|
| `kMoonCol` **0.62** | **40.06** | **0.323** |
| `kMoonCol` 0.14 (reference) | 22.96 | 0.185 |
| day arm | 123.88 | 1.000 |

`0.62` **clears** the gate's 0.32 ceiling, by 0.003. Two consequences:

- **The ceiling is NOT treated as validated at 0.32.** Healthy arms top out at
  0.229 (`water`), so 0.32 leaves ~1% margin above the regression it exists to
  catch. A gate that false-positives on a 1% margin gets disabled by whoever
  hits it first, and a disabled tripwire is worse than a loose one. Proposed
  loosening to ~0.27 (well above 0.229, well below 0.323) — shading session's
  call, it is their lane and their number.
- **The brightness multiplier is 1.74x at this camera, not the 1.40x**
  extrapolated from the reference camera's 32.9 → 46. This camera's night frame
  is darker and more moon-driven, so the moon's share of it is larger. Another
  instance of a number not transferring between cameras.

**Restoration, proven three ways, not asserted:** shader md5 back to
`7637a2fac15da77df8b2f8410bf24be5` (`md5sum -c` OK); diffstat back to exactly
`160 insertions(+), 2 deletions(-)`; `grep` on the **file** shows `* 0.14`;
`.spv` `e3e1ded3…` → `e9220de0…`. Restored from a byte copy taken before
editing rather than by inverting the edit, so the restore does not depend on the
edit being symmetric. Overlay md5 `a7daecd5` unchanged. One render, no test
group.

**Protocol corrections this window produced — both worth keeping:**

1. **`git diff` is the wrong instrument on a dirty tree.** The proposed proof was
   "git diff must be empty afterwards", but `common_base.glsl` was *already*
   160 insertions from before the window. The only way to empty that diff is
   `git checkout --`, which deletes the night path outright — and is verbatim
   the command that destroyed an uncommitted `world.json` edit earlier the same
   day. A check whose only satisfying action is the day's worst incident is
   worse than no check. Proof belongs on the **file** (md5) and the **diffstat**,
   neither of which can be satisfied by reverting to HEAD.
2. **`grep` the file, not the diff.** `git diff | grep kMoonCol` is non-empty on
   a *correct* restore, because `kMoonCol` is itself an uncommitted addition that
   does not exist in HEAD — so it appears in the diff whether the token is 0.14
   or 0.62. A non-empty hit there is precisely the signal that sends someone for
   `git checkout`. My proposed check #3 had this flaw; the shading session caught
   it. Checks must distinguish the two states they are meant to distinguish, not
   merely correlate with them.
3. **Hash the `.spv`, not the binary, for a shader change.** The binary md5 came
   back **identical** to the pre-window value across the whole window: shaders
   load at runtime from `build/shaders/*.spv` and are not linked into the
   executable. So the agreed "binary md5 differs from the 0.62 build" was
   unsatisfiable by construction, and reporting it as a failure would have been
   misreading a pass. Corollary: the "loud binary" warning broadcast to the other
   sessions described an artifact that never changed — nobody could have run a
   too-bright night by accident here.

**Pending:** the measured 0.323 is specific to the current night sky. The
shading session's sunset-transition work lands after this window and moves it,
so the ceiling decision and the whole threshold set need one re-measure
afterwards. Not finalised before then.

### Post-sunset re-measure (2026-10-07 23:55)

Re-measured all four items independently at the three canonical views after the
shading session's sunset transition work (+222 insertions to
`common_base.glsl`). Items 1-3 came back **bit-identical** to the pre-sunset
values — ratio band 0.185/0.143/0.229, sky floor 40.1/8.7/10.0 %, top-blue
100.0/75.1/70.9 %, daylight classifier 0.00 % on all three, day arm unchanged.

Exact equality is interpretable, not lucky: at elev -30 the sunset term
contributes exactly nothing and at 34 deg it is exactly 1.0 (smoothstep clamps),
so both endpoints are untouched by construction and the change lives entirely in
the 12 deg -> 3 deg window that previously had **no transition at all** — the
fifth member of the calibration family, a constant standing in for a range.

The floors and band are therefore confirmed on the tree the gate will run on and
need no edits.

**Item 4 (the 0.323 regression figure) is NOT re-measured and is STALE** — it was
measured on the pre-sunset sky, and re-validating needs `kMoonCol` at 0.62 again
(a shader edit plus a shared build). So the tripwire is neither demonstrably
weaker nor demonstrably intact: healthy band 0.185/0.143/0.229 (current), last
confirmed regression 0.323 (pre-sunset), ceiling 0.27 between them. Second
validation window requested. If 0.62 has moved toward the healthy band the
ceiling gets re-derived, never the band narrowed to fit.

**The `b > 120` finding is permanent.** A 37-value night sky cannot cross 120,
so no shading work can heal that classifier — only editing `visual_check.py`
can. The night classifier in `night_check.py` is permanently necessary, not a
stopgap for the current sky.

**Stale-build trap checked, not assumed.** `ninja -C build` reported "no work to
do" immediately after the shader change, which in this repo can mean a stale
object. Verified via the `.spv` md5 (post-sunset `40fe6576…` vs pre-sunset
`e9220de0…`) and mtime ordering, so the build was current. Unchecked, this
re-measure would have measured the wrong tree and reported "no movement" as a
finding — the `.spv` lesson from the tripwire window paying off immediately.

Overlay md5 `a7daecd5` unchanged across both arms.

### 0.323 confirmed post-sunset BY MECHANISM, not re-measured (2026-10-07)

The shading session predicted the regression ratio would not move and asked to run
(1)-(3) first as a falsifiable test. It came back bit-identical, so **no second
window was taken** — and that is the deliberate call, not an omission.

The mechanism, read in the source rather than accepted:

- `applyNight` does `col = mix(col, nightBase, night)`, so at `night == 1.0` the
  incoming colour is **discarded wholesale** before the moon disc/corona/wash.
- `nightFactor() = 1.0 - smoothstep(-0.14, 0.06, kSunDir.y)` is **exactly** 1.0 at
  the preset's y = -0.5; the clamp is well clear of it.
- The sunset work touches only `col` upstream of that, in **both** blocks —
  `skyColor` and the `skyIrradiance`/fog variant at `common_base.glsl:727`, which
  also ends `col *= sunDaylight(); return applyNight(...)`. The second block was
  the one that could have leaked into ground lighting; it does not.
- Day arm pinned the same way: `sunDaylight()` is exactly 1.0 at 34 deg.

So both terms of the ratio are outside the change, and the label is stated
exactly: **"confirmed by mechanism", not "re-measured"**. Opening a second window
would have produced a matching number and a stronger-sounding label while adding
a shader edit and shared-build exposure to prove something already provable from
the source. That is provenance theatre, not provenance.

**Residual kept visible rather than closed:** the argument covers the *sky* path.
`kMoonCol` also reaches geometry via `moonLight()`, and these frames are
sky/distance-dominated, so a small surface-only change could hide under an
identical mean. Established: three cameras, mean luma to two decimals, dark and
sky shares identical. Surface-level confidence wants a `--probe` pair on a lit
surface per `kMoonCol`, not another full render.


### Tripwire RE-MEASURED post-sunset: 0.323 reproduces to 0.13% (2026-10-07)

Second shader window, run under the protocol corrected by the first: hash the
`.spv` not the executable, restore from a byte copy, one render, rebuild, prove.

| arm | mean luma | ratio |
|---|---|---|
| `kMoonCol` 0.62, **re-measured post-sunset** | **40.06** | **0.323** |
| `kMoonCol` 0.62, pre-sunset | 40.06 | 0.323 |
| delta | — | **+0.13 %** |

The shading session's prediction — that the sunset work lives inside `col` before
`applyNight`, which discards `col` wholesale at `night == 1.0` — reproduced. So
the ceiling's provenance is now a **current measurement**, not the weaker
"confirmed by mechanism" label filed earlier. That label was right about what
could be concluded; it was superseded the moment the measurement existed, and the
page now says so rather than carrying both.

Tripwire strength unchanged: healthy max 0.229 (`water`) -> regression 0.323,
ceiling 0.27 between them with 0.041 / 0.053 of margin. No band edits.

**Restoration:** shader file md5 back to `52dd3c5989e72c30723f2d0de6180dc4`;
diffstat back to exactly `222 insertions(+), 10 deletions(-)` — the **post-sunset**
baseline, not the 160/2 from the first window, since the sunset work moved it and
restoring to a stale number would have been the same category of error as a stale
regression figure; file grep shows `* 0.14`; `.spv` returned to the **exact**
pre-window value `40fe6576…`, not merely to something different from 0.62.
Binary md5 `07306f1c…` unchanged across both windows, reconfirming that a shader
edit does not touch the executable. Overlay md5 `a7daecd5` unchanged.

**Broadcast wording corrected:** the "too-bright night frame will be visible"
half of the warning was the misleading one. A peer rendering inside the window
gets a bright night with **no tell in the binary or a `strings` check** — only the
`.spv` md5/mtime reveals it. That was my wording as much as the protocol's, and
worth not repeating in the next window.

Third time in this exchange that a **bit-identical control** (the 123.88 day
denominator here) is what made a small delta interpretable instead of alarming.

## [2026-10-08] ingest | Dynamic sun shadows: the 13 s stall is not the sun's fault
Answered George's "dynamic shadows" ask (sun shadows are CPU-baked per surfel, so
a sun move forces a re-bake). Two findings, from source, **no renders**:

1. **The stall is wasted work.** `setSunAngles` → `requestWorldReload()` →
   `m_layers.requestReload` (run_poll.cpp:35-37) re-runs the full `LayeredWorld`
   reload including `VoxelField::build` — the ~13 s per-component padded-bbox
   EDT from [[concepts/load-time-field-build]] — and **the field has no sun in
   it**. The only sun-dependent work is `rebuildSurfels`, which already reads
   the resident field (`buildSurfels(m_layers.field(), sp)`,
   surfel_stream.cpp:359). Caveats filed: the live-edit overlay would not be
   re-applied (that is `applyWorldReload`'s job, not `rebuildSurfels`), and the
   SVO reference never needed any of this — splat-only.
2. **The per-fragment alternative loses twice.** `softShadowSplat` marches
   `heightAt` (smoothed) + `objDist`, and `objDist` is the coarse `r8_snorm`
   256³ volume — 0.4 m texels — against a bake that marches the exact 10 cm
   lattice. So it is ~4x coarser *and* up to 32 dependent taps shaded twice per
   covered pixel (base EQUAL pass + blended band pass). The bake is the
   higher-quality path, not a workaround. Sun-space depth map is the shape to
   use if a GPU shadow is ever actually wanted.

Page: [[concepts/dynamic-sun-shadows]]. The measurement table is deliberately
**empty** — `rebuildSurfels()` wall time and the GPU-ms A/B are unmeasured, per
[[concepts/measurement-discipline]], and filling them with an estimate is the
failure this repo keeps paying for.

**Harness built and baseline proven.** Scratch clone of `shaders/` + `-I<scratch>`
+ `VOXELFORGE_SHADER_DIR=<scratch>` gives a variant tree with **no edit to the
shared sources** (every pass reads that env at init). The scratch
`splat.frag.spv` is md5 **identical** to `build/shaders/splat.frag.spv`
(`2d3202cd42bc32dff4c1b6cf7a67a9f3`) — the instrument's identity is confirmed,
not its health, which is the lesson of [[concepts/measurement-discipline]] and
of the `kMoonCol` window entry above.

**Probe bug worth remembering (twice in one session).** The first stale-`.spv`
sweep reported *every* shader `MISSING`, which looked alarming and was entirely
my own filename derivation: `${f%.*}` strips the stage suffix (`splat.vert` →
`splat.spv`). The real name is `$(basename $f).spv`. And `common_base.glsl` has
**no** `.spv` by design — the four `common_*.glsl` are `#include`d via `-I` and
appear only in `DEPENDS`. "Missing" was the correct answer for all four. A probe
that cannot tell a broken build from a correct one is not a probe.

Scoped with Wendy: she took the bake *rule* (`baked-sun-shadow-contract.md`),
this page keeps the cost/feasibility side and cites hers.

## [2026-10-08] ingest | baked-sun-shadow-contract — the splat sun-shadow bake rule (CPU shadowMarch, backface skip, neighbour averaging), plus why bit 1 widening to point lights changes enclosure

Page filed by Wendy ([[concepts/baked-sun-shadow-contract]]); catalog entry added
here to avoid a second lease cycle. It is the **rule** half of the pair with
[[concepts/dynamic-sun-shadows]]: that page is the cost/feasibility of not
re-baking every frame, this one is the bake the splat path actually reads. The
link matters in both directions — a sun-space shadow map would have to reproduce
this rule exactly, and `sh = 1` from the backface skip means *unmeasured*, not
*lit*, which is the trap that made `aoShEnclosure` silently pin to 0 on cave
walls.

## [2026-10-08] lint | catalog/link sweep: 1 missing Pages entry found and fixed, 1 benign dangling link

Machine-checked both directions now that Vega and I were both in the catalog at
once (the contention is what surfaced the gap).

**Direction 1 — every page must be in the catalogue.** `concepts/brush-preview-visibility.md`
existed, was linked from "How to navigate", was cited from `AGENTS.md`, and was
**absent from the Pages list**. That is the worst shape for a wiki page: fully
reachable *if you already know it exists*, invisible to a reader who is looking
for what it covers. Entry added.

**Direction 2 — every link must resolve.** One dangling link in the whole tree:
`log.md` cites `[[concepts/water-flooding]]`, a page that no longer exists.

Leaving it. Two reasons, and the distinction matters more than the fix:

- `log.md` is **append-only**, so editing that line would falsify the record of
  what a past session actually believed. Correcting history is worse than a dead
  link.
- The page is genuinely **superseded, not lost**: water became one fixed-level
  plane (`WATER_LEVEL = -0.9`) with no per-column bookkeeping and no flood
  machinery at all, so the concept dissolved into
  [[concepts/water-plane]]. The dangling link is an accurate timestamp of when
  the flood design stopped existing.

So the rule this sets: a dangling link inside `log.md` is evidence, not a defect.
A dangling link in a **content** page is a defect and gets fixed in place. The
sweep counted 51 pages; after the fix there is no gap in either direction except
this one deliberate exception.

Method note: the check is a link-set difference both ways (`rglob('*.md')` minus
`index.md` versus the wiki-link targets). Cheap enough to re-run after any
ingest — it is the mechanical form of "is this page findable", which is the
question a reader asks first and the one an authoring session never asks about
its own work.

**A detector blind spot this very entry created.** The first re-run reported a
dangling link with the target `...` — which was this paragraph's own literal
example of the link syntax, quoted inside inline code. A regex that scans raw
text cannot tell a real link from a documented one. Two lessons, both about the
instrument rather than the wiki:

- Strip inline code spans and fenced blocks **before** extracting links, or the
  sweep manufactures false positives out of prose about links.
- `log.md` must be excluded from the "page on disk" set as well as `index.md`,
  or it reports *itself* as missing from the catalogue. My first version got
  this wrong and printed `missing from Pages section: ['log']`.

**Correction, and the reason it matters more than the fix.** The entry above
claims "no gap in either direction except this one deliberate exception", and at
the time it was written only **half** of that was machine-checked: direction 2
(links resolve) was a real tree-wide scan, but direction 1 (page present in the
catalogue) was an eyeball read plus one `grep` for the single page I had already
noticed. A read-through *feels* like coverage.

Vega (independent pass, Pages-section-scoped `sed '/^## Pages/,.../'`) then
machine-checked both directions over 51 pages: **zero catalogue gaps, zero
dangling links in content pages.** So the claim above is now independently
verified, and the one symptom I fixed was the only symptom there was.

Two instrument bugs from that pass are worth keeping, because both report clean
for the *wrong reason*:

1. Direction 1 first grepped all of `index.md`, so `brush-preview-visibility`
   "passed" on its appearance in the How-to-navigate section — it would have
   reported zero gaps without ever testing the Pages list. Restricting the scan
   to the Pages section is what makes it the real test.
2. The dangling-link detector printed **nothing at all**, which is nearly the
   worst possible output to be handed: silence reads as clean. Its positive
   control had been injected into `index.md`, which that detector never scans,
   so the control proved nothing. Re-run as a temp file inside `concepts/` with
   one real and one fake link, it fired on the fake and resolved the real.

That is the heightfield-blindness shape one level up — a measurement that
returns the same value whether or not the thing under test is present. A sweep
with no control is not evidence; see [[concepts/measurement-discipline]].

## [2026-10-08] decision | Sun shadow-map pass chosen for dynamic shadows; "Shadows" (bit 1) widened to gate point-light occlusion

Requested by George (shading) at 2026-10-08. Recorded as **decisions with
attribution**, not as validated results — the distinction is the point of this
entry.

### D1 — a sun shadow-map pass is the chosen route for dynamic shadows

- **Attribution:** user decision, relayed by George. Not derived from a
  measurement recorded in this wiki.
- **State:** chosen. **Not implemented, not validated.**
- **What the wiki already argues, and does not contradict:** the per-surfel CPU
  bake is the cheaper and *more accurate* option today. The splat GPU sun march
  would be slower and less sharp — 0.4 m `objDist` texels against a 10 cm bake,
  two shading passes, ~64 taps/px
  ([[concepts/dynamic-sun-shadows]]), and the 13 s stall on a sun change is a
  `VoxelField` EDT that contains no sun at all
  ([[concepts/load-time-field-build]]).
- **So the decision rests on something unmeasured**, most plausibly contact
  sharpness or a quality ceiling the bake cannot reach. That is a legitimate
  reason; it is just not the reason in the record, and the next session should
  not inherit "measured and found better" by association.
- **The constraint that survives either way:** a shadow-map pass must reproduce
  the bake *rule*, or it will disagree with everything around it —
  [[concepts/baked-sun-shadow-contract]]. Three clauses are the trap:
  the `dot(n, sunDir) > 0.02` backface skip where `sh = 1` means *unmeasured*
  rather than lit; the pass-3 neighbour averaging that makes the value the
  shader reads spatially filtered rather than per-fragment; and the 0.3 m origin
  offset that exists so canopy does not self-shadow.
- **Open:** cost, quality-versus-bake comparison, and whether a map can be
  filtered to match a neighbour-averaged verdict at all.

### D3 — a post-init ("late") sun hook is a precondition of D1, not test scaffolding

Agreed with George 2026-10-08, who is adding it alongside the shadow-map work.
The reframing that matters: a dynamic sun must be movable **after** init whatever
it drives, so the seam had to exist; timing it in isolation is only its first
honest use.

Three constraints recorded now so they are not rediscovered as bugs:

1. **Never compare a late-hook number against a pre-init one.** Both
   `VF_TEST_SUN_PHASE` and `VF_TEST_SUN_TIME` fire inside `App::run` *before*
   `initWindow`/`initVulkan`, so a headless run measures startup + the coalesced
   first reload — an **upper bound containing the ~17.6 s initial world load**,
   not the stall. (Found by Vega; he is recording provenance rather than
   publishing a figure that is not the stall.)
2. **Label what the timer brackets.** `applyWorldReload` also calls
   `vkDeviceWaitIdle` before `rebuildSurfels()`, and the GPU-idle wait is part
   of what a user feels. A rebuildSurfels-only figure is honest but answers
   "what could it be", not "what will it cost" — two labelled fields, not one.
3. **A late sun flip has no visible answer until the rebuild *lands*, not until
   it is queued.** Nothing chases it: baked sun shadows are per-surfel
   (`[[concepts/baked-sun-shadow-contract]]`), patched/live chunks lose their LOD
   ring until the next full reload, and `aoShEnclosure` reads the baked `sh`.
   The sky and direct light move on the next frame because `m_sunDir` rides the
   push constant; the shadows do not. Same shape as the `setSunPhase()` GUI path
   in `[[concepts/sun-direction-pipeline]]`. If the hook reports "queued", the
   first measurement reads as *no shadow change* and looks like a bug in the
   hook rather than in the timing.

If the seam proves unobtainable headlessly, **record that as the finding** —
"no post-init sun seam exists" is the actionable result, and it is what a
day/night slider needs resolved. An honestly-labelled upper bound is useful; the
same bound without its label becomes *the number*, and the next session quotes
it as the stall. See [[concepts/measurement-provenance]].

### D2 — "Shadows" (bit 1) gates point-light occlusion

- Renamed from "Sun shadows" to "Shadows", and `applyLights()` now runs
  `lightVisibilitySPlat` / `lightVisibilitySVo` only when
  `(gRenderFlags & 2) != 0`. Point lights used to march unconditionally, so the
  wider label would otherwise have been untrue.
- **Attribution:** code decision, landed in the working tree.
- **Blast radius worth restating:** bit 1 is now an input to `aoShEnclosure`
  (which requires bits 1 **and** 2 **and** 8), so "Shadows off" also changes how
  caves and interiors shade, and `VF_RENDER_FLAGS=255` is not a bit-1 escape
  hatch. Details in [[concepts/enclosed-space-lighting]].

### Related, same tree: the emissive lane

An `emissive` / `emissiveScale` flag on `world.json` texture bindings, the
`TexTable.z` channel and `emissiveTerm()` all landed (uncommitted 2026-10-08).
**Deriving real point lights from emissive materials is the open seam:**
`TexAtlas::emissiveScale()` / `meanColor()` exist with **zero call sites**, and
`VoxelField::collectEmissive` appears only in a comment. The constraints on it
are already visible in the landed half — `VF_TEXTURES=0` zeroes `m_emis` so a
derived light must vanish too (or that escape hatch stops being bit-exact),
`kEmissive` is duplicated CPU-side in `common.hpp` and GPU-side in
`common_base.glsl`, and derived lights share the same 16-slot `LightUBO` as
authored lamps so the split budget has to be decided rather than discovered.

## [2026-10-08] finding [RETRACTED] | Claimed bit-1 enclosure asymmetry — unreachable past `aoShEnclosure`'s own guard

**This finding was wrong. Retracted the same day; kept because the failure mode
is more reusable than the claim was.**

What was claimed: splat folds an **ungated** baked shadow into `aoShEnclosure`
(`splat.frag:321` passes `vShade.w` as `shRaw`; `common_splat.glsl:178` folds
it) while SVO folds its **flag-gated** `sh` (`common_svo.glsl:583`, folded at
`:594-595`), so clearing bit 1 was claimed to leave splat interiors darkening
through `(1 - ao) * (1 - sh)` while SVO lost the proxy — a backend-asymmetric
third instance of "a bit-cleared escape hatch does not restore the previous
image". Filed with a proposed A/B at `VF_RENDER_FLAGS=253`.

**Why it is wrong.** `aoShEnclosure` opens with its own guard,
`if ((gRenderFlags & (256 | 3)) != (256 | 3)) return 0.0;`. `256 | 3` = 259
needs bits 8, 1 and 0; at 253, `253 & 259 = 1 ≠ 259`, so it returns 0.0 **before
`sh` is read, identically on both backends.** The difference is unreachable in
precisely the state the test proposed — the A/B was guaranteed flat.

Refuted at the source layer by Vega before it reached a GPU window, and
retracted to George in the same turn. **No GPU time was spent, and no code was
changed** — which was the entire point of filing it as source-verified rather
than as a hypothesis.

Three things this cost, worth recording honestly:

1. **The call sites were read correctly and the callee's guard was still missed.**
   The guard sat three lines above the comparison in the same file and had
   already been read earlier in the same session. Having the evidence is not
   using it.
2. **The "structural, not measured" hedge was a way of keeping a wrong claim
   alive.** A hedge is not a smaller claim; it is the same claim with a
   disclaimer that lets it survive being wrong. The correct move on disagreement
   was to delete it, which is what the page now says.
3. **The `shRaw` difference is a mitigation, not a drift.** `splat.frag`'s caller
   gates `sh` to 1.0 on backfacing surfels, which pinned the proxy to 0 on cave
   walls (45.26 vs 45.84); SVO has no such caller gate, so its ungated `sh`
   satisfies the same contract. "Same rule, different inputs because one caller
   has a gate the other lacks" beat "one site diverged" — a reading that only
   becomes available if you read the caller comments, not just the callee.

**The generalisable form: a call-site comparison is not a behaviour
comparison.** This looked like a backend asymmetry at both call sites and is
invisible at the only place that decides. That is the third instance of this
shape in one exchange — a `${f%.*}` probe that passed while wrong, a
catalogue sweep clean on one direction only, and this — all of them clean at the
layer inspected and uninformative at the layer that decides. Folded into
[[concepts/measurement-discipline]].

Residual, **not** part of this finding: with bits 0/1/8 all set, splat folds a
baked shadow and SVO a frame-time march. Those differ numerically and that is the
pre-existing baked-vs-marched difference the design accepts.

Incidentally verified in the same pass and **standing**: SVO does march the sun
per fragment (`common_svo.glsl:583` `softShadow`, 16-tap PCF over
`exactSVOHit`; `:555` `softShadowTerrain` for the submerged bed), so the
per-surfel bake shortcut is splat-only — previously sourced only from
`AGENTS.md`, now from the shader.

## [2026-10-08] lint | Hedge sweep across content pages: zero asserting hedges; one rule was missing, not one habit

Ran the grep from the new `[[concepts/measurement-discipline]]` section across
`concepts/` + `entities/` (`structural, not measured|unmeasured|unvalidated|
untested|not compared`).

**Instrument note first:** the raw grep fired on 8 hits, so it is known to be
able to fire — the control Vega had to add for his own detectors. All 8 were
then triaged **by hand**, which is the weak part of this result and the reason
it is logged as a sweep rather than as a gate.

**Outcome: zero asserting hedges.** Every hit was either *disclosure* —
`dynamic-sun-shadows` tables saying "unmeasured", `sun-direction-pipeline`
saying "George-reported, not measured by the wiki session",
`sky-probe-is-a-camera-assertion` saying "not measured by me; not comparable" —
or *terminology*, i.e. `sh = 1` meaning unmeasured rather than lit.

**The interesting part is what that implies.** The pages had already learned the
disclosure habit: attribution, explicit unmeasured tables, cross-tree
comparability warnings. What was absent was any rule about **routing** — where
an unsettled claim is allowed to live. The gap was not carelessness in the
writing; it was that nothing said an unsettleable claim must leave the content
layer entirely. That is the row now added to
[[concepts/measurement-discipline]], with "structural, not measured" named as
state (a) wearing a disclaimer.

One borderline, referred rather than fixed: `entities/live-edit-brush.md` labels
a proposed test "**unvalidated** — a proposed test, not a working one". The
disclosure is correct, but it is attached to the *test* while the *mechanism*
around it keeps an implicit status — the same shape as the retracted finding,
pointing the other way. Owner judgement, not a wiki edit.

## [2026-10-08] ingest | Emissive-derived point lights landed (seam closed); night ratio band marked STALE; verified shader A/B harness

The emissive lane recorded earlier in the decision entry as an **open seam** is
now closed. `VoxelField::collectEmissive()` exists
(`src/voxel/voxel_field.cpp:858`) and is called from `App::uploadLightSources()`
(`src/app/rhi/surface.cpp:143`); `TexAtlas::emissiveScale()` / `meanColor()` have
their first consumers. Filed into `[[concepts/enclosed-space-lighting]]`, with
the four non-obvious decisions: emission read from the **atlas** not the
manifest (so `VF_TEXTURES=0` kills the light with the glow), **count-desc /
key-asc** ordering for reload determinism, **1.5 m thinning**, and each centroid
**lifted to the nearest AIR cell** — a light buried in its own emitter is
occluded by every receiver's march, so it would take a slot and contribute
nothing. Authored lights fill `kMaxLights` first; derived lights are never
persisted. The default hamlet derives **14**.

Three consequences that outlive the feature:

- **`kEmissive` is duplicated** (CPU `common.hpp`, GPU `common_base.glsl`), so
  one-sided edits make lit colour and glowing colour disagree.
- **The derived lights are not day/night gated**, so they are present in the
  night arm as well as the day arm — which makes the `test-night` ratio band
  **STALE**. `[[concepts/night-gate-thresholds]]` now carries a banner: both
  endpoints (healthy max 0.229, `kMoonCol` regression 0.323, ceiling 0.27) were
  measured in a tree with no derived lights, and if the night arm moves so does
  the regression arm, so the **margin must be re-derived, not re-adjusted**.
  Recomputing only the healthy max would anchor a new ceiling to an old
  regression value — the mixed-tree error in
  `[[concepts/measurement-provenance]]`. **The page predicts no sign**, because
  recording a belief about the sign would manufacture a second unmeasured
  assertion.
- **A pure-ratio gate is blind to in-band drift, and that is structural rather
  than a threshold error.** The ratio band was chosen *because* it survives
  camera drift — the same scale-free property that makes it immune to framing is
  what makes it incapable of noticing a scene that scaled. Derived emissive
  lights are present in the day arm and the night arm alike, so if they lift
  both roughly in proportion the ratio can sit at ~0.19–0.23 while every
  absolute value moves materially, and nothing in `night_check` notices.
  (Identified by Vega, 2026-10-08.) Same property, opposite failure — an
  inversion of the provenance lesson, not a contradiction of it.

  **Agreed shape, not yet in code.** Three outputs with three different
  validities, so no one number is asked to mean all three:

  | output | valid across trees? | role |
  |---|---|---|
  | ratio band, re-derived in-tree | **yes** | the gate |
  | absolute mean per arm (day / night-healthy / night-regression) | in-tree | the anchor the band is computed from |
  | `new-absolute / old-absolute` quotient vs pre-emissive figures | **no — informational only** | drift indicator; may never gate |

  The pre-emissive absolutes (day 123.88 / 105.41 / 111.88, night 22.96 at hero)
  **cannot gate anything** — that is the mixed-tree error. Their quotient is
  legitimate as a drift *report*, because it answers "did this tree move the
  scene" without claiming pass/fail.

- **Proposed: a second, scale-bearing assertion.** An absolute **day-mean luma
  band** per camera, gated separately, re-derived in the same tree and session
  as the ratio band, carrying the same "re-derive them together" comment — a
  second absolute constant is exactly what goes stale next and then gets
  "adjusted" by whoever hits it first. Rationale: a scale-free check cannot
  detect a scale change, so without a scale-bearing assertion the gate is
  structurally incapable of noticing one.

  **The two assertions are not substitutes.** After the split the ratio can go
  red with the day band green (a real lighting regression) and the reverse
  (tree/content drift). Both are meaningful, and the failure mode is someone
  "fixing" whichever is red. The assertion comments must say so.

### Contract for the second assertion (decided 2026-10-08, code not yet written)

Two design calls made up front, with the reasoning recorded so the *reasoning*
can be overruled later rather than just the conclusion:

**The day band is symmetric, not floor-only.** A one-sided band would re-introduce
the exact defect this assertion exists to fix. The ratio gate is structurally
blind because it is scale-free; a floor-only day band is structurally blind to a
scene that got **brighter**, which is the direction emissive-derived lights move.
The crying-wolf concern is a **width** problem, not a symmetry problem, and the
width is knowable rather than guessed: the day arm came back **bit-identically**
(123.88 / 105.41 / 111.88) across a 222-insertion shader change, because
`sunDaylight()` is exactly 1.0 at 34°. If it ever cries wolf, widen it **with a
recorded reason** — never remove a side.

Two properties of the day arm make it a clean drift detector (Vega, verified at
source, 2026-08-08):

- **`kMoonCol` cannot touch the day arm.** `moonLight()` returns `vec3(0.0)` when
  `night <= 0.001`, so the day anchor is a true common denominator across shader
  variants — a day-mean move is unambiguously content/lighting, never an artefact
  of the constant the regression arm manipulates.
- **`kMoonCol` fans out to four call sites** (`common_svo.glsl:569`, `:610`;
  `common_splat.glsl:185`, `:261`) across both backends. A regression arm must
  therefore be rendered on **both** to be comparable; a splat-only arm must never
  be presented as the whole thing. Also note `kMoonCol` is defined **once**
  (`common_base.glsl:611`) with **no CPU-side copy** — verified, because
  `AGENTS.md`'s duplicated-table warning would otherwise have applied here.

**Vintage rule — the load-bearing constraint.** Both bands are derived in one
tree in one session, **or neither exists**. A day band shipped now would sit
beside a ratio band awaiting a newer tree: the mixed-tree error in the specific
shape of two same-file thresholds that look like a matched set. So the contract
is filed and the code waits for the re-derivation moment.

The residual risk is that the page makes the day band look *additive*, so
someone adds it alone later. Mitigated by writing the vintage rule into the
**assertion comment** verbatim as a **prohibition**, not a preference — because
a preference is what gets ignored at 2am, and the comment travels with the code
rather than only with the wiki.

**The band's width has its own provenance and must declare it.** The
justification above is that the day arm came back bit-identically across a
222-insertion shader change. That is **one observed no-change**: it supports
"the day arm is stable", which is what justifies adding a scale-bearing assertion
at all, but `n = 1` bounds observed variance from above on one occasion rather
than describing a distribution. A symmetric width that reads as measured will be
*treated* as measured. So the comment must say the width rests on a single
observed stability data point, **paired** with the widen-with-a-recorded-reason /
never-remove-a-side rule — otherwise the band becomes an absolute constant whose
evidentiary basis is one run, which is precisely how `kMoonCol = 0.62` and the
0.27 ceiling each became load-bearing by accident. (Caught by Vega.)

**Lease timing — do NOT hold a lease over `tests/night_check.py` until you write.**
A lease held for hours over a file nobody is editing is its own hazard: it
implies ownership nobody is exercising and it blocks George or the wiki layer
from the file for no reason. Claim at the moment of the edit, release
immediately after — the discipline used for `run_hooks.cpp`. (Corrected here:
this entry originally said he holds the file under lease, which the vintage rule
makes false.)

Ownership: `tests/night_check.py` is untracked and listed in
`[[concepts/night-gate-thresholds]]`'s `sourceRefs`, so it belongs with the wiki
layer's provenance. Vega writes the code, taking a lease only for the duration
of the edit; the wiki holds the contract. No file overlap.

- **Shader A/B harness, verified end to end** (Vega, 2026-10-08) — the
  instrument the re-derivation depends on: clone `shaders/` to a scratch dir,
  compile the full set with `-I<scratch>`, render with
  `VOXELFORGE_SHADER_DIR=<scratch>`. 0 compile failures, 18/18 `.spv` produced,
  **all 18 md5-identical to `build/shaders/*.spv`**, so an unedited clone is
  provably the same tree rather than an asserted one, and editing one constant
  there gives a single-variable arm. All 18 must be present — the env var
  replaces the whole directory, and a partial set fails or silently falls back,
  and a half-run is worse than a failure because it looks like data.

  **Dated observation:** at that moment `build/shaders` was in sync with
  `shaders/` (0 of 18 differed), which excludes a stale `.spv` as the
  explanation for anything rendering oddly in that window — the cheapest and most
  attractive wrong answer, and the one `AGENTS.md`'s stale-object note primes
  everyone to reach for. **Re-check before relying on it.**

Cross-links: [[concepts/measurement-discipline]],
[[concepts/measurement-provenance]], [[concepts/enclosed-space-lighting]],
[[concepts/baked-sun-shadow-contract]], [[concepts/sun-direction-pipeline]].

## [2026-10-08] lint | `test-night` was missing from the group table, and group independence was never stated

Two gaps in `[[concepts/focused-test-groups]]`, both found because a session
recorded `test-night` as "skipped when the visual group went red" and the
recording was believed.

1. **`test-night` was absent from the group table entirely.** A page whose whole
   purpose is "which group do I run" omitted one of the groups. Added, with its
   coverage (ratio band, night sky classifier, `kMoonCol` tripwire).
2. **Group independence was never documented.** Each `test-<group>` is
   `ctest -L <group>` with `VOXELFORGE_TEST_GROUPS=<group>`, and
   `vf_add_test_group` gives it `DEPENDS` on the **binaries only** —
   `vf_add_test_group(night voxelforge)`. No `DEPENDS`, no
   `FIXTURES_REQUIRED`, no ordering. So a red in `test-visual` **cannot** skip
   `test-night`; the night gate had simply not been run.

The generalisable point, which is the reason this is worth a page section rather
than a footnote: **"skipped" and "not run" are different states, and only one of
them is evidence.** A reported skip should be checked against an actual run
before its reason is accepted — the same discipline as "an absent signal is only
evidence if you have shown the probe can fire."

Also clarified on `[[concepts/night-gate-thresholds]]`: a **green** night result
is two findings at once, not one. It means the band survived *and* that the gate
is scale-blind in practice (derived lights present in both arms, lifting day and
night in near-proportion). So green is **evidence for** the proposed
scale-bearing day-mean assertion, not evidence against needing it — and it would
justify that assertion by measurement rather than argument. "Green" and "the gate
can see everything" are different states; conflating them is how a scale-free
gate outlives the problem it was built for.

## [2026-10-08] finding | `VF_TEXTURES=0` was not bit-exact on the per-cell override path; `ninja -k1` masquerades as a skip

Two findings, one from George's report (verified in source) and one from
diagnosing why `test-night` never ran.

### The escape hatch had a hole in it

`VF_TEXTURES=0` is documented as a **bit-exact** escape hatch — the control that
lets a gate prove a texture change did nothing. It was not bit-exact on the
per-cell override path.

The old code decoded every texture and *then* cleared the slot table. That is
enough for the material path (`matTex[mId].x` → `-1` → `sampleTex` returns
`vec3(-1)` → palette). It is **not** enough for the override path, because
`texSlotFor` gives `gTexOv` precedence:

```glsl
return gTexOv > 0.5 ? floor(gTexOv + 0.5) : uTex.matTex[mId].x;
```

So override cells kept sampling the **decoded** layer with the slot table already
cleared, and the hatch left **0.83 % of the hero frame** showing the very checker
it promises to remove — caught by the `texture_check` `vf_off` arm. Fixed by
skipping the decode entirely (`if (disabled) break;`), so every layer keeps the
neutral filler and the result is byte-identical to the no-table palette arm.

**Status: verified green (George, 2026-10-08).** `texture_check` passes with the
`vf_off` residual now **0** and bit-exact, so the mechanism is confirmed by the
gate rather than by reading alone. Recorded here because the entry was first
filed as a *report of a staged fix with the mechanism unverified* — the
distinction between "a gate says so" and "I read the code and it looks right" is
the whole reason the escape hatch was worth filing at all.

**Why this is expensive rather than merely wrong:** the hatch is only ever
exercised when someone thinks to check it, and its whole value is that it is
*provably* identical. A hole in it is invisible in normal use and silently
invalidates every A/B that used it as a control. Filed in
[[concepts/texture-atlas]].

### `ninja -k1` stops scheduling, which reads as "skipped"

A session reported `test-night` as "skipped when the visual group went red". The
real cause was **ninja-level, not ctest-level**: `ninja -k1` halts scheduling
after the first failing target, so `test-night`'s target never started. CTest
groups are fully independent — `vf_add_test_group(night voxelforge)` gives
`DEPENDS` on the **binaries only**, with no `DEPENDS`, no `FIXTURES_REQUIRED` and
no ordering between groups.

The generalisable point: **"skipped" and "not run" are different states, and only
one of them is evidence.** A reported skip should be checked against an actual
run before its reason is accepted. Use `-k0` to keep going past failures when the
full picture is wanted.

## [2026-10-08] lint | Systematic sweep: 44/52 pages reference changed files, 2 real defects found and fixed

Ran the periodic lint properly rather than piecemeal. Method: extract
`lastReviewed` + `sourceRefs` from every page, intersect against
`git status --porcelain` plus files changed since 2026-10-05.

**44 of 52 pages reference at least one changed file.** That number is *not* a
staleness signal on its own — the tree is mid-flight with ~30 modified files,
so most pages legitimately point at something in flight. Treating it as a defect
count would have produced 44 false positives. The useful signal is the **oldest
pages against the most-changed files**, so verification was targeted there.

**Verified accurate, no action:** `concepts/load-time-field-build.md` (2026-09-18)
— the page Victor is about to optimize. Every load-bearing constant checks out
against `src/voxel/voxel_field.cpp`: `kStore = 6` (:16), `kMargin = 2` (:15),
`kPad = kStore + kMargin = 8` (:17), `groupComponents` exists (:62, called
:398), the bbox padding is `2 * kPad` (:146-148), and the air-band test is
`dCell > kStore * VOXEL` (:271). The 361M padded-bbox / ~680× inflation figure
is consistent with `kPad = 8` on 1-cell components.

**Defect 1 — duplicated paragraph with an inconsistent variant.**
`concepts/voxel-object-authoring.md` carried the same sentence twice, once
ending "keeping **the bake sweeps** the single source of truth" and once
"keeping **`scene()`** the single source of truth". An editing accident, and the
two variants disagree about what the source of truth is. Removed the duplicate,
kept the `scene()` form (it matches the surrounding text's framing).

**Defect 2 — a claim contradicted by another page.** That same page was headed
"## Why SDF-in-code (**no mesh import**)" and said "A converter was considered
and rejected for now". Mesh import exists: the sidebar **Mesh** section,
`vf_mesh2vox`, and the `import_mesh` MCP tool
([[entities/mesh-to-voxel]]). The heading now reads "Why SDF-in-code is the
**primary** path" and says import is a *secondary* path, with the reasons
re-framed as why SDF-in-code stays primary rather than why import is unavailable.

**Also confirmed clean:** no page still claims `writeLightManifest` is missing
(the only "loader-only" hit is the sentence saying that claim is *retired*), and
no content page carries a `pre-splat-rework` marker — those survive only in
`index.md`, where they are accurate pointers.

**Method note for the next lint:** a `git status` intersection cannot
distinguish "the page drifted" from "the tree is mid-flight". On a dirty tree it
produces a number that looks like a defect count and is not one. The actionable
unit is *oldest page × most-changed file*, and the verification has to be done
by reading the constants, not by counting.

## [2026-10-08] correction | The night band is NOT stale — re-derived unchanged, and now empirically confirmed blind

**The banner I filed two messages earlier was wrong, and Vega was right to
challenge it.** It said the 0.10–0.27 ratio band "no longer carries the tree it
was measured on" and told the reader a passing gate could not be trusted. The
measurement then came back **identical with the 14 derived lights present**:

| arm | pre-emissive | with derived lights | delta |
|---|---|---|---|
| hero | 0.185 | 0.185 | 0.000 |
| house | 0.143 | 0.143 | 0.000 |
| water | 0.229 | 0.229 | 0.000 |
| day absolutes | 123.88 / 105.41 / 111.88 | 123.88 / 105.42 / 111.90 | +0.00 / +0.01 / +0.02 |

So the margin does not need re-deriving. The legitimate risk was closed by the
measurement, and the banner was then **actively harmful** — it trained the reader
to dismiss the one signal that would catch a real regression. Retitled to
**RE-DERIVED 2026-10-08, UNCHANGED**.

**Why it held (Vega's mechanism, which I had wrong):** the derived lights are in
**both** arms, so they largely cancel in a ratio — a ratio of two quantities that
both moved is not evidence that either moved. And at the three exterior canonical
cameras the emitters contribute very little solid angle to a frame mean: 14 lights
in a hamlet, seen from outside, move the day mean by **+0.02/255**. The second
reason is the one that generalises, and it is a property of the *cameras* rather
than the lights, so it holds for any future emitter set placed inside the hamlet.

**The corollary is the finding: the band is now empirically confirmed as blind.**
A 20 % change in every lamp would pass it. That is a *demonstrated* argument for
the scale-bearing day-mean assertion rather than an argued one — the strongest
form the thesis can take. The gate's robustness to content change and its
blindness to content change are the same property: scale-free by design, and a
scale-free check cannot detect a scale change.

**A lights-only control is a decided "do not build", with the reason.** The
`VF_TEXTURES=0` confound is real (it swaps photo→palette albedo in both arms, so
any delta is unattributable — house's night went *up* while its day also went
up, and removing light sources cannot raise night luma). A dedicated hook was
considered and rejected: if the lights off moves the day arm by +0.02/255, it
cannot move the night arm enough to matter for a ratio band. **Do not spend a
hook on a 0.02/255 signal.** Recording a rejection with its reason is worth more
than leaving it open — an open question gets re-litigated by everyone who sees it.

**Lesson for the next banner:** a warning that outlives its evidence is worse
than no warning. The banner was correct when filed and wrong one measurement
later, and nothing in its wording said which state it was in. A banner should
carry its own expiry condition, or it should be removed the moment the risk
closes.

## [2026-10-08] ingest | Renderer improvement roadmap filed; EDT-batching state verified in code

`[[concepts/improvement-roadmap]]` created from the wiki's open items,
indexed, prioritized P1–P3 with the measuring page for each. Code check
2026-10-08: the EDT path already has per-component content-hash caching
and parallel Dijkstra (`src/voxel/voxel_field.cpp`); the open part is
spatial merging of small components before the padded-bbox EDT (~680×
inflation, ~13 s of ~17.6 s). Tile-splat (`VF_TILE=1`) status delegated
to Vega for a current measurement; EDT batching handed to George.
Cross-links: [[concepts/load-time-field-build]],
[[concepts/measurement-provenance]].

Tile-splat status corrected from Vega's analysis (2026-10-08): the
143/57/65/78 ms figures are doc-sourced, not re-measured; the opaque
fp deltas are categorical set differences from the seal's
`fragDepthQ == depth` equality test, not accumulation error; water is
bit-exact because it uses a strict `<` test with identical per-fragment
contributions; the tile path's honest value prop is determinism, and its
gate must be the pinned 0.18/255 noise floor, never bit-exactness.
Re-measure with VF_TRACE + per-arm spv md5 provenance is pending on a
free GPU.

**Update (2026-10-08):** Vega's same-binary measurement reverses the
documented parity claim — forward-vs-tile mean|d| 5.56/255, 64.31% of
pixels differ, ~119× the pair's 0.047/255 control floor; localised to
all geometry uniformly with the sky at noise floor and no brightness
bias, i.e. a different set of contributing disks. Her earlier
seal-equality explanation was refuted by her own localisation; next
candidate is `VF_SPLAT_SEAL_ALPHA`/`uSplat3.x` default parity between
paths, plus open question on `dups 0` near-band tiles. Timings remain
"unre-measured" (`VF_TRACE` silent in `--shot`). AGENTS.md's "small
fp deltas" wording flagged stale. George declined the EDT-batching
prototype until his shadow-map feature lands; offer reactivates on his
ping.

## [2026-10-08] query | Per-object voxel size for detailed objects

User asked: can objects carry a different voxel size (a vase of
500x100x1000 cells shown smaller than the 10 cm world)? Answered from
code: rejected at load time (worldfile/layered_world/editable_world all
gate meta.voxelSize), downstream stack assumes one lattice, global
VOXEL change is budget-prohibitive (stored quirk). Honest levers today:
detail pipeline (micros, crease bridges, anisotropic disks), per-cell
texture + detail normals, fine-author/resample import. Filed as
[[concepts/per-object-voxel-size]]; indexed.

**Victor's first profile numbers (2026-10-08):** surfelize bake 4278 ms
with shade+bucket 4006 ms over 4.19M surfels; --smoke 60 averages
5.52 ms/frame at 640x360; test-surfel 4/4 green. Frame-CPU env-cache
and bake AO-hash already landed parity-safe. Filed as a P1 on the
roadmap page; load breakdown (EDT vs surfelize vs upload) still
pending.

**Correction (same day):** the 4278 ms surfelize figure was a cold
run; warm-day is 3418/3503/3445 ms across three runs, night 2619 ms.
HEAD control invalid on a dirty tree, so the env-cache + AO-hash work
claims no isolated win. Roadmap page updated to match.

## [2026-10-08] ingest | irradiance-volume page — emitter-lit indirect, and why frame means cannot verify it

Landed the CPU bake (`src/voxel/irradiance_volume.{hpp,cpp}`, 64^3 RGBA16F, binding
26), the self-contained `shaders/common_irradiance.glsl`, CMake registration, and
three camera-free test cases in `tests/test_world.cpp` (31 assertions; `ninja -C
build test-world` green, 2/2). Descriptor/upload/shading integration is the
shading session's files and is deliberately NOT done. NO RENDERED FRAME HAS SHOWN
THIS YET — the page says so explicitly, because "landed and tested at the data
level" is not "looks right".

Page: [[concepts/irradiance-volume]].

FOUR THINGS WORTH KEEPING FROM THIS, in decreasing order of how much they cost to
learn:

1. **A test caught a bug that no frame mean would ever have.** A volume cell
   CONTAINING an emitter came out unlit: the bake skipped `dist <= 1e-4f` to avoid
   normalizing a zero-length direction, but `(1-d/r)^2` is 1.0 at d=0, so the
   emitter's brightest cell was a black hole while every neighbour lit correctly.
   Found by `CHECK(hereLuma > 0)`, not by looking.

2. **Frame means are BLIND to localised lighting — measured, not argued.** Landing
   the 14 emissive-derived lights moved the day arm by +0.00 / +0.01 / +0.02 out of
   ~110-124, about 0.02%. So this feature's natural verification ("did the frame
   get brighter") would have returned "no effect" and been wrong in the OPPOSITE
   direction from a false positive. Two consequences: the night band is valid but
   nearly vacuous with respect to lights (a 20% change in every lamp would pass
   it), and a lights-only control is NOT worth building — if turning the lights off
   barely moves the day arm it cannot move the night arm enough to matter. The
   instrument with power is a LOCAL interior-region mean. Same blind spot as
   [[concepts/splat-edge-fade-measurement]] and the tile-parity work.

3. **Correcting an earlier claim of my own, twice, in one day.** (a) The `shRaw`
   "backend asymmetry" was a false positive, defeated by a guard inside
   `aoShEnclosure`; retracted to both peers before either acted, and the guard is
   now in AGENTS.md as intended design. (b) "shade+bucket is flat across sun
   states, so shadowMarch is small" was CONFOUNDED — my three arms were separate
   processes and the night arm was the cold first run, so the cold penalty
   cancelled the night saving and I read the cancellation as "no effect". Real
   effect is ~650-800 ms across two independent methods; the perf session's
   control is what caught it. Both retractions are recorded on the pages that
   carried the wrong claims.

4. **Include-order trap worth knowing before it costs an afternoon.**
   `common_irradiance.glsl` uses `pc.b.x`, and `pc` is declared by the ENTRY POINT,
   not a shared header. Wrong order fails with `'pc' : undeclared identifier` AND a
   second error reading like a typo (`'b' : vector swizzle selection out of range`)
   rather than an ordering mistake. `common_surfel.glsl` is included before `pc`
   exists, so that chain can never host it. Documented in the header itself.

Shared-emitter-set coupling recorded explicitly: the volume consumes the same 16
`LightUBO` slots as `applyLights`, so truncation is SHARED — consistent, but a
dropped emitter has no symptom of its own, hence the `seen`/`used`/`cellsLit` stats
the caller must log.

Also retracted a retracted thing: my own `stats.used` was hardcoded to report 0
(flag never set), found by reading the diff rather than by the compiler.

**[2026-10-08] ingest | Tile-splat parity canonical page created**

`[[concepts/tile-splat-parity]]` now carries Vega's measured 2026-10-08
status verbatim: forward-vs-tile mean|d| 5.5624/255, 64.31% of px
differ, 47.64% >2/255, max|d| 153, ~119x the same-binary control floor
(0.0467/255 forward, 0.0547/255 tile); localisation shows sky at the
floor, non-sky at 10.134/255, uniform top-to-bottom, no luma bias.
Seal-equality mechanism recorded as REFUTED. Timings
(143/57/65/78 ms) recorded as doc-sourced only — VF_TRACE silent in
--shot. Roadmap page now points at this canonical page. Vega will
restore the [[concepts/tile-splat-parity]] link in her irradiance-volume
page and drop its pending blockquote.

## [2026-10-08] correction | Night band re-derived unchanged and empirically confirmed blind; surfel counts reconciled

**The night-band banner I filed earlier was wrong, and Vega was right to
challenge it.** It said the 0.10–0.27 ratio band "no longer carries the tree it
was measured on" and told the reader a passing gate could not be trusted. The
measurement came back **identical with the 14 derived lights present**:

| arm | pre-emissive | with derived lights | delta |
|---|---|---|---|
| hero | 0.185 | 0.185 | 0.000 |
| house | 0.143 | 0.143 | 0.000 |
| water | 0.229 | 0.229 | 0.000 |
| day absolutes | 123.88 / 105.41 / 111.88 | 123.88 / 105.42 / 111.90 | +0.00 / +0.01 / +0.02 |

So the margin does not need re-deriving. The legitimate risk was closed by the
measurement, and the banner was then **actively harmful** — it trained the reader
to dismiss the one signal that would catch a real regression. Retitled to
**RE-DERIVED 2026-10-08, UNCHANGED**.

**Why it held (Vega's mechanism, which I had wrong):** the derived lights are in
**both** arms, so they largely cancel in a ratio — a ratio of two quantities that
both moved is not evidence that either moved. And at the three exterior canonical
cameras the emitters contribute very little solid angle to a frame mean: 14 lights
in a hamlet, seen from outside, move the day mean by **+0.02/255**. The second
reason is the one that generalises, and it is a property of the *cameras* rather
than the lights, so it holds for any future emitter set placed inside the hamlet.

**The corollary is the finding: the band is now empirically confirmed as blind.**
A 20 % change in every lamp would pass it. That is a *demonstrated* argument for
the scale-bearing day-mean assertion rather than an argued one — the strongest
form the thesis can take. The gate's robustness to content change and its
blindness to content change are the same property: scale-free by design, and a
scale-free check cannot detect a scale change.

**A lights-only control is a decided "do not build", with the reason.** The
`VF_TEXTURES=0` confound is real (it swaps photo→palette albedo in both arms, so
any delta is unattributable — house's night went *up* while its day also went
up, and removing light sources cannot raise night luma). A dedicated hook was
considered and rejected: if the lights off moves the day arm by +0.02/255, it
cannot move the night arm enough to matter for a ratio band. **Do not spend a
hook on a 0.02/255 signal.** Recording a rejection with its reason is worth more
than leaving it open — an open question gets re-litigated by everyone who sees it.

**Lesson for the next banner:** a warning that outlives its evidence is worse
than no warning. The banner was correct when filed and wrong one measurement
later, and nothing in its wording said which state it was in. A banner should
carry its own expiry condition, or it should be removed the moment the risk
closes.

**Surfels are not one number — reconciled (Victor, 2026-10-08).** The current
bake logs **4.19M** = 2.11M terrain + 0.79M object + 64k edge + 774k LOD1 +
517k LOD2. So the **5.0M** on `load-time-field-build` was stale (object was 1.3M
then, now 0.79M, and edge/LOD were not broken out), and the **3.4M** on
`per-object-voxel-size` was base-only before the LOD rings existed. Both pages
updated, and `per-object-voxel-size` now carries a three-row table of what each
figure counts, because a surfel count quoted in a storage-scaling argument has to
be the same count as the one in the cost model.

**Two figures flagged on Vega's new `concepts/irradiance-volume.md`:** the
"~7.7 s world load" contradicts the canonical ~17.6 s used everywhere else (it may
be a warm reload, but it does not say so), and binding 26 is new — which means the
descriptor pool must grow for it, the same trap that made the first lamp test a
silent no-op at binding 25. Both flagged on the page rather than silently
corrected, since they are Vega's measurements to confirm or re-label.

## [2026-10-08] ingest | GPU-perspective analysis: per-object finer voxels have three independent blockers

Filed a rendering-side section on `[[concepts/per-object-voxel-size]]` to
complement Fledge's representation-side summary. The two are complementary, not
competing — both must hold for a finer-lattice object to work.

**Fledge (representation):** CPU-side format work, splat path nearly free, SVO
brick format is the blocker.

**Me (rendering):** the splat *rasterizer* is indeed nearly free — the shader
does not care about absolute disk size, so mixed-scale surfels in one pass are
feasible. The blocker is **coverage**: a 10 cm terrain surfel is a 10 cm disk
that extends beyond a 1 cm vase wall's coverage and **pokes through** it.
Fixing that means splitting coarse surfels at fine-object boundaries — a CPU
cost, a complexity cost, and a break of the single-merged-record-set invariant
that `VoxelField`, SVO, surfelize, overlay and picking all rely on.

**Three independent blockers, not one:**
1. *Representation* — SVO bricks are fixed 8³ cells at 0.1 m; a finer object
   needs finer bricks, which changes the brick **format**, not just a decode
   constant. Deepest change, and it is the reference backend.
2. *Rendering* — the coverage problem above.
3. *Bake* — the EDT is already bbox-bound at ~13 s; a 1 cm object has a
   bbox-to-content ratio far worse than 10 cm terrain, so the padding inflation
   gets *more* severe.

**The one GPU answer that works: render the fine object as triangles.** The
mesh is already there (STL/OBJ). A standard rasterizer handles arbitrary
resolution with no coverage problem (triangles have no disk that can poke
through), no second octree, and no EDT. Composite through the shared depth
buffer — the depth test handles occlusion in both directions. Costs are real
but different: a second PBR shading path, and shadows consistent with the
voxel world's baked shadows. Those are *shading* problems, not *geometry*
problems.

**Verdict:** for a hero prop where the silhouette matters, mesh-as-triangles is
the better trade. For everything else, the detail layers are the better trade.
The lattice stays at 10 cm.

Also flagged by Fledge: `AGENTS.md`'s "small fp deltas" line is stale. Not
owned by either of us, but a stale known-issue line trains readers to ignore
the file — worth correcting or removing.

## [2026-10-08] coordination | Ownership map settled across five sessions

Coordination round closed with no collisions outstanding. Recorded so a future
session does not have to reconstruct it:

| session | owns | state |
|---|---|---|
| George | the 7 irradiance plumbing files + `AGENTS.md` doc corrections | in flight |
| Wiki | `concepts/per-object-voxel-size.md` **body** | folding in SAT-inflation, thin-structure admission, two caveats |
| Fledge | `index.md` + `concepts/improvement-roadmap.md` + presence-vs-uniqueness lint rule (roadmap P3) | duplicate index line collapsed |
| Wendy (me) | `concepts/measurement-discipline` rules + `concepts/night-gate-thresholds` | corrected |
| Victor | assets authoring + perf controls | idle GPU lanes |

**Two findings from Vega's closing summary, both worth keeping:**

1. **Binding 26 is declared but compiled by nothing.** `common_irradiance.glsl`
   declares `binding = 26`, but nothing `#includes` it yet — 0 irradiance hits in
   `app.hpp` / `splat_pass` / `svo_pass` / `world_textures`. That is the
   stale-shader hazard in miniature, and worse than a stale shader because a
   stale shader at least *ran*. An unreferenced header proves nothing about the
   runtime while looking verified. **A fifth instance of the "correct value,
   present, unread" category.** Should be recorded on the irradiance-volume page
   when the plumbing lands, because the next session will see "binding 26" and
   assume it is live.
2. **A dropped `max(distance, VOXEL*0.35)` floor in Vega's port** — numerically
   identical today, wrong-by-coincidence if `VOXEL` or the `0.85` ever changes.
   Same shape as `kMoonCol = 0.62`: right for reasons that are not the ones in
   the code. The reason is written at the site so it cannot be "simplified" back
   out — the correct defence, and the same one as `aoShEnclosure`'s guard
   comment.

## [2026-10-08] correction | My bridge line to the normalizing layer-load was wrong; filed the correction

Wiki landed `worldfile::resampleRecords()` + a per-layer `"scale"` field on
`feature/per-object-scale`. They approved a one-line bridge from my GPU section
to their implementation, and I had drafted it as: the normalizing load "sidesteps
the disk-pokes-through issue entirely, because the fine object's records become
coarse records before they reach the surfelizer."

**That was wrong, in the direction that overstates.** It assumed the normalizing
load preserves fine geometry. It does not — it resamples *into* the world
lattice, so it sidesteps the coverage problem by **giving up the fine geometry**,
not by solving it. Wiki's own status line is the accurate one: authoring
convenience and coverage, still no sub-10 cm geometry.

This is the retraction I recorded twice today, in the one place I did not expect
it: I wrote a bridge from a section I had not reread after someone else edited
the page it pointed at. The rule is the same one — **an assertion written before
the thing it describes is settled gets filed as a claim, not as a hedge**, and a
hedge would not have saved this either because the wrongness was in the
mechanism, not in the confidence.

Filed the correction as a table instead, because the two halves are in tension
rather than complementary and the page should say so:

| | preserves fine geometry? | coverage | what it buys |
|---|---|---|---|
| `resampleRecords` (landed) | **no** — resampled into the world lattice | clean (one lattice) | authoring convenience, correct scaling |
| fine-raster surfel stream (**not built**) | yes | fine stream must occlude the coarse one correctly | actual sub-10 cm geometry |

**The cost analysis is not where intuition puts it.** Wiki measured the surfel
bill for the finer vase at **~0.27 MB** — noise against the ~3.4 M-surfel scene,
so the count is *not* the problem. The case against a fine-raster stream is
**correctness, not budget**: the fine stream must occlude the coarse one
correctly, and SVO/voxel-fed marches (shadow, water DDA, irradiance) still see no
detail. Hence the unbuilt design keeps a **coarse field for shadow/picking**
alongside a **fine surfel stream** — two representations, one visual.

Manifest field, for the record: `scale` on a layer object in `world.json`
(sibling of `"pos"` / `"rot"`), a **unitless size ratio** (0.5 halves the
object's physical size), **default 1.0**, **rejected to identity when not > 0**,
applied about the layer's **bottom-center pivot**.

**[2026-10-08] ingest | Per-object voxel size analysis filed**

`[[concepts/per-object-voxel-size]]` captures the GPU-perspective
reasoning: surfels are resolution-blind so the splat path absorbs a
per-layer lattice nearly free; the SVO brick byte format (CPU
`int(d/VOXEL)`, shader `raw*VOXEL`, DDA `sdf <= 0`) is the real blocker;
first-wins-a-cell merge cannot hold across two lattices, so merge must
move to surfel level; VXW v2 sections can already carry per-layer
metas. The practical alternative — expressing object detail via
micro-surfels, crease bridges, anisotropic footprints, texture detail —
is documented as the cheaper path.

**[2026-10-08] lint | Presence-vs-uniqueness gap in the wiki check**

Vega's whole-tree check reported "total dangling: 0" while
`index.md` carried a duplicate `per-object-voxel-size` entry — both
links resolve, so presence passes. Rule: occurrence count exactly 1 per
page, not merely "the link appears". A check weaker than the claim it
supports is the same failure family seen three times today (noise-floor
vs delta, gate presence vs isolation). Filed as a P3 on the roadmap.
Duplicate index line collapsed; page body ownership settled on Wiki
(CPU/format half by Fledge + rendering/coverage half by Wiki); roadmap +
index owned by Fledge.

**[2026-10-08] lint | Whole-tree link/orphan/duplicate sweep: clean**

Ran the presence + uniqueness + orphan check across `.opencode/wiki`:
no orphan pages, no page missing from `index.md`, and **no duplicate
entries inside the Pages section**. Refinement to the uniqueness rule
recorded earlier today: the naive whole-file count reports ~35
"duplicates" because the "How to navigate" section deliberately
cross-references Pages entries — uniqueness must be checked **within the
Pages section**, not across the whole file, or the rule cries wolf.
The one accepted dangling target is unchanged:
`[[concepts/water-flooding]]`, cited only by historical log entries
(the concept was folded into [[concepts/water-plane]]).

**[2026-10-08] lint | Settledness added as a third category; Pages-scoped uniqueness**

Verified `log.md` after Wendy's concurrent append (her correction at
line 4360 sits cleanly before both of my lint entries — no clobbering;
`edit` on a stale match fails rather than overwrites, but it was checked
rather than assumed). Lint rule now has three categories: presence,
presence-vs-uniqueness (scoped to the Pages section), and
**settledness** — a cross-reference written before another session's
build settles is filed as a claim, not a hedge, and a hedge would not
have saved Wendy's `resampleRecords` bridge line because the error was in
the mechanism (resampling *into* the world lattice gives up the fine
geometry rather than preserving it), not the confidence.

## [2026-10-08] finding | Per-object voxel sizes: analysis consolidated, the "minimal honest change" landed

Answered the user question "can objects have a different voxel size?" end to
end. Analysis (shared across four sessions, consolidated in
[[concepts/per-object-voxel-size]]): the splat path absorbs fine surfels for
instance count (a 0.3 m prop goes ~34 -> ~540), but NOT for coverage (smaller
disks need more overlap to seal; the documented thin-structure failure) and NOT
for the marches (shadow / water DDA / irradiance all read the coarse field).
SVO brick format is the representation blocker; finer resolution can make LOAD
slower because components split into more padded-bbox EDTs.

Implementation, on branch `feature/per-object-scale` in the worktree
`/home/christoph/code/voxelforge-per-object-scale`:
`worldfile::resampleRecords()` + per-layer manifest `"scale"` (unitless size
ratio, default 1.0, about the bottom-center pivot). Object/scatter layers
authored at any `voxelSize`/`gridN` are resampled into the world lattice with
dominant-material voting; `LayeredWorld`'s cache re-normalizes on scale change
and its dirty hash folds scale in; `EditableWorld::importLayer` resamples
meta-mismatched sources. `test-world` green, `--selftest` green. The
fine-raster surfel stream is NOT built.

Wording that matters for the next reader: `resampleRecords` and a
fine-raster surfel stream are a TRADE, not two stages of one plan —
resampling sidesteps coverage by discarding fine geometry, not by solving
coverage. The count is not where this fails; correctness is.

Measurement filed (real `assets/vase.vxw`, 10 cm -> 2.5 cm): 869 -> 55,616
cells, 1,046 -> 16,736 exposed faces (16.0x), ~0.27 MB of surfels against a
~3.4 M-surfel scene.

Recorded here rather than in a concept page, because
`concepts/measurement-discipline.md` belongs to Wendy (log line 4349) and I
had edited it on Vega's request before that ownership was corrected; my hunk
is reverted and the line is hers to write. The rule, arrived at twice today: a
test that re-implements the expression it is testing cannot falsify it — 232
agreeing assertions certified a 51.2 m / 32-cell coordinate error because the
helper copied the implementation's own formula. The external reference point
is the whole content of a correctness check.

Coordination cost worth recording: three bare "continue" messages in a row
mean "file the thing", and two sessions independently believed they owned the
same wiki page body until a third arbitrated. Claim before edit, every time,
including for the file you already released.

## [2026-10-08] fix | irradiance volume: origin-centred cell frame (51.2 m / 32-cell misregistration)

Found by the shading session diffing my CPU bake against my own GLSL, not by the
test suite. Confirmed independently on three axes before changing anything:
`VoxelField::sampleWorld` uses `(p + 0.5f*WORLD)/VOXEL`; the shaders use
`p/pc.b.x + 0.5`; `heightmap.hpp`'s `kHmMinMeters = -8.0f` means world
coordinates are routinely negative. `--probe` corroborates (origin reads air
above the hamlet, (51,0,51) reads solid).

Fixed: `p = (i+0.5)*kCell - WORLD*0.5f` on all three axes, and the test's own
coordinate handling — `irrCellCentre` forward and a new single `irrCellIndex`
inverse, replacing two inline `int(air.x / c)` conversions that were also 0-based.
The real geometry of the bug was that asymmetry: one forward call site (reviewable)
against two inverse ones (not).

Why 232 assertions passed: `irrCellCentre()` reproduced the bake's own formula, so
bake and test formed a closed loop with no external reference point. With the bake
fixed and the test left 0-based, `stats.used` came back 0 — the emitter reached no
cell centre in the real frame.

New case `irradiance volume: the cell frame is origin-centred` breaks the loop by
checking `kOriginOffset` against `sampleWorld`'s conversion written out longhand.

Assertion count FELL 232 → 152 and that is the finding, not a regression: the ray
walk covered 186 cells over 6 axis rays in the broken frame versus 89 cells over 3
correct — the emitter in the wrong half of the world travelled further through open
space and asserted about ~97 cells unrelated to the light. Both figures read by
forcing the gate to fail (`CHECK(checked > 1e6)`), since a passing `checked > 20`
never discloses the value. Free-space energy after: 585.06 against the pi*r^3/3
ceiling 2873.51, ratio 0.204.

Verified: `vf_tests --test-case="*irradiance*"` 6/6, 152 assertions; `test-world`
2/2 in 109.8 s; `build/voxelforge` md5 verified untouched at every step. Restores
during measurement were md5-verified, not mtime-verified — one build failed and
silently ran a stale binary, which is how a wrong number nearly got reported.

Aphorism and the general rule are on [[concepts/measurement-discipline]] (Wendy's,
instance nine); this entry carries only the concrete instance. Source:
[[concepts/irradiance-volume]].

## [2026-10-08] lint | Structural-lint rules filed (three axes); log ordering caveat

Filed on [[concepts/measurement-discipline]] under "Structural lint: checks that
need no judgement":

1. **Orphaned table rows** — a `|` line not preceded by a row is an orphan unless
   it is itself a header (separator follows).
2. **`edit` anchored on a heading deletes it** — re-emit the anchor, then verify.
   Instrument `git diff -U0 | grep '^[+-]## '`; blind spot: invisible to a heading
   added *and* deleted in one uncommitted session.
3. **A heading split from its body** — anchoring on a heading and inserting below
   it separates the two. Nothing is missing, so presence checks pass; only
   `grep -A3 '^## '` sees it. This is the order axis: the two others check
   presence, this one checks arrangement.

Applied: repaired two headings and a duplicated table row on
[[concepts/measurement-discipline]]; wiki now 54 pages, zero catalogue gaps, one
deliberate dead link (`concepts/water-flooding`), no orphaned rows, no
heading/body splits.

Also recorded there: the compiler-enforced constraint family from the irradiance
fix (test's own `indexOf`, `static_assert(sizeof(glm::vec4)==16)`, `kBytes` tied
to `kN^3 * sizeof`, byte-copying upload); **a model of an instrument is not the
instrument** (Python model predicted 4 failing cells, compiled code failed on 8);
and a **failed build can leave the previous binary running**, so *which source was
written* and *which artifact ran* are two questions needing md5 + HEAD + dirty
paths. Suite total is **153** (not the 152 first reported).

**Wiki-layer caveat:** `log.md` is append-only, which mitigates lost *text* but
not misrepresented *ordering* — and this file already shows it, with a
correction entry sitting above an earlier finding. Do not infer causation from
vertical position.

## [2026-10-08] LEAD (not a finding) | irradiance volume: slice loop used slice COUNT as a z STRIDE

**Deliberately held out of every content page** until `vf_core` is green and the
suite has been re-run through `ninja` rather than a hand-link. The general lessons
*are* filed, on [[concepts/measurement-discipline]]; this entry is the lead.

**Mechanism (reported; verified in the author's own CPU-only translation unit, no
GPU/display):** `buildIrradianceVolume`'s slice loop used `sz * kSlice` as the z
stride, where `kSlice` is the **slice count**. With `kN=64`, `kSlice=4` the
covered band is `z ∈ [0,16)` of 64 — a quarter of the volume, and the quarter at
the world edge. 15 of 16 emissive clusters sit near `z +7..+15`, entirely outside
it. Fixed to `kZPerSlice = kN / kSlice`; measured 8 of 14 used, 13 cells lit, max
RGB 0.987.

**Configuration — the figure without this is the defect:** `assets/world.json` as
shipped (14 derived lights, 0 authored), 21 layers merged, `latN=1024`, `kN=64`,
`kCellM=1.6`, `sunDir` normalize(-0.4,0.6,-0.5), CPU only, `.o` linked directly
against a **pre-existing `libvf_core.a`** because Victor's collision broke the
`editable_world` header seam.

**Why the tests passed anyway — the part that generalises:** 6/6, 153 assertions.
The tests **place a synthetic emitter wherever `findOpenAirCell` lands**, and the
truncated band happened to contain that spot. A test that **chooses its own
subject** cannot detect that the subject set is a fraction of what it should be —
and a hand-computed expected value would have passed too, if computed over the
same band. The fix is a **census**, not an oracle: assert what was *visited*.

**Method point:** two instruments disagreed (40 candidate pairs in range, 0
visited) and **the disagreement was the signal** — while both hypotheses reasoned
about visibility and resolution, i.e. the physics, and the defect was in the
iteration bounds. Also: the census built to test one hypothesis invalidated
**both**, the third purpose-built instrument today to answer a neighbouring
question.

**Pending:** negative control (revert the stride, expect a fail) is queued but not
run. Promoted to a content page only on a green `ninja` run.

## [2026-10-08] rule | A guard defined relative to its feature disappears with the feature

Filed as a standing review question on [[concepts/measurement-discipline]],
from the micro-surfel removal (ordered, not landed): the live-edit check
asserted "the patched run grows with `VF_MICRO` on vs off" — an assertion
*about the feature existing*, not about the protected property ("a patched
chunk's regenerated run still covers the cells the stamp touched"). Deleting
the feature deletes that assertion in the same commit, leaving a guard with a
hole shaped exactly like the deletion. Cheap review form: after any deletion,
grep the surviving checks for the deleted identifier; prefer replacements that
name no part of the removed system. Wiki's replacement plan already matches
(non-empty patched run, count differs from pre-stamp, base+bridges
regenerate), per George.

## [2026-10-08] finding | Only the carve could have caught the rim class

Fledge's distinction, filed on [[concepts/brush-falloff-curve-contract]]:
dome/sphere tests assert reach monotonicity and proportional shortening, not an
exact boundary count — so a lost boundary cell there does not fail. The carve's
`found == expected` over the whole top-layer disk was the only exact count,
which is why 2814-of-2821 surfaced there. Until boundary-count checks exist for
the dome/sphere rims, green proves the carve boundary and the un-tapered paths,
not the tapered dome/sphere boundaries. Boundary-count proposals for those two
rims are authorized from the contract page (spec sign-off; implementation stays
in Fledge's `test_editable` + clean-build gate). Contract page deliberately
still spec-only until `vf_tests` is green.

## [2026-10-08] status | Victor Phase 1 reported green on its own cases

Recorded as reported, not wiki-verified, on
[[concepts/per-object-voxel-size]]: `importLayer` hunk re-applied, `vf_tests`
builds, 4 worldfile cases green (429 assertions incl. fine-grid resample). One
remaining failure, `test_world.cpp:613 slicesWithSky` (4096 vs 64), attributed
to Vega's dirty test plus untracked irradiance files — not the merge path
(byte-identical at scale 1). Confirmation routed to Vega; treated as reported
until he confirms. Phase 1 boundary holds (representability only, no fine
geometry); `scale` fifth-field note and three-behaviours doc debt stay pending
until merge.

## [2026-10-08] verify | Falloff-curve contract implemented-and-verified

Fledge reports `test_editable` green: 13/13 cases, 3013/3013 assertions. Filed
on [[concepts/brush-falloff-curve-contract]], mechanism-precise:

1. **Carve** — taper reach floored at `kCarveTopMargin + VOXEL` plus the SDF
   epsilon (the 2814-of-2821 fix, as specified).
2. **Dome** — graded-column kill: `colHeight < 0.5 * VOXEL` emits nothing, gated
   on grading so `Constant` keeps its exact legacy footprint. The leak was the
   straight-disk branch testing ungraded `perp2 <= r2` — rim kept its base cell
   (reach 1, now pinned as nothing-at-rim). General rule: the inclusion test
   must see the tapered quantity, never the un-tapered one.
3. **Sphere** — no fix, structurally exempt: `dist <= R * f(q)` grades both
   sides of the comparison together, so the class is not expressible. Prefer
   this form wherever the predicate allows it.

**3053 → 3013 is the suite getting tighter, not thinner:** same 13 cases, and
the count comes from per-record `CHECK` loops — fixing the boundary emits fewer
cells, so the count falls by construction. Recorded on the page so a future
reader does not misread a falling total next to a boundary fix.

Boundary-count proposals for the dome/sphere rims (exact-count over the rim
ring mirroring the carve's `found == expected`, asserting zero cells at `q=1`
for every tapered curve): authorized from the contract page, implementation in
Fledge's gate.

## [2026-10-08] verify | Slice-stride lead promoted (current-tree label)

Vega reports the promotion condition met — `vf_core` green (`ninja vf_tests`
links), irradiance **7/7 cases, 180/180 assertions** via the ninja-built
binary, `test-world` group 2/2 via `ninja` (110.66 s),
`build/voxelforge` md5 untouched throughout. The stale
"not yet verified through `ninja`" line on
[[concepts/measurement-discipline]] is corrected to this green run.

**Label travels with the promotion:** Vega's 4 files are uncommitted, so this
is a **current-tree** green, not a hash green. The 6/6-153 figure stays on the
page as the bug-present run; 7/7-180 (with the fixed-subject seventh case)
supersedes it. Vega's slice-stride section, 4 MB RGBA32F correction, and
Fledge's two applied nits live on his page — this entry is the pointer, not
the restatement.

## [2026-10-08] verify | Dome/sphere boundary gap closed — 15/15, 3037/3037

Fledge's boundary-count checks pass: dome rim ring zero across all five tapered
curves (legacy path guarded non-zero, kill gated on grading as specified),
sphere a full cell inside with `Constant` bit-identical. Recorded on
[[concepts/brush-falloff-curve-contract]] as the closure of the "only the
carve could have caught the rim class" gap — every tapered boundary is now
proven, and the page status reads 15/15-3037. Authorization for those checks
came from the contract page; implementation stayed in Fledge's gate throughout.

## [2026-10-08] status | Victor Phase 1 settlement part one — landed in worktree, uncommitted

Recorded as reported on [[concepts/per-object-voxel-size]]: coordinator call
gated on Fledge's 15/15 green accepted 5 port files + the `importLayer` hunk
into the worktree, uncommitted. A landing state, not a merge state — the
pending-merge note and all branch figures stand. Still open:
`slicesWithSky` attribution with Vega (reported-not-settled) and the
texture-header recurrence (awaiting owner re-apply; unfiled until confirmed).

## [2026-10-08] finding | Checkout-loss recurrence instance two — `TextureBinding::emissive`

Settled form arrived from two sessions and is filed on
[[concepts/uncommitted-edit-is-not-yours]] next to the FalloffCurve instance:
the extension lived in `worldfile.hpp` (`TextureBinding::emissive` bool +
`emissiveScale` float) + `worldfile.cpp` (textures-table parse/emit), taken by
a port checkout in a dirty tree, re-implemented by Fledge **from the usage
sites** under coordinator authorization. Scratch round-trip PASS +
byte-identity PASS; the durable committed gate is still open
(`test_worldfile.cpp` is Victor's claim). Attribution stays
prime-suspect-unconfirmed — filed as mechanism, not culprit. Two instances in
one day promotes the prevention from `assets/` advice to an any-tree rule.

## [2026-10-08] decision | Emissive-lights spec change ordered, pending Wiki landing

Coordinator order (topic `emissive-lights`): every emissive voxel becomes a
**shadow-casting point light**, superseding the 1.5 m thinning rule and the
`kMaxLights` = 16 cap. Filed as a pending banner on
[[concepts/enclosed-space-lighting]] — the current chain (thinning, 16 slots,
authored-first, truncation logged) stays as the description of the tree until
the landing, because a per-voxel light set does not fit binding 25's fixed UBO
shape and the landing must restructure the path. Reconcile on landing: the
banner comes down and the section is rewritten against the new path.

## [2026-10-08] decision | Full dynamic lighting ordered — priority after micro green

User order via George (topic `dynamic-lighting`): sun + lights re-derive live —
no baked shadow, no stall, no 16-cap. Filed as a pending banner on
[[concepts/dynamic-sun-shadows]], superseding the static/thinned chain
([[concepts/baked-sun-shadow-contract]],
[[concepts/enclosed-space-lighting]]). Owner split: **Victor shadow-map,
Wiki cells-emission** — and both greener paths are recorded as **prerequisite
gates**, so the decision lands when both are green. Sequencing: after micro
green. Reconcile when
both prerequisites land: banners down on all three pages, sections rewritten
against the live path.

## [2026-10-08] verify | Micro removal landed — hybrid shape, current-tree label

Wiki's removal is implemented in the working tree (AGENTS.md diff +
`git status` showing the app/test/doc hunks, all **uncommitted**). Verified by
grep and recorded on [[concepts/detail-pipeline]] §3, which replaces the
DECIDED-NOT-LANDED banner:

- **Deleted:** `U` handler, `App::m_microDetail`, all `VF_MICRO*` env reads
  (two comment-only strings remain), sidebar toggle, every test reference.
- **Retained, default-off:** the bake emitter (`emitMicroSurfelsForCell`,
  `buildMicroSurfels`, `microStart` layout; `microDetail = false` at
  `surfelize.hpp:54`). The cited lines `app.hpp:351` / `run_hotkeys.cpp:298`
  are **gone**, as promised in the re-verify — the re-verify also found the
  retention, which the inventory had not predicted.
- **Guard risk closed:** `check_patched_run` asserts a non-empty patched run
  and names no micro — the specified replacement, not a deletion.
- **Cost table restated** on [[concepts/load-time-field-build]]: `VF_MICRO=0`
  is the default, +56.7 % / +1.23M is removed cost kept as provenance,
  `3,398,081` labelled with-micros historical.

Label travels with all of it: **current-tree until commit**.

## [2026-10-08] status | Dynamic lighting in progress — N=64/K=4, visibility proof required

George's update (topic `dynamic-lighting`): the decision is now **sole priority**,
**in progress** with config **N=64/K=4, unbounded-adjustable** — Victor codes
the shadow-map lane, Wiki codes the emission path, both greens as gates.
Recorded on [[concepts/dynamic-sun-shadows]] (banner now reads IN PROGRESS).
New and load-bearing: the user reports **zero visible change**, so landing
additionally requires a **visibility proof** — a measured frame delta above the
noise floor on at least one canonical view, not just green gates. A lighting
change with no visible effect is a clean zero, and clean zeros need the
enumeration precondition from [[concepts/measurement-discipline]].

## [2026-10-08] lint | Wiki cleanup sweep — micro-as-current scrubbed, caps marked interim

Coordinator-ordered sweep (topic `wiki-cleanup`), 11 files. Micro-as-current
removed everywhere found: edge-bridges layout + `VF_MICRO_DIST` + micro tail
([[concepts/edge-aware-surfel-radius]]), 2.13M/3.40M range and `U`-obligation
([[concepts/dynamic-sun-shadows]]), `VF_MICRO=0` run note
([[concepts/splat-base-seal]]), detail-budget list
([[concepts/per-object-voxel-size]]), third-contributor diagnosis
([[concepts/surfel-holes]], kept as history), `no_micro` variant
([[concepts/demo-capture]], env half now a no-op), `U`-stall comparison
([[concepts/sun-direction-pipeline]]), test-clause rewrite
([[entities/live-edit-brush]]), catalog line (`index.md`), plus the layout
contract rewritten as three-declared-arrays-two-live-ranges
([[concepts/detail-pipeline]]). Historical measurements keep their config
labels (they were true on the frames they were made on).
Thinning + 16-cap marked interim till the N=64/K=4 flip
([[concepts/enclosed-space-lighting]], [[concepts/night-gate-thresholds]]);
UBO in-flux note verified against the tree — `worldfile.hpp:147` already reads
`kMaxLights = 256` / KMax 8 / budget 64 / K 4, so the flip is in-tree but
unverified (15:12 binary `bad_alloc`, Victor rebuilding). Emissive banner
aligned to IN-PROGRESS-with-gates vocabulary. Gone-line citations confirmed
absent wiki-wide (`app.hpp:351`, `run_hotkeys.cpp:298`); `surfelize.hpp:54`
cited as retained-off.

## [2026-10-08] lint | Cleanup sweep closed — exception + parked parity noted

Coordinator resolution on the two returned items: (1) the store-169/field-19
parity numbers live only in Wiki's uncommitted branch (parked-skipped parity
test) plus the crosstalk record — no wiki/code pointer exists yet, so the
no-guess stands and the landing reconcile picks it up when she lands; (2)
Vega closed by user, so `irradiance-volume.md:50` took a one-line interim mark
as an explicit coordinator-assigned exception to one-writer-per-page, scoped
to that line, no further edits to that page.

## [2026-10-08] lint | Quirk hygiene — 10 updated for the micro landing + N/K flip

User-ordered outdated-quirk pass via the plugin CLI (`lint` itself reports
healthy — it only checks confidence/duplicates, not session-invalidated
claims). 5 full rewrites: micro decision (ordered → landed-uncommitted
hybrid), micro toggle mechanism + LiveEditor cache invalidation + drag-paint
seed (all describe deleted code → historical), emitter single-source-of-truth
(live half deleted). 5 dated addenda appended to the originals: 16-cap
(superseded in-tree by N=64/K=4, uncommitted/unverified), LOADER-ONLY quirk
(writer landed + cap superseded; design parts stand), shadow-map-vs-march
("if ever wanted" → IN PROGRESS Victor lane), sun-change counts (3.40M now
historical), shotlist knob list (VF_MICRO dropped). Dated measurement records
left alone — a timestamped A/B is history, not a current-behavior claim.
Caught live during the pass: the sweep's own `demo-capture` edit named a
`no_micro` variant the tool no longer has (`record_demo.py` now ships `no_lod`
/ `VF_LOD=0`) — corrected to tool truth.

## [2026-10-08] reconcile | Dynamic-lighting banner-down closed — emission landed

Coordinator criterion met (flip landed + parity green). Banner-down + landed
facts across 5 pages + index: `enclosed-space-lighting.md` (provenance box
replaced with landed note; 16-cap/528 B rewritten to 256 slots at budget
192 + `follow` lamps; in-flux box replaced with landed box incl. night cost),
`irradiance-volume.md` (16 slots/budget/64^3x16 arithmetic to landed values;
historical incident numbers kept as history), `night-gate-thresholds.md`
(RE-BASELINE box with Victor's 169-light calibration hero 0.337 / house
0.336 / water 0.515; old table framed as pre-flip record; light-count line to
169/192), `dynamic-sun-shadows.md` (NOT-YET-LANDED banner rewritten to
half-landed: emission live, shadow-map lane open), `index.md` (4 bullets),
`sky-probe-is-a-camera-assertion.md` (cabin-occlusion known-issue note,
micro contribution audited 0.000%). Landed numbers: store enumeration 169
clusters (budget 192, knee 169, 3 probe-verified buried-lava ghosts excluded
from parity by exact position), parity 7/7, store group 2/2, night 169-light
cost 11.68 ms avg/150 frames 960x540 vs 9.51 ms at 16. Open, not wiki's:
Victor's re-baseline write-up (replaces the interim box) + shadow-map lane.
