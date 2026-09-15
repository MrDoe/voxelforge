# Voxelforge Wiki

> **Resync status (2026-09-15):** the pages below marked *(pre-splat-rework)*
> describe the world before Gaussian surfels became the primary backend and
> before the live-edit/ChunkStore work landed. Treat `AGENTS.md` + `docs/`
> as the source of truth until they are rewritten; the code facts they state
> (`scene()`, `world.vxw`, "single render path") no longer exist.

## Pages

### Entities
- [[entities/svo-render]] — chunked-SVO raymarch + the Gaussian-surfel backend contract *(pre-splat-rework wording)*
- [[entities/live-edit-brush]] — the Carve/Add/Delete/Paint brush: store stamps, GPU patching, hover tint, overlay persistence

### Concepts
- [[concepts/shading-model]] — the PBR lighting model (GGX, SDF AO, sky irradiance, aerial fog, grading) *(pre-splat-rework)*
- [[concepts/voxel-object-authoring]] — layer-by-layer SDF/stamp authoring loop: vf_slice + probe checks, screenshot gate, tool landscape *(pre-splat-rework wording; the loop itself still applies)*

## How to navigate
- Rendering: [[entities/svo-render]] (reference backend) + [[concepts/shading-model]]; the primary backend is the Gaussian-surfel rasterizer (`--mode splat`, default) — see `docs/rendering.md`.
- Live editing: [[entities/live-edit-brush]].
- To add or edit world objects, follow [[concepts/voxel-object-authoring]] and the `voxel-object` skill.
- Full developer documentation: `docs/` (start at `docs/index.md`); engineering conventions for agents: `AGENTS.md`.
