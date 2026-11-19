# Sceneries

A **scenery** is a complete, self-contained landscape in one folder under
`scenery/<name>/`. Voxelforge ships one — `scenery/LakesideHamlet/` — and you
can add more by copying that folder (or authoring a fresh `world.json` +
layers) and switching.

## Layout

```
scenery/
  last.json                 # which scenery was used last (the default; gitignored)
  LakesideHamlet/
    world.json              # layer manifest (+ textures table, lights, sun)
    heightmap.png           # terrain source
    landscape.vxw           # terrain shell records
    hamlet_*.vxw            # object/scatter layers
    ai_edits.vxw            # live chat/MCP edit layer
    micro/…                 # high-detail objects + .rig/.skin/.anim side-cars
    runtime_edits.vxw       # live-edit overlay (gitignored — state, not content)
    world_all.json          # generated all-enabled test manifest
```

`assets/` holds only the **shared library**: `textures/` (per-material photos,
bound by the manifest's `textures` table) and `models/` (STL/OBJ import
sources). Every scenery draws on the same library; a texture is material
identity, not scene content.

## Selecting a scenery

Resolution order (first match wins), in `vf::voxel::scenery::root()`:

1. `--scenery NAME` (one launch; does not touch the pointer file)
2. `VF_SCENERY=<name|path>` — an absolute path or a bare folder name
3. `scenery/last.json` — written on the last switch, so the **last-used
   scenery is the default**
4. the shipped default `LakesideHamlet`
5. the first discovered `scenery/*/world.json`

Headless gates pin `VF_SCENERY=LakesideHamlet` so a user's pointer never
redirects the test suite. The C++ test binary pins the same default before it
loads the heightmap.

## Switching

Phase 1 switches by **relaunch**: `./build/voxelforge --scenery MyScene`, or
delete/edit `scenery/last.json`. In-app (World panel) and in-session switching
drive the same `root()` and the full-reload path; that is tracked as Phase 2 in
[`.opencode/wiki/concepts/sceneries.md`](../.opencode/wiki/concepts/sceneries.md).

## What moves with a switch

Because every world path resolves against `root()`, switching changes the
manifest, the terrain (heightmap + landscape), all object layers, the micro
objects, the AI-edits layer **and the live-edit overlay together** — so live
edits never bleed from one scenery into another, and an unaware headless run
cannot rewrite another scenery's session file. Texture bindings are resolved
against the shared library, so the atlas follows the switch intact.
