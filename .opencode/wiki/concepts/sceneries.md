---
title: Sceneries — the world lives in scenery/<name>/
tags: [scenery, assets, world-json, paths, manifest]
sourceRefs: [src/voxel/scenery.hpp, src/voxel/scenery.cpp, scenery/LakesideHamlet/world.json, CMakeLists.txt, AGENTS.md]
lastReviewed: 2026-11-19
---

A **scenery** is a complete landscape: one folder under `scenery/<Name>/`
holding the manifest (`world.json`), the terrain (`heightmap.png` +
`landscape.vxw`), every object/scatter layer `.vxw`, the `micro/` tree (with
`.rig`/`.skin`/`.anim` side-cars), the chat/MCP layer (`ai_edits.vxw`), and
the per-scenery live-edit session overlay (`runtime_edits.vxw`). The shipped
default is [[entities/hamlet-scene|LakesideHamlet]]. The app uses the
**last-used scenery as the default** via the gitignored pointer
`scenery/last.json`.

## What does NOT move

`assets/` is now **only the shared library**: `textures/` and `models/`.
Textures are bound per MATERIAL (atlas layer == material id), models are
import sources — material identity, not scene content — so every scenery
shares them. The manifest's `textures` table keeps its `textures/<file>`
naming: `scenery::resolveShared()` tries the manifest dir first (a scenery
MAY carry a local override) and then the shared library, so a table authored
before the move survives it untouched.

## Path resolution

`VOXELFORGE_ASSET_DIR` stays the compile-time CMake macro, but it is only the
**shared library** now. Every world path resolves at runtime through
`vf::voxel::scenery::root()` (src/voxel/scenery.cpp), in order:

1. explicit `setCurrent()` / `--scenery NAME` (CLI = one launch, no pointer write),
2. `VF_SCENERY` (a folder name, or a direct path containing `/`),
3. the pointer file `scenery/last.json` (written atomically on switch),
4. `kDefaultScenery` (`"LakesideHamlet"`),
5. the first discovered `scenery/*/world.json`,
6. the shared dir (a broken/unbaked tree — the callers' existence checks then
   print the usual "run 'ninja -C build world'" message).

`root()` is memoised per process; the `--probe` path and App member
initialisers resolve during construction, so `main()` applies `--scenery`
before `App` is constructed. The heightmap lazy singleton, the store overlay
path (`VF_OVERLAY_PATH` still wins), `EditableWorld`'s layer+manifest paths,
`vf_mcp`'s world tools and the Python gates all ride the same resolver; the
gates pin `VF_SCENERY=LakesideHamlet` so a user's last-used pointer can never
redirect the test suite, and `tests/main.cpp` pins it for the unit tests.

## Consequences for the existing invariants

- "The app REWRITES world.json" — now per scenery: layer toggles, placements
  and texture swaps persist into the **active** scenery's manifest. The
  write-verify rules and `git diff scenery/` (was `git diff assets/`) as the
  shipped-scene check follow the move unchanged. See
  [[concepts/uncommitted-edit-is-not-yours]] for the attribution hazard.
- The `ninja clean` hazard moved with the bake outputs: `build.ninja` now
  registers `scenery/LakesideHamlet/{heightmap.png,world.json,landscape.vxw}`
  — never clean; recover with `git checkout -- scenery/…`. See
  [[concepts/authored-assets-are-build-outputs]].
- Switching scenery means a different overlay too: live edits are scoped to
  the scene they were made in, and an unaware headless run cannot rewrite
  another scenery's session file.
- `vf_mcp` (separate process) resolves the same pointer file, so chat/MCP
  authoring lands in the active scenery.

## Phase status

- **Phase 1 (landed 2026-11-19):** the resolver, the migration of the shipped
  hamlet, the CLI flag + env override, the gates, and the bake-target path.
- **Phase 2 (landed 2026-11-19):** the in-session switcher — World panel
  SCENERY group (combo + Switch), `App::switchScenery` with full rollback,
  overlay/ai_edits flushed to the LEFT scene before paths move, authored sun
  re-read, `last.json` written so the GUI selection IS the next default.
  Follow-up ticket: a `VF_TEST_SCENERY_SWITCH` headless hook so the switch
  itself can be gated (the repo's no-mouse-only-features rule).
  Bug found by the Phase-2 smoke test and fixed in the same pass: micro
  `meshSource` refs (shared-library STLs) resolved against the MANIFEST dir
  and broke the alpaca's mesh actor after the move — now scenery-dir-first /
  shared-library-second via `resolveShared`, same contract as textures.
- **Phase 3:** pointer-following inside one long-running `vf_mcp`, and the
  second authored scenery to prove the format (in flight: LighthouseIsle).
- [[concepts/large-landscape-streaming]] — the proposal for escaping the fixed
  102.4 m cube when a scenery outgrows it (streaming + LOD, still a design).
