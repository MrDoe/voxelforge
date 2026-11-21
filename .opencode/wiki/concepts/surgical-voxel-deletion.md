---
title: Surgical voxel deletion in vf_mcp
tags: [mcp, vf_mcp, deletion, authoring, lighthouse-isle]
sourceRefs: [src/ai/mcp_server.cpp, src/ai/tools.hpp, docs/ai-editing.md, tests/test_chat_tools.cpp]
lastReviewed: 2026-11-20
---

# Surgical voxel deletion in vf_mcp

A baked object layer holds many separate objects (the LighthouseIsle
`isle_forest` layer has ~44 trees in ~150k cells). Until 2026-11-20 the MCP
surface could only delete a **whole layer** (`delete_object` / `delete_layer`)
or wipe `ai_edits.vxw` (`clear_edits`). A gulf opened between that and the
in-app Delete brush ([[entities/live-edit-brush]]), which edits the runtime
store and is reversible through `runtime_edits.vxw`.

`delete_voxels` closes the gulf: it removes **cells from one layer**, leaving
the rest of the layer and its manifest entry/placement alone.

## Selectors

Selectors are unioned when several are supplied; all operate on the layer's
own absolute world-lattice coordinates:

- `box:[x0,y0,z0,x1,y1,z1]` — inclusive lattice cells; `min` + `max` is an
  equivalent spelling. This is the reliable choice when canopies overlap.
- `anchor:[x,y,z]` + `radius:<metres>` — a sphere around the picked cell.
- `anchor:[x,y,z]` + `component:true` — a flood fill of the picked cell's
  6-connected component (default; `connectivity:26` for diagonally connected
  foliage). This is the "delete exactly one tree" case.
- `mat` / `from` / `material` — optional material filter.
- `dry_run:true` — report the count and bounds, write nothing.

## Safety contract

- Refuses `landscape`, `packed`, and `ai_edits` (the app owns the last one).
- Refuses a selection covering every cell — that is what `delete_object` is
  for.
- Writes `<layer>.vxw.tmp` then `std::filesystem::rename`, never in place (a
  crash cannot leave a truncated tracked layer). VXW v2 sections from the read
  are preserved.
- Calls `reloadWorld()` after the rename: `worldSignature()` watches only
  `world.json` and `ai_edits.vxw`, so without an explicit reload the server's
  cached `LayeredWorld` would keep answering `probe`/`ground` from the old
  content.
- **There is no Undo.** The reply tells the caller to back up the `.vxw`. The
  in-app Delete brush remains the reversible alternative.

## The normalization trap

`canonicalToolName()` in `src/ai/tools.hpp` used to map *any* name containing
`delete` / `remove` / `erase` to `delete_object`. A small model saying "remove
voxels" therefore wiped the entire layer — a dangerous failure mode, which is
why the in-app assistant originally refused to attempt a single-tree deletion.
The rule now splits by target:

```
delete/remove/erase + voxel/cell/region/area  -> delete_voxels
plain delete/remove/erase                      -> delete_object
```

Pinned by `tests/test_chat_tools.cpp` ("deletion aliases split surgical carve
from whole-layer delete"). When adding a native MCP tool, register it in both
`kNative` (verbatim path) **and** the fuzzy normalizer so an alias cannot land
on a destructive neighbour.

## Verification

Scratch-scenery integration test (real `vf_mcp`, default scenery untouched):
a 54-cell two-component layer; `dry_run` reported 27 cells and wrote nothing;
`component:true` removed exactly the 27-cell component and left the other
intact; a whole-layer box errored; a radius-0.1 m preview selected 4 of 27
cells; `remove_cells` (fuzzy alias) routed to `delete_voxels`. `tools/list`
parses as JSON with 45 tools. See also [[concepts/lighthouse-isle]].
