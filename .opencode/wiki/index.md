# Voxelforge Wiki

## Pages

### Entities
- [[entities/svo-render]] — SVO & dense raymarch backends, parity contract, brick data

### Concepts
- [[concepts/shading-model]] — the PBR lighting model (GGX, SDF AO, sky irradiance, aerial fog, grading)
- [[concepts/voxel-object-authoring]] — layer-by-layer SDF/stamp authoring loop: vf_slice + probe checks, screenshot gate, tool landscape

## How to navigate
- Start with [[entities/svo-render]] for the render pipeline, then [[concepts/shading-model]] for the lighting.
- Shading changes MUST be mirrored in both `shaders/*_raymarch.comp`; `--compare` enforces parity (meanDiff < 14/255).
- To add or edit world objects, follow [[concepts/voxel-object-authoring]] and the `voxel-object` skill.
