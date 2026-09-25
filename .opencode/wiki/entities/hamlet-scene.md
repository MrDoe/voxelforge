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
| `hamlet_hall` | timber great hall 11×6 m, stone plinth, stepped shingle roof, door recess, shuttered windows, chimney, porch, weathervane |
| `hamlet_cabin` | full `Forrest_Hunting_Cabin.stl` voxel import, fit to 5 m, kept at the authored `pos/rot/rotX/rotZ` placement |
| `hamlet_tower` | round stone watchtower, battlements, window slits, conical roof, green pennant |
| `hamlet_pier` | jetty deck on piles, railings, crates, barrels, glowing lantern (emissive mat 9) |
| `hamlet_boat` | moored boat with hull, mast, sail, rudder, oar |
| `hamlet_well` | stone well, roof, bucket, crank |
| `hamlet_market` | three market stalls with coloured awnings, counters, produce, campfire |
| `hamlet_garden` | fenced beds with crop rows, scarecrow |
| `hamlet_pines` | four detailed conifers on soil mounds (slope-tolerant bases) |
| `hamlet_props` | cart with wheels, signpost, benches, barrels, lantern posts |

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
