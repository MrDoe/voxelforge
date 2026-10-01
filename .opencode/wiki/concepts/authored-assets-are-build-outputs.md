---
title: The authored scene is registered as build OUTPUTS — any clean deletes it
tags: [assets, build, cmake, ninja, hazard, incident, world, gotcha]
sourceRefs: [CMakeLists.txt, tools/heightmap_gen.cpp, build/build.ninja, AGENTS.md, docs/world-format.md, docs/tooling.md]
lastReviewed: 2026-09-26
---

# The authored scene is registered as build OUTPUTS

`assets/` is **versioned, hand-authored scene content** — and CMake nonetheless
registers those files as **outputs of the `heightmap_gen` custom command**:

```cmake
# CMakeLists.txt:130
set(VF_HEIGHTMAP_PNG ${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmap.png)
set(VF_WORLD_LAYERS ${CMAKE_CURRENT_SOURCE_DIR}/assets/world.json
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/landscape.vxw ...)
```

which `build.ninja` materialises as a `CUSTOM_COMMAND` whose **output list is the
authored scene**. So the build system believes it *owns* files that are actually
source.

**Consequence: any clean deletes the authored scene.** `ninja -C build -t clean`
removes every registered output that still exists. Observed 2026-09-26: three
tracked files — `assets/heightmap.png`, `assets/world.json`,
`assets/landscape.vxw` — vanished mid-gate, and the app and every content test
refuse to start without `world.json`.

## Why exactly three, and not seventeen

The registered output list is **stale**. It still names the old baked object sweeps
(`house.vxw`, `tree1..6.vxw`, `rock1..3.vxw`, `bushes.vxw`, `alpaca.vxw`,
`fence1.vxw`), which were removed from the repo when those sweeps were dropped in
favour of runtime-authored `hamlet_*` layers. A clean can only delete what still
exists, so:

> deletions = registered outputs ∩ files on disk = the three that survive

That arithmetic is the fastest way to confirm a clean is the mechanism: **count
the surviving outputs and compare.** It matched exactly, which is what identified
the cause rather than merely suggesting it.

## The documented recovery is not a no-op — this is the real trap

`AGENTS.md` tells you the fix is `ninja -C build world`. That advice is correct
about the *symptom* and wrong about the *consequence*: running it **re-bakes the
terrain shell** from `terrainHeightAt()` and rewrites `world.json`. So the
documented recovery for "assets are missing" is itself a **regeneration**, and a
regenerated scene can differ from the committed one.

What survives a regen is bounded: `writeManifest` preserves foreign manifest entries
whose `.vxw` still exists on disk, so the runtime-authored `hamlet_*` layers are
kept rather than dropped. What does **not** survive is fidelity to the committed
`heightmap.png` / `landscape.vxw` bytes. If those were hand-edited, or the baker
has drifted since they were committed, the regen is a silent content change that
still leaves a working, plausible, *different* scene.

**Therefore: restore with `git checkout -- assets/` first, and only reach for
`ninja -C build world` when the files are genuinely absent from git** (fresh
checkout, or a deliberate re-bake you actually want).

## Rules that follow

- **Never `ninja -t clean` (or `ninja clean`) in this repo.** It is not
  recoverable by re-running the build; it is only recoverable from git. If a full
  rebuild is needed, delete `build/` instead — that is a build artefact directory
  and owns nothing in `assets/`.
- **Do not treat `assets/` as read-only.** It is not enforced anywhere; it is a
  request. See [[concepts/measurement-discipline]] for the other way tests mutate
  it (`world_all.json` is rewritten *inside* `assets/` on every `test-world` /
  `test-unit` / `test-surfel` run).
- **When assets vanish, identify the mechanism before restoring.** `git checkout`
  fixes the symptom; only knowing it was a clean tells you it will happen again on
  the next clean by any session.
- **If `assets/` is ever re-plumbed**, the fix is to stop registering versioned
  content as build outputs — bake into `build/` and copy, or mark them
  `BYPRODUCTS`/non-output. Until then, the hazard is structural, not procedural.

Cross-refs: [[entities/hamlet-scene]] (what the authored content is),
[[concepts/voxel-object-authoring]] (how objects get authored),
[[concepts/measurement-discipline]] (shared mutable state as an instrument
hazard), [[concepts/focused-test-groups]] (the test groups that write into
`assets/`).
