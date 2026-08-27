# Voxelforge Wiki

## Pages

### Entities
- [[entities/svo-render]] — the single chunked-SVO render path, brick data contract

### Concepts
- [[concepts/shading-model]] — the PBR lighting model (GGX, SDF AO, sky irradiance, aerial fog, grading)
- [[concepts/voxel-object-authoring]] — layer-by-layer SDF/stamp authoring loop: vf_slice + probe checks, screenshot gate, tool landscape

## How to navigate
- Start with [[entities/svo-render]] for the render pipeline (single path — there is no second backend), then [[concepts/shading-model]] for the lighting.
- To add or edit world objects, follow [[concepts/voxel-object-authoring]] and the `voxel-object` skill.
- Full developer documentation: `docs/` (start at `docs/index.md`); engineering conventions for agents: `AGENTS.md`.
