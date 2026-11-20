---
title: Mesh-to-micro authoring (vf_mesh2micro)
tags: [micro, mesh, authoring, scenery, cc0, raster-owned, tool]
sourceRefs: [tools/mesh_to_micro.cpp, tools/fetch_micro_meshes.py, scenery/LighthouseIsle/world.json]
lastReviewed: 2026-11-20
---

# Mesh-to-micro authoring

The gap this fills: the repo had two authoring paths but neither produced a
*micro* mesh layer from a real downloaded mesh. `vf_microvox` only makes
procedural lathe/alpaca shapes; `vf_mesh2vox` bakes on the world lattice
(0.1 m), which the micro loader refuses (`voxelSize >= VOXEL`). So "add a
detailed mesh to a scenery" had no data-only path.

## The tool

`vf_mesh2micro <file.stl|file.obj> --out NAME [--fit M | --scale S] [--mat N]
[--voxel V] [--up y|z] [--at X,Y,Z | --ground X,Z] [--append]`

- Reads the mesh with the shared `readMeshFile` (STL or OBJ), converts the
  file's up axis with `upAxisToY` (`--up z` for DCC exports), scales/rebases to
  `--fit` metres, and voxelizes on the object's own fine lattice with the
  proven parity fill (`voxelizeMicroMesh`). The normalized side-car is always
  written Y-up, so the manifest's `meshUp` is always `"y"` — even when the
  source file was Z-up (setting `"z"` would rotate the side-car a second time
  in `loadMicroMesh`).
- Writes a micro `.vxw` (world size derived from the grid, `waterLevel`,
  `gridN`, `brickN=8`) and, with `--append`, a `role: micro` entry in the active
  scenery's `world.json` carrying `meshSource`, `meshUp`, and the min-corner
  `pos`.
- The app then draws it **raster-owned** from the mesh (`micro_mesh.vert/frag`)
  with the voxels as the shading/pick/shadow source — see
  [[concepts/geometry-format-policy]].

### Two load-bearing details

1. **The mesh side-car must carry the same scale as the voxels.** `--fit`
   scales the voxel field, but the app's `loadMicroMesh` reads the `meshSource`
   file verbatim. Pointing it at the raw download (e.g. a 2.7-unit torch for a
   0.55 m field) makes ~100% of vertices miss every rest solid cell and draws
   the object ~5x too big. The tool writes a normalized ASCII STL beside the
   `.vxw` (`scenery/<name>/micro/<name>.stl`) and sets
   `meshSource: "micro/<name>.stl"` — `scenery::resolveShared` tries the
   scenery dir before the shared library. The original stays in
   `assets/models/` as tracked source; the side-car is a derived cache
   (gitignored).
2. **Open / multi-component meshes leave holes.** Parity fill only closes a
   surface that encloses volume; a compass (10 components) or radio (15) is
   open, so surface cells are missing and per-vertex appearance falls back
   (flat-grey patches). The tool seeds a solid cell at every mesh vertex's
   lattice cell after voxelization, guaranteeing `nearestRestSolid` hits within
   0 cells. The silhouette still comes from the raster mesh; only the shading
   source is thickened.

## Asset acquisition

`tools/fetch_micro_meshes.py` stages a curated set of CC0 Quaternius Survival
pack props (torch/lamp, compass, radio, backpack, pot, gas can, log) into
`assets/models/micro_*` with rewritten `mtllib` and a LICENSE note. Network is
via `gdown` (the pack's public Drive folder); `websearch` returns 402 in this
environment. Quaternius OBJs are Y-up (`--up y` default); solid-colour `Kd`
MTLs import cleanly (map_Kd-only packs would import monochrome).

## Verification

`--selftest` log must show, per layer,
`micro: 'micro/X.vxw' per-vertex appearance on N vertices (0 fell back)` and
`raster-owned from .../micro/X.stl`. Any non-zero fallback count means the
scale/side-car contract above broke. A close-up `--shot` at the layer's `pos`
confirms the prop is visible and not flat grey.
