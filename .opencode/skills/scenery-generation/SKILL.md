---
name: scenery-generation
description: Build and maintain a voxelforge SCENERY (a self-contained world folder under scenery/<name>/) — terrain baker, layer authoring, mesh hero props, textures, lights, and the verification pipeline. Use when creating a new scenery, re-baking or extending an existing one (LighthouseIsle, LakesideHamlet), or when deciding what may live in the landscape file vs separate layers. Covers the user-mandated content rules of 2026-11-19.
---

# Scenery generation

A scenery is a folder `scenery/<name>/` with `world.json` (manifest),
`heightmap.png` + `landscape.vxw` (terrain) and one `.vxw` per authored
layer. See `.opencode/wiki/concepts/sceneries.md` for the runtime side and
`concepts/lighthouse-isle.md` for a full worked example (the pipeline this
skill distills).

## The content rules (user directives — do not re-litigate)

1. **Landscape = heightmap-generated terrain, then edited for detail.**
   Slopes, ridges, cones, beaches, dune bands, material bands are terrain
   detail and belong in `landscape.vxw` / `heightmap.png`.
2. **Caves and stones ARE allowed as part of the terrain file** — they are
   natural features (user clarification 2026-11-19). A cliff-hewn cave or
   boulder fields may be baked into `landscape.vxw` directly.
3. **Buildings, props, vegetation are separate layer files.** Nothing that
   reads as a *thing* goes into landscape: no flattened pads, no carved
   dishes supporting a structure, no house that lives inside the terrain
   shell. Every building brings its **own footing** in its own layer (stone
   base down past the lowest ground corner) — never lean on a baked pad.
   Water features that are discrete *things* (pool, cenote) are borderline:
   a cave/hollow carved into rock = allowed natural terrain; a manicured
   swimming-plate dish = a thing — prefer layers or skip.
4. **No voxel grass/small plants.** Ground cover is shading: terrain mats
   0/1 trigger `applyFlora`; the photo-card pass is the geometric successor.
   Trees (large) and logs (props) stay as geometry.
5. **Doors are authored OPEN** (no articulation exists): cut the doorway
   cells. A hollow `--shell` import MUST have a real opening or the field
   flood-fills its interior solid — the open door is structural, not
   cosmetic. Same contract for walk-in caves: interior air must connect to
   outside air, and cave air must sit ABOVE the heightfield column top
   (terrain promotion re-solidifies below).
6. **Smooth ellipsoid domes on flat plates are banned by taste** ("what is
   this ugly dome? remove it!" 2026-11-19): natural masses must be
   irregular; a cave belongs in a cliff, not parked on the ground.

## Emissive discipline (burns the light budget silently)

- Cells with **material id 9..15 glow** (kEmissive), regardless of any
  texture bound to the slot. A "gravel" cell authored as mat 15 glows
  gravel-white. Gravel look = **mat 5 + tex-override 15**.
- Intended emitters ride **mat 10** (ember; keep slot 10 unbound so the
  record's own RGB shows). Never mat 15 if that slot carries a photo.
- `landscape.vxw` must hold **zero** cells with mats 9-15 (audit:
  `scenery/<n>/bin/isle_opendoor layer.vxw --stats` — a materialId
  histogram; also supports `--box` door cuts and `--remap from,to`).
- **Open water is emissive**: water volume is marked mat 9
  (`layered_world.cpp`), so a sea scenery floods the 192-derived-cluster
  budget (LighthouseIsle: ~1996 clusters, warning every load). Mitigation:
  put every lamp that must work into the manifest `lights` key —
  **authored lights fill the UBO before derived ones** (max budget 192;
  `vf_mcp add_light` DOUBLE-APPENDS, so author the key by rewriting it,
  deduped — see `tools/isle_interiors.py`).
- Mesh imports map solid MTL `Kd` to the NEAREST palette material — the
  mapping MOVES when the palette grows (red roofs: mat 10 ember at
  kPaletteN=22, mat 22 wet soil at 23). Check `--stats` after every import
  before remapping; keep a `--remap 10,16` safety net in the pipeline.

## Pipeline (LighthouseIsle's layout — replicate per scenery)

Order matters: nature reads the baked heightmap, buildings read it too,
interiors read the buildings' measured bounds.

1. **Terrain baker**, standalone, NOT wired into the default ninja build
   (author-time tool; the scenery-migrator owns CMakeLists): compile the
   `tools/<name>_gen.cpp` (reuses `voxel/common.hpp` + `heightmap` +
   `worldfile` + `scenery`) with a direct g++ line into
   `scenery/<name>/bin/`, run it: writes heightmap.png + landscape.vxw +
   world.json **preserving `ai_edits` and foreign layers**. Keep the
   terrain formulas deterministic so a re-bake is reproducible.
2. **Textures**: scenery-local photo overrides resolve before the shared
   library (`scenery::resolveShared`). Repo-conformant drop-in = fetch →
   LANCZOS 512 → **mean-match to the slot's palette identity**
   (`tools/fetch_textures.py` / `tools/upgrade_textures.py`; ambientCG,
   existence probe via `api/v2/full_json?id=X`, key `foundAssets`).
3. **Nature layers** via `vf_mcp write_object` (`tools/isle_nature.py`
   pattern: numpy heightmap sampler + JSON-RPC one-shot to `build/vf_mcp`).
   **write_object caps at 200 000 voxels and truncates SILENTLY** — the
   signature is a layer of EXACTLY 200000 records. Split big forests
   greedily across two layers.
4. **Mesh hero props** via `build/vf_mesh2vox --shell` for anything hollow
   (`--ground x,z` places the bottom on natural terrain; the shell keeps a
   1-voxel skin).
5. **Interiors + doorways** (`tools/isle_interiors.py` pattern): measure
   the placed bounds with `inspect_layer` (bounds come from the WRITTEN
   file — `--rot-y` turns walls off the AABB edge, so door boxes must reach
   ~1.8 m into the footprint), cut doors, author footings + furniture,
   rewrite the manifest `lights` key (parametric from bounds).
6. **Remap safety net** pass (ember roofs).
7. **Shots**: one `--shotlist` load (lines `PATH X Y Z TX TY TZ`), dusk
   pass with `--sun <elev> <azim>` + `VF_VOLFOG=1`, SVO parity shot; PNGs
   to `tests/screenshots/`. Verify footings/doors with `--probe` (CPU).

## Hard rules from the field

- **Export `VF_SCENERY=<name>` on EVERY vf_mcp/vf_mesh2vox/app invocation**
  (and `VF_OVERLAY_PATH=/tmp/opencode/….vxw` + `VF_NO_OVERLAY=1` for
  headless): without it a fresh checkout falls through to
  **LakesideHamlet and writes into the shipped scene** (cross-tenant hazard,
  peer-documented 2026-11-19). Verify the first write's echoed path + file
  mtimes. The World panel's SCENERY switcher (Phase 2) writes the
  `scenery/last.json` pointer on every in-session switch, so an interactively
  used machine self-protects after one switch — but TOOLING runs in fresh
  shells never write it and must always carry the export.
- **Probe terrain before placing anything**: decode heightmap.png with
  numpy (`-8 + 32·v/65535`, bilinear) and query the spot — this caught a
  ship on the beach, a buried jetty and an empty reed ring.
- Placement truth flows one way: baker → heightmap → layer scripts sample
  it. Never hand-edit derived `.vxw`/height textures.
- The app REWRITES `world.json` from its layer list on every run; preserve
  foreign entries, and a test/session must not delete keys it cannot write
  back. Never run `ninja clean` in a tree with a baked scenery registered
  as build output.
- Screenshots are the gate, but METRICS beat captions: check the lighting
  log lines (`--stats`, `lighting: N authored / M derived`) and probes; a
  vision model cannot see a 0.1 % artifact or a sealed-interior bug.
- Commit EXACT paths on a shared tree (never a directory), and never
  rebuild `build/` while a peer's gates or interactive window are running.

## Verification checklist before showing a scenery to a human

- [ ] bake log: water coverage sane; `--stats` shows NO mats 9-15 in
      landscape; no layer of exactly 200000 records
- [ ] doors cut (`opendoor: removed N` with N > 0 on the first pass — 0 on
      a re-run is fine, 0 on a FRESH import means the box missed the wall)
- [ ] `lighting: N authored` covers every lamp you care about
- [ ] hero + dusk + interior + parity shots in `tests/screenshots/`
- [ ] `git status` shows only your scenery paths
