---
title: LighthouseIsle scenery (tropical v2.1: mountain, interiors; cave dome removed)
tags: [scenery, island, baker, mesh-import, photoreal, cc0, ambientCG, quaternius, interiors, caves]
sourceRefs:
  - tools/isle_gen.cpp
  - tools/isle_nature.py
  - tools/isle_interiors.py
  - tools/isle_opendoor.cpp
  - tools/fetch_isle_meshes.py
  - tools/isle_textures.py
  - scenery/LighthouseIsle/
  - docs/lighthouse_isle.md
lastReviewed: 2026-11-19
---

# LighthouseIsle scenery

The second shipped world after [[concepts/sceneries]] LakesideHamlet: an
island in the open sea with a beach, forest, a house, a moored boat and a
working lighthouse. Full author guide in `docs/lighthouse_isle.md`; this page
captures the decisions worth recalling.

## The sea is free — an island is just a heightfield

Water is the one fixed-level plane at `WATER_LEVEL = -0.9`
([[concepts/water-plane]]), rendered world-wide. So an "island in the sea"
needs no flood or shoreline machinery: the baker writes a heightfield that
dips below −0.9 everywhere around the island, and the global plane *is* the
sea. A shallow sand shelf just offshore keeps the near-water bed light
(`materialFromBands` returns sand while `wd < 0.55`), so the water reads
turquoise over the shelf and dark over the drop-off.

## Baker: standalone, not CMake-wired

`tools/isle_gen.cpp` mirrors `heightmap_gen` (same hand-rolled 16-bit stored
-deflate PNG writer, same landscape-shell + manifest-preserve pipeline) but is
compiled by `scenery/LighthouseIsle/bake.sh` with a direct `g++`, and the
binary lives at `scenery/LighthouseIsle/bin/`. **Why not a ninja target:** the
scenery-migrator session held `CMakeLists.txt` + `build/` during its gate
runs, and an author-time baker only needs to run when the scenery is edited —
so no root-build wiring (mirrors the `world` target's "explicit tool, never
default-build" rule without touching the shared CMake).

One landform gotcha: the micro-relief mask (`landMask`) is 0 in the island
interior by design, so gating the **lagoon carve** on it carved nothing — the
lagoon sits mid-island (coast distance ~0.48). The lagoon gets its own
interior mask (`smoothstep(1.08, 0.92, d)`), not the relief mask. (v2 replaced
the lagoon with a **cenote** + beach-flat **tidal pool**, each with its own
interior carve mask for the same reason.)

## v2 landform: mountain, dome-removal, and the cave contract

The tropical expansion (mountain + caves + waterfalls + dense wood) is
composed so every piece respects an engine invariant:

- the **mountain** is a heightfield volcanic cone (N, ~11.8 m) — slopes are
  slope-classed rocky; a heightfield cannot overhang, so it can never host a
  cave;
- the v2.0 **cave mesa** — a ring-built rock shell dome (1 m skin, open door
  arch + skylight + light window, boulder skirt, dry seasonal chute down the
  SE face) — was REMOVED 2026-11-19 on the user's verdict "what is this
  ugly dome? remove it!". The landform lesson: a smooth ellipsoidal shell
  on a flattened footplate reads as a LENTIL from every camera, no matter
  how weathered the collar; a future cave should be cut into a CLIFF (an
  irregular voxel mass with a wide low mouth), not parked on flat ground —
  and per the user's refinement the SAME DAY, caves and stones ARE allowed
  to be part of the terrain file itself (they are natural features; the
  ban is on baked building supports and prop-shaped dishes). Codified in
  the `scenery-generation` skill.
  The contract it validated stays load-bearing: (1) cave AIR cells must sit
  ABOVE the heightfield column top, because terrain promotion re-solidifies
  everything below it; (2) the interior must stay CONNECTED to outside air
  (open door + skylight), because `VoxelField` flood-fills an enclosed air
  component and promotes it to solid — a sealed cave is a solid block;
- **dense wood** = ~44 trees split across `isle_forest`/`isle_jungle` (one
  `write_object` caps at 200 000 voxels; a truncated layer reports EXACTLY
  200000 — the round count is the signature), plus a palm layer.

## Interiors with open doors (user directive 2026-11-19)

Doors cannot be articulated, so **every door is authored open**. Mesh
buildings import in `vf_mesh2vox --shell` (1-voxel skin, hollow — same
flood-fill logic as the cave: hollow only survives if the doorway is real
air), then `tools/isle_opendoor` (standalone, reuses `worldfile` so CRC and
sections stay valid) deletes records inside world-space door boxes; the
boxes must reach ~1.8 m INTO the bounds because `--rot-y` turns the wall off
the AABB edge (a 0.8 m window removed 0 of 33160 records — silent seal).
`tools/isle_interiors.py` measures each shell's bounds via `inspect_layer`
and authors furniture relative to them (house hearth fires mat 10 ember → one
derived light; the lamps elsewhere ride mat 10 too — mat 15 white-hot is an
emissive slot with the gravel photo bound to it, so a mat-15 cell samples
gravel AND glows; cave light is authored, not emissive voxels; the gravel
footpath is terrain mat 5 + tex override 15). Authored lights are not a
style choice: water volume is marked with mat 9 (lava — EMISSIVE), so an
open-sea world floods the derived-light budget (measured 1996 clusters /
192 slots) and every lamp that must work belongs in the manifest `lights`
key (authored fill the UBO before derived; `isle_interiors.py` owns the
authoritative list; vf_mcp `add_light` double-appends — dedupe on write).
The tool also does
`--remap from,to` and `--stats` (a
materialId histogram): the packs' red roofs once nearest-mapped onto
emissive mat 10 and flooded the derived-light budget (2104 clusters), but
that mapping MOVES with the palette — with mat 22 "wet soil" present the
same Kd now lands there (non-emissive), so check `--stats` before remapping;
`isle_meshes.sh` keeps `--remap 10,16` as a safety net.

## Hero props are meshes, the rest is procedural voxels

House / barn / windmill / sailing ship / rowboat are **CC0 Quaternius OBJs**
imported through the repo's own `vf_mesh2vox` (solid voxelization). They carry
real colour because their MTLs use **per-face solid `Kd`** — `readOBJ` maps
each `Kd` to the nearest `kPalette` material ([[entities/mesh-to-voxel]]).
Textured packs (map_Kd) would import **monochrome**, so only Kd-coloured
packs are usable directly; that constraint drove the pack choice and is the
reason the textured "Simple Buildings" set was rejected.

All vegetation geometry is procedural voxel layers (`tools/isle_nature.py`)
authored through `vf_mcp write_object` — but **only LARGE or woody things**:
the forest (canopies are SOLID blobs so the surfelizer's thin rule cannot fire,
[[concepts/surfel-thin-rule]]), the lighthouse, jetty, rocks and driftwood logs.
Per the user's 2026-11-19 directive (relayed by the photo-card session),
**grass and small plants are never voxel geometry** — the first authored version
had `isle_nature`/`isle_dunes`/`isle_reeds` voxel tufts and those layers were
deleted. Ground cover is the shading-level flora (terrain mats 0/1 → `applyFlora`,
see [[concepts/grass-and-foliage]]) and, next, the CC0 photo-card pass (render
bit 13, default off, keys on the same mats ≤1/8 so the isle's meadow/dune/sand
bands keep their flora trigger). The one gap: the lagoon rim is a damp-soil mat
that neither shading flora nor the cards cover, so reeds there await a reed card.

## Sourcing CC0 without a browser

`websearch` is returning HTTP 402 in this environment; assets were fetched by
`curl`/`gdown` instead. Quaternius publishes each free pack as a **public
Google Drive folder** linked from `quaternius.com/packs/<name>.html` (a
`window.open` target, extractable with grep), mirrored headlessly with `gdown
--folder` inside a throwaway venv (system pip is PEP-668 externally-managed).
Quaternius OBJs are Y-up — no `--swap-yz`. Textures are ambientCG via the
repo's own `fetch_textures.py` pattern (mean-matched to the palette identity).

## Photoreal stack

The isle is the testbed for the photoreal flags: scenery-local photo albedo
via its own manifest `textures` table (resolved by `scenery::resolveShared`
before the shared library), wet-shoreline soil (render bit 12), backlit
foliage transmission (bit 11), the emissive lighthouse lantern auto-deriving a
point light ([[concepts/enclosed-space-lighting]]), auto-exposure and
linear-HDR FX. Hero flags: `VF_RENDER_FLAGS=2559 VF_AUTOEXPOSE=1 VF_LINEAR_FX=1`.
