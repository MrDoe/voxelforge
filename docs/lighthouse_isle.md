# LighthouseIsle

A sea island scenery — the second shipped world next to LakesideHamlet.
View it: `./build/voxelforge --scenery LighthouseIsle` (or pick it in the
World panel; it writes `scenery/last.json` like any other scenery).

## The landform

A tropical sea island (radii ~72 × 56 m) on the fixed world water plane
(`WATER_LEVEL = -0.9`, so open sea = anything the heightfield drops below
it — the sea needs no flood machinery at all). Baked by
`scenery/LighthouseIsle/bin/isle_gen` (source `tools/isle_gen.cpp`,
standalone compile in `bake.sh`, no CMake wiring on purpose):

- south: the **beach** — a wide gentle ramp to a shallow sand shelf (kept
  under 0.55 m of water near shore so the bed stays light sand: turquoise
  over the shelf, dark rock beyond the drop-off);
- the dune line behind the beach is grass at SHADING level only (terrain
  mats 0/1 → `applyFlora` blades; voxel small plants are banned — the
  photo-card pass is the geometric successor);
- west meadow: the **cottage pad** (flat, 2.2 m) with a gravel footpath to
  the beach; barn to the NE and a tower windmill on the SE meadow;
- a **volcanic cone** at the N (heightfield, rocky flanks, small crater);
- **caves**: the v2.0 cave mesa (a ring-built ellipsoidal rock shell with a
  walk-in sea cave + skylight) was REMOVED on the user's order 2026-11-19
  ("what is this ugly dome? remove it!") — a smooth ellipsoid on a flat
  plate reads as a lentil no matter how weathered the boulder skirt is. The
  walk-in-cave CONTRACT it proved (cave air must sit above the column top
  AND stay connected to the outside, or the field flood-fills it solid) is
  kept in `.opencode/wiki/concepts/lighthouse-isle.md` for a future
  cliff-hewn cave; the dry "waterfall" chute went with it (falling water
  has no representation on the fixed plane anyway). Refinement the same
  day: **caves and stones ARE allowed inside the terrain file** (natural
  features) — the ban is only on building/prop supports (pads, carved
  dishes, structures-as-landscape). Codified in the `scenery-generation`
  skill.
- a **cenote** (blue hole, −1.6 m dish the plane fills) and a rock-rimmed
  **tidal pool** on the beach flat;
- the east **headland**: a steep rocky knoll flattened at the top for the
  **lighthouse** — banded tower with an OPEN archway (doors cannot be
  articulated yet, so every door is modeled open), a spiral stair winding
  the hollow core, a gallery, and a mat-10 lantern; its glow is real light
  via an authored manifest light (the derived budget is seawater noise)
- a **dense jungle** (~44 trees: banyan/beech/oak/alder/birch + pines on
  the cone, split across `isle_forest` + `isle_jungle` because one
  `write_object` call caps at 200k voxels) and ~25 **coconut palms**
  (ringed S-curve trunks, arching fronds, coconuts) — solid-canopy voxel
  geometry;
- **walk-in interiors**: the house, barn and windmill import in `--shell`
  mode (hollow) and carry furniture in `isle_interiors` (rug, hearth with
  an ember fire, table + stools, bed, chest, shelf, hanging lamps; hay,
  barrels and crates in the barn; flour sacks, wallot gear and a ladder in
  the windmill). Doors cannot be articulated yet, so **every door is
  authored OPEN** (user directive 2026-11-19): `tools/isle_opendoor`
  deletes the door-panel cells through the (rotated!) wall — the opening is
  what keeps the hollow interior from being flood-filled solid, so a closed
  shell would silently render as a solid block. The lighthouse has its own
  open arch + spiral stair; the cave mesa its open arch + skylight.

## What is procedural vs mesh

| piece | how |
|---|---|
| forest (trees), palms, lighthouse, jetty, cave mesa, rocks, driftwood | procedural voxels (`tools/isle_nature.py`, `vf_mcp write_object`) |
| building interiors (furniture) | procedural voxels (`tools/isle_interiors.py`), placed from the imported shell's measured bounds; open doorways cut with `bin/isle_opendoor --box`; two warm lights authored into the cave via `add_light` (manifest `lights`) |
| grass, marram dunes, undergrowth | **never voxel geometry** (2026-11-19 user directive): the terrain material bands (0/1) drive the shading-level `applyFlora` micro-layer today, and the photo-card pass (render bit 13, in development) owns ground cover next. The first authored version shipped voxel tufts/reeds/undergrowth — removed. |
| house, barn, windmill, sailing ship, rowboat | **CC0 Quaternius meshes** through the repo's own converter `vf_mesh2vox` (`--shell` for anything with an interior: 1-voxel skin, hollow; OBJ MTL `Kd` → nearest palette material — the mapping depends on the palette the importer was built with, so `isle_meshes.sh` ends with a `--remap 10,16` safety net against red roofs nearest-matching emissive ember on older palettes; `--stats` shows the material histogram). Sources: `assets/models/isle_*.{obj,mtl}`, provenance in `scenery/LighthouseIsle/meshes/LICENSE.txt`. Staging: `tools/fetch_isle_meshes.py` (gdown against the public Drive folders), import: `isle_meshes.sh`. `vf_mesh2vox` writes NO `meshSource` manifest tag, so the OBJ pair + this table ARE the source of truth — re-run `isle_meshes.sh` after any re-bake to regenerate the layers deterministically. |

## Photoreal features used

Terrain bands + per-material photo albedo (the isle wears its own beach
sand and a wave-worn coastal cliff via its own manifest `textures` table —
scenery-local overrides, resolved before the shared library); the
`applyFlora` two-scale grass/leaf micro-layer; wet-shoreline soil (render
flag bit 12); backlit foliage transmission (bit 11); emissive lantern →
auto point light; auto-exposure (`VF_AUTOEXPOSE=1`) and linear-HDR FX
(`VF_LINEAR_FX=1`). A/B renders exist in `tests/screenshots/` (search
`isle_*`). Hero flags: `VF_RENDER_FLAGS=2559 VF_AUTOEXPOSE=1 VF_LINEAR_FX=1`.
Eight bound textures were upgraded to high-detail ambientCG 2K photos on
2026-11-19 via `tools/upgrade_textures.py` + `tools/prepare_texture.py`
repair, all passing `tools/check_texture.py` IN PLACE: Bark015 →
`bark_brown.png`, Plaster003 → `stone_plaster.png`, Gravel027 →
`gravel.png`, Ground019 → `proc_moss.png`, Rock058 → `rock.jpeg`, Rock063 →
`rock_grey.png`, Wood030 → `wood_highres.png`, Planks037A →
`wooden_planks_dark.jpeg` (mat 7 roofs read as weathered wood shingles —
the clay-tile candidate RoofingTiles012B failed the seam gate twice and is
out). Isle building records remapped mat 22→7 so every roof samples a real
roofing material (mat 22 wet soil was an accidental nearest-Kd landing).
Every promotion: 2K colour map → LANCZOS 512 → prepare (strip/flatten/
seamless) → mean-match to the slot's palette identity → gate PASS; a
passing pre-existing file is an A/B tie, not a free swap (promotion war of
14:13–14:40 documented in `.opencode/wiki/log.md`).

## Re-baking

`./scenery/LighthouseIsle/bake.sh` — recompiles isle_gen (standalone),
re-bakes terrain, re-stages textures (cached), re-authors nature layers and
mesh layers. The baker preserves `ai_edits` and every foreign authored
layer exactly like the hamlet's. Every headless authoring run must keep
`VF_SCENERY=LighthouseIsle` exported and `VF_OVERLAY_PATH` pointed at a
temp file (AGENTS.md overlay rules).
