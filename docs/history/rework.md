# Plan: Fully dynamic, records-only voxel world (VoxelsOnly branch) — FINAL

**Branch:** continue on `VoxelsOnly` @ `fd36a64`. The layer-enable-mask machinery from fd36a64 is replaced in-place; `ChunkPool` incremental rebuild is kept, re-keyed on layer mtime.

**Goal.** Runtime app + `svo_raymarch.comp` derive *all* geometry — objects **and** terrain — solely from `.vxw` voxel records + `world.json`. No `scene()`/analytic object SDFs/terrain SDF anywhere at runtime (C++ or GLSL). `heightmap_gen.cpp` stays as offline asset baker. Splats and dense renderer are removed entirely. Result: a high-performance dynamic voxel world editable by local AI without touching source code.

**Locked decisions:**
- Dense path (`raymarch.comp`, `RaymarchPass`, `DenseVolume`, `--compare`) is REMOVED (user instruction overrides rework2's retention). Gates remaining: unit tests, visual_check, selftest, probe.
- Procedural `World::build()` fallback RETIRED — missing assets = hard error ("run ninja world").
- Push struct: semantic trim + relocation into `svo_pass.hpp`; physical 128 B block retained.
- Docs: prune living docs; `THREAD_SUMMARY.md` stays as history.
- Staged commits, green tree each.

## What stays
Chunked-SVO renderer (`SvoPass` + `svo_raymarch.comp`) incl. TAA, water/underwater/fog/ACES, `LayeredWorld` hot-reload (0.5 s disk poll), `EditableWorld` (`ai_edits.vxw`), AI Chat UI (instant reload trigger), `vf_mcp` (9 tools), picking (Ctrl+LMB), layer toggles, heightmap_gen baker.

## Commit 1 — Remove splats
Delete: `src/render/splat_pass.{hpp,cpp}`, `shaders/splat.{vert,frag}`, `assets/env.hdr`.
Edit: CMake L131/L145-146/L164(tbb); main.cpp (`RenderMode`, splat Args+flags, `buildSplatData`/`rebuildSplats`, keys F/+/-/B/O/I/1-8/H, headless+selftest splat branches, HUD text, push.misc lanes); stale-splat banner chat_ui.cpp:226; comment mentions (mcp_server.cpp:8, layered_world.hpp:7, editable_world.hpp:14, common.hpp:659, picking.cpp:55, worldfile.cpp:448, heightmap_gen.cpp).

## Commit 2 — Remove dense + --compare
Delete: `src/render/raymarch_pass.cpp`, `shaders/raymarch.comp`, `src/voxel/volume.{hpp,cpp}`, `tests/test_volume.cpp`; slim header to push struct only.
Edit: CMake L84/L131/L143/L170; main.cpp dense block L474–614, `Backend`, `runCompare()`+flag+camera spawn, samplers/volumes/m_pass, dispatch→`m_svoPass.record`, HUD labels; resources.{hpp,cpp} samplers+mip chain out; visual_check.py dead `--backend svo`.

## Commit 3 — VoxelField = single records-derived source
- New `src/voxel/voxel_field.{hpp,cpp}` built by `LayeredWorld`: terrain per-column `terrainTop(xz)`+material (landscape role); objects EDT-SDF/mat/color over flood-filled solids; API `distance/material/color/solid/terrainTop`.
- `fillBrick` → field queries; remove mask machinery (`BuildCtx.enabledMask`, `treeBit`, `OB_*`, `m_enabledObjMask`, mask-aware scene() overload).
- `uHeight` RG32F emitted from `landscape.vxw` records; PNG upload dies; heightmap.png becomes baker-only.

## Commit 4 — De-analytic svo_raymarch.comp
Delete analytic object SDFs + getMaterialId + constant arrays (kHousePosG/kTreeSpots/kRockRadii/kBushCell). map(): objects ⇒ brick SVO SDF; terrain ⇒ `p.y − heightAt(uHeight)`; water plane y=-0.9, fog, ACES, bidirectional waterHit, gUnderwater unchanged. calcNormal: heightfield gradient + map gradient. Materials: terrain ⇒ uHeight.g; objects ⇒ brick word1.mat.

## Commit 5 — Purge consumers + retire fallback + docs
- picking.cpp, mcp_server probe/ground → field; chat_ui probe; system_prompt wording.
- Retire `World::build()` fallback — hard error pointing at `ninja world`.
- Drop aiEditsRegister/aiEditsAt; retarget test_authoring/test_world to VoxelField queries.
- Chat edits → immediate m_pendingWorldReload; push struct relocated into svo_pass.hpp.
- Docs: rewrite AGENTS.md/skills/wiki; delete implementation_plan.md; prune ImplementationPlan.md.

## Verification per commit
`ninja -C build && ctest --test-dir build`, `--selftest`, `--probe`; screenshots via `--shot` analyzed in detail. Finale: interactive smoke + vf_mcp add/probe round-trip.
