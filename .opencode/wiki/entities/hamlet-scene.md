---
title: Lakeside hamlet (default world)
tags: [world, authoring, layers, vf-mcp, scenery]
sourceRefs: [assets/world.json, assets/models/Forrest_Hunting_Cabin.stl, src/ai/mcp_server.cpp, src/app/main.cpp, tools/heightmap_gen.cpp, tests/test_world.cpp]
lastReviewed: 2026-09-25
---

# Lakeside hamlet

The default world since 2026-09-16: a runtime-authored village on the north
shore of the valley lake, built with the `vf_mcp` object tools and the GUI
mesh importer (no baker sweeps, no recompile). `assets/world.json` lists
`landscape` + the `hamlet_*` layers, including the imported `hamlet_cabin`.
The old baked scenery (old cabin, trees, rocks, alpaca, fence, bridge,
forest, dock, shore, ferns, bushes, winter assets, carve/raise edits) was
deleted.

## Layers

| layer | contents |
|---|---|
| `hamlet_hall` | timber great hall 11×6 m, stone plinth, stepped shingle roof, door recess, shuttered windows, chimney, porch, weathervane (currently disabled) |
| `hamlet_cabin` | **new layer 2026-10-04**, replaces `CabinPart1` (kept on disk, now `enabled:false`): detailed log cabin (see below) |
| `hamlet_tower_v2` | **new layer 2026-10-04**, replaces `hamlet_tower` (kept on disk, now `enabled:false`); **moved into the forest clearing at (15, 14)** via `pos [-4.90,-0.51,-3.90]`: hollow round watchtower with a real interior (see below) |
| `hamlet_pier` | jetty deck on piles, railings, crates, barrels, glowing lantern (emissive mat 9) |
| `hamlet_boat` | moored boat with hull, mast, sail, rudder, oar |
| `hamlet_well` | stone well, roof, bucket, crank |
| `hamlet_market` | three market stalls with coloured awnings, counters, produce, campfire |
| `hamlet_garden` | fenced beds with crop rows, scarecrow |
| `hamlet_trees` | four detailed conifers on soil mounds (slope-tolerant bases) |
| `hamlet_props` | cart with wheels, signpost, benches, barrels, lantern posts |
| `hamlet_nature` | **new 2026-10-04**: bushes, ferns, flower patches, mushrooms, mossy boulders, fallen logs, stumps, grass tufts, a pulled-up canoe, a mossy standing-stone circle (west meadow), forest-floor undergrowth + dark moss/leaf-litter patches, a **gravel path** (9.3k cells, `tex=15` → `textures/gravel.png`) that leads **from the cabin door to the tower door**, loops the tower clearing, tails north and branches west to the stones, and a **cobblestone street** (4.9k cells, `tex=14` → `textures/light_rock.png`) running along the river shore x −11…23 |
| `hamlet_forest` | **new 2026-10-04**: **38 deciduous trees** — oak/birch/maple + one dead snag — solid trunks, branches and stacked canopy blobs; a dense ring around the tower clearing at (15, 14) plus belts on the west, north and south-east margins (terrain-sampled; spots below the waterline, in the keep-out, inside the cabin/garden, in the tower clearing or within 2.6 m of another trunk are skipped) |
| `hamlet_forest2` | **new 2026-10-04**: **17 varied large trees** in a second layer (stays under the 200k write cap): beech (broad tall crown), pine (layered conifer tiers), willow (low drooping crown), poplar (tall columnar), alder, and ancient oaks (thick trunk, 14-blob crown); same placement guards, 2.3 m trunk spacing |

## 2026-10-04 rebuilds (authoring scripts)

Three scripts under `tools/` build the content through `vf_mcp write_object`
(explicit absolute cells). Naming convention: the rebuilds are **new layers**
(`hamlet_cabin`, `hamlet_tower_v2`) and the files they replace are left on
disk but disabled in `world.json` — that keeps the original assets intact and
makes the change visible in the sidebar's World list. `hamlet_cabin` sits
first among the object layers so `tests/visual_check.py`'s ownership subject
(the first enabled object layer) stays the cabin.

- `tools/hamlet_tower_v2.py` — battered plinth, coursed shaft with pilasters
  and string courses, pointed-arch doorway with an iron-bound plank door,
  arrow slits, six arched windows with dark glazing and timber mullions,
  **hollow interior with wooden decks and a continuous stone spiral stair
  around a central newel** (decks open where each flight arrives), corbelled
  wall head, crenellated parapet, a **solid** shingled spire with flared
  eaves and a finial + pennant, emissive lanterns, ivy/moss, interior props.
  ~92k voxels. The cone must stay SOLID: as a 2.6-cell shell it triggered the
  surfelizer's thin-structure rule (~50 % of surface cells) and rendered as a
  sparse field of disks in the splat backend —
  see [[concepts/surfel-thin-rule]].
- `tools/cabin_v2.py` — stone foundation, stacked log walls (2-cell log +
  recessed pale chinking, alternating corner log-ends with end grain),
  board-and-batten door with iron furniture, four glazed windows with open
  shutters and flower boxes, gable roof with exposed rafter tails, coursed
  shingles + moss, ridge cap, bargeboards, stone chimney with a cap, porch +
  bench, woodpile, chopping block, barrels, a mossy stone path, ivy, and an
  interior with a lit hearth (mat 10 ember), table, straw bed and chest.
  ~36k voxels; roof skin 5 cells deep for the same thin-rule reason.
- `tools/hamlet_nature.py` — the two new nature layers (all solid masses:
  thin rule + flood-fill safety). Terrain heights were sampled from
  `assets/heightmap.png` before placing; nothing lands in the live-edit
  keep-out (world x −3…3, z −1…6).

Anchors (game metres): hall (11, 14), tower (20, 18), pier (14, 9), boat at the
water plane x 14 / z 5.6, well (14, 12), market (17, 13), garden (6, 17),
pines (22, 22), props (13, 10). Terrain there is ~0 m (lake at z≈3–7, water
plane -0.9 m).

## Authoring conventions learned here

- `write_object` shape units: `at` = voxel offsets (bottom-centre anchored),
  box `size` in voxels, ellipsoid `radii` and cylinder `radius`/`height` in
  **metres**; `ground:[x,z]` snaps the anchor to the terrain top.
- Shapes are additive solids: door/window openings must be left by composing
  wall segments, not by inset panels (inset detail ends up buried inside the
  solid volume).
- Slope safety for multi-tree layers: give each tree a soil-mound box that
  extends ~1.5 m down, so a single anchor can serve objects across a slope.
- Verify with `vf_slice --axis … --center <metres>` (solid cells print their
  material glyph) and `--probe`; `read_object` dumps every voxel — only use it
  for small layers.

## Regeneration behaviour

`ninja -C build world` re-bakes **only** `landscape.vxw` (+ preserved
`ai_edits.vxw`) and appends any manifest entry whose `.vxw` still exists, so
the hamlet survives a regen. The old baker object sweeps were removed; the
analytic shapes in `src/voxel/common.hpp` remain as test fixtures only.

Cross-refs: [[concepts/detail-pipeline]], [[concepts/voxel-object-authoring]],
[[entities/mesh-to-voxel]].
