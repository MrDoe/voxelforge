---
title: mesh-to-voxel converter (STL/OBJ import)
tags: [mesh-import, tooling, voxel-object, mcp]
sourceRefs: [tools/mesh_to_voxel.cpp, src/voxel/mesh_voxel.hpp, src/voxel/mesh_import.hpp, src/ai/mcp_server.cpp, src/app/main.cpp, docs/tooling.md, tests/test_authoring.cpp]
lastReviewed: 2026-09-25
---

Converts a triangle mesh (`.stl` / `.obj`) into a voxelforge object layer
(`assets/<name>.vxw`), so arbitrary authored or scanned geometry can join the
world without being re-modelled as SDFs or stamps. Three entry points share
one pipeline: the offline CLI `vf_mesh2vox` (`tools/mesh_to_voxel.cpp`), the
live MCP tool `import_mesh` on `vf_mcp`, and the in-app **Import STL / OBJ**
workspace in the Dashboard/World Layers GUI. The GUI scans `assets/models/`,
accepts typed paths, and exposes fit/scale, Z-up/winding, material,
solid/shell, and lattice-anchor controls. Replacing an existing layer changes
only its record file; `world.json` `pos`/`rot`/`rotX`/`rotZ` remain the
authored placement/orientation. `VF_TEST_MESH_IMPORT` calls the same
`App::importMeshFromGui()` method headlessly for deterministic verification.

## Pipeline

1. **Parse** (`mesh_import.hpp`): binary+ASCII STL (auto-detected: the 80 B
   header + u32 count + 50 B/tri size check decides, because some binary files
   also start with `solid`), and a Wavefront OBJ subset (`v`/`vt`/`vn`/`f`
   with quads + n-gons fan-triangulated and negative relative indices),
   `mtllib`/`usemtl` with `Kd` diffuse colours.
2. **Transform**: `--swap-yz` (Z-up import) -> `--rot-y` about the AABB centre
   -> rebase so the AABB min is at the origin and the base sits on y=0 ->
   `--scale` (model units per metre; CAD STL is usually mm so `0.001`) or
   `--fit M` (longest side = M m) -> optional `--flip` winding. Units are the
   caller's job: no auto-detection.
3. **Voxelize** (`mesh_voxel.hpp`): per-triangle 13-axis box/voxel-cube
   overlap test marks a 1-voxel-thick **shell**; a 6-neighbour flood fill of
   empty cells from the grid border classifies the **exterior**; everything
   unreached and non-shell is **interior** and emitted as solid. Default
   output is the FULL SOLID VOLUME — see why below.
4. **Place** (`meshToRecords`): records placed by the SOLID cell AABB so the
   bottom-center lands exactly on the anchor (lattice `anchor`/`cell`, or
   `ground x,z` snapping to terrain). Material: `--mat` for STL; MTL `Kd` ->
   nearest `kPalette` entry with the colour kept on the record. Interior cells
   inherit the shell's material via an inward BFS. `convertMeshToRecords()`
   is the common entry point for the GUI, CLI, and MCP; OBJ MTL lookup starts
   beside the OBJ and malformed face indices are rejected rather than read out
   of bounds.

## Why solid fill, not shell

`VoxelField::build` flood-fills each object component to a solid, but only if
the stored shell is **watertight** — and a rasterised 1-voxel shell of e.g. a
cylinder is not (the stored quirk: the trunk comes back as a hollow tube,
probed solid/air/solid). Emitting the full solid volume removes the failure
mode entirely, at the cost of a larger layer (~18k records for a 5 m cabin).
`--shell` / `"shell": true` restores the thin variant for known-watertight
meshes where size matters more.

The tradeoff to know: **conservative voxelization inflates the solid by up to
one voxel per axis** vs the mesh (every cube a triangle touches is marked), so
a 1 m cube yields 11³ = 1331 cells, not 1000. That one-cell overlap is what
keeps the shell watertight.

## Leak detection

If the mesh has a hole larger than one voxel, the exterior flood escapes and
no interior can be classified. `voxelizeMesh` sets `VoxelizedMesh::leak` and
all entry points print a WARNING: only the shell was written, so the object
may render hollow. Fix the mesh or increase `--fit`. A cabin model with an
open doorway legitimately has its room classified as exterior (connected to
outside air) — that is correct, not a leak.

## Verification recipe (what actually caught things)

- `--dry-run` first: triangle count, AABB in metres, shell/interior/solid
  counts, placement cell — no file written.
- `vf_slice --axis z|x` cross-sections: walls as the expected material glyph,
  openings (doors/windows) visible as gaps, room interior as air.
- `--probe` a vertical line through the object: solid at the walls, and check
  the interior is *not* solid/air/solid (the hollow-tube signature).
- A `--shot` render judged via `ascii_view.py`.
- Unit tests (`tests/test_authoring.cpp`, 5 cases): unit cube = 11³ solids
  with an all-solid mid-column probe; `--shell` emits no interior; a box
  missing its top face reports `leak`; `meshToRecords` places the base ON the
  anchor and drops out-of-lattice cells; an OBJ+MTL file resolves beside the
  OBJ and converts end to end.

Placement gotcha: flat ground is scarce — the heightmap has ridges everywhere
and a 0.6 m patch scan is not enough (a snow-mat-16 ridge one metre off made
slices print `?`). Scan a ±2 m patch, or place with `--at X,Y,Z` explicitly.
`vf_slice` only has glyphs for mats 0..8, so mat >= 9 (snow, bark…) prints `?`
even when correct — verify with `--probe`, which prints the numeric mat.

## CMake

`vf_mesh2vox` sits beside `vf_trees` (explicit target, never in the default
build/tests/`world`): run it when you want a converted asset. `vf_mcp` gains
the `import_mesh` tool from the same headers, while `voxelforge` exposes the
GUI workspace in `src/app/main.cpp`. The cabin reimport used the full
`Forrest_Hunting_Cabin.stl`, fit 5 m, material 6, anchor `[428,507,676]`;
its existing manifest pose was `pos=[0,1.65,0]`, `rot=-113`, `rotX=-89.8`,
`rotZ=-63` and stayed byte-for-byte unchanged.
