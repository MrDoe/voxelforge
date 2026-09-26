# Voxelforge Wiki

> **Resync status (2026-09-15):** the pages below marked *(pre-splat-rework)*
> describe the world before Gaussian surfels became the primary backend and
> before the live-edit/ChunkStore work landed. Treat `AGENTS.md` + `docs/`
> as the source of truth until they are rewritten; the code facts they state
> (`scene()`, `world.vxw`, "single render path") no longer exist.

## Pages

### Entities
- [[entities/hamlet-scene]] — the runtime-authored default world (lakeside hamlet: 9 `hamlet_*` layers, authoring conventions, regen behaviour)
- [[entities/svo-render]] — chunked-SVO raymarch + the Gaussian-surfel backend contract *(pre-splat-rework wording)*
- [[entities/live-edit-brush]] — the Carve/Add/Delete/Paint/Smooth brush sized in voxels (1 voxel = per-voxel, exactly one cell), the click-vs-drag stamp gate (stack suppressed by cell identity, jitter by 6 px travel, plus the reverted net-from-press design and its residual), the resolved Undo margin bug, and click-activated, bounds-centered yaw/pitch/roll trackball: store stamps, GPU patching, hover tint/outline, overlay persistence
- [[entities/mesh-to-voxel]] — STL/OBJ -> .vxw converter (`vf_mesh2vox` CLI + `import_mesh` MCP + GUI workspace): solid-fill voxelization, leak detection, orientation-preserving replacement, and why shell-only is a footgun
- [[entities/hud-sidebar]] — the single opaque, flush left editor panel with an icon rail, one content pane, fixed footer, and user-draggable horizontal splitter

### Concepts
- [[concepts/layer-placement]] — exact `.vxw` provenance and selected-layer placement: stable owner IDs, click activation, bounds-centered yaw/pitch/roll rings, canonical-pivot preview across forward/GPU-cull/tile paths
- [[concepts/load-time-field-build]] — why every run/test pays ~17.6 s: the per-component padded-bbox EDT (72k components, 361M bbox cells ≈ 680× inflation), measured phase table + the batching fix to do next
- [[concepts/water-plane]] — the one fixed-level water plane: world-wide surfel coverage + depth-test clipping in the splat backend, the height-texture sync that keeps foam/absorption/reflections following edits (and the two bugs that made live digs look dry)
- [[concepts/smooth-terrain-brush]] — the Smooth edit mode: snapshot-based surface-position relaxation (terrain columns and object surfaces along their own axis), falloff/strength, ownership rules, exact-band GPU refresh, and live persistence
- [[concepts/detail-pipeline]] — the four detail layers (10 cm lattice, base disks, hash-driven micros, LOD/shading) incl. the per-chunk `[base | edge bridges | material micros]` layout contract (`chunkRange <= edgeStart <= microStart <= chunkRange[i+1]`) and the live-edit micro path
- [[concepts/edge-aware-surfel-radius]] — hard-edge-only parent tightening plus small always-on crease bridges, with full-bake/live-store parity and measured cost
- [[concepts/focused-test-groups]] — opt-in CTest groups (`test-surfel`, `test-live-edit`, `test-visual`, etc.); bare CTest skips bodies and no all-tests target exists
- [[concepts/surfel-holes]] — the 2026-09-19 "walls/roof look unsolid" fix: enclosed air is solid in the SDF, so buried cells emitted +Y-fallback disks that polluted smoothing and bled dark fill through the depth band at grazing angles; dropped them, softened micro facets, shrank object disks at creases
- [[concepts/detail-normals]] — texture detail normals (render flag bit 7 / key B): a Sobel of the albedo luminance perturbs the shading normal; +40 % HF energy, shading-normal-only so SSAO/SSR are untouched
- [[concepts/texture-conformance]] — the drop-in gate for `assets/textures/`: tileable / albedo-only / no-stamp / on-palette, with `check_texture.py` + `prepare_texture.py` (strip → flatten → seamless). 1 of 13 AI candidates passed raw, 10 after repair
- [[concepts/texture-resolution]] — measured answer to "is resolution the biggest lever?": 256²→512² buys +4.8 % near-field HF energy for free, but relief and atmosphere move the same metric ~10× more
- [[concepts/water-caustics]] — the refracted-sun cell web and the absorption-alpha ceiling: the bed is 97 % hidden past 0.7 m, so caustics are composited at the surface (bed-only: 1.7 % of pixels at 10× strength; surface: 10.6 % at 0.85)
- [[concepts/world-detail-content]] — the 2026-09-19 shoreline reed layers (`hamlet_reeds`, `hamlet_reeds_far`): heightmap-driven placement, the saturated-green lesson, and the live_edit_check keep-out zone
- [[concepts/volumetric-fog]] — the J atmosphere rewrite: march to the G-buffer hit (not a fixed 16 m), correct forward-scatter sign, bounded extinction, sky-ambient + Mie sun lobe; the 42 %-darkening regression and its gate
- [[concepts/ssao-gbuffer]] — the normal G-buffer (`m_gnorm`) and the world-scale two-band SSAO: kernel, apply/denoise split, knobs, backend parity, `ssao_check`
- [[concepts/shading-model]] — the PBR lighting model (GGX, SDF AO, sky irradiance, aerial fog, grading) *(pre-splat-rework)*
- [[concepts/voxel-object-authoring]] — layer-by-layer SDF/stamp authoring loop: vf_slice + probe checks, screenshot gate, tool landscape *(pre-splat-rework wording; the loop itself still applies)*
- [[concepts/texture-atlas]] — material textures: the 17-layer 512² atlas, the `world.json` "textures" table, triplanar sampling, the ambientCG fetch/gen tools and the in-app per-material picker
- [[concepts/per-cell-texture]] — phase-2 per-cell overrides: the record `reserved` byte's full path to the shader (and the four places it was silently lost)
- [[concepts/oriented-brush-rasterizer]] — rules for the axis-oriented brush stamps (Carve cylinder / Add growth): project the loop reach on `axisDir` (never world Y, or the brush no-ops on walls), make the profile branches meet at the switch plane instead of branching on a sign, and keep the CPU volume and the `BrushUBO` GPU preview as one shape
- [[concepts/x11-input-injection]] — driving the real window to test what `--shot` cannot see: python-xlib/XTEST mechanics, the 32-byte keymap indexing trap, ambiguous `Voxelforge` window lookup, and why ffmpeg `blackframe` is not a diff metric
- [[concepts/demo-capture]] — producing demo frames: `--shotlist` (one world load for N cameras, deterministic, but no ImGui HUD), where the hamlet actually is in world coords, and `tools/record_demo.py` + its voxel/splat A/B variant matrix. `--mode dual` is verified for a complete **single**-backend pass; the two-backend concatenation is **not** produced end to end
- [[concepts/measurement-discipline]] — a result is not evidence until the instrument can produce it: positive controls, silence ≠ healthy, switch vs cause vs mitigation, and never freeze a content name in a test

## How to navigate
- Rendering: [[entities/svo-render]] (reference backend) + [[concepts/shading-model]]; the primary backend is the Gaussian-surfel rasterizer (`--mode splat`, default) — see `docs/rendering.md`. Screen-space effects + the normal G-buffer: [[concepts/ssao-gbuffer]].
- Detail quality / tuning: [[concepts/detail-pipeline]] + [[concepts/edge-aware-surfel-radius]] + [[concepts/detail-normals]]; atmosphere: [[concepts/volumetric-fog]].
- Textures: [[concepts/texture-atlas]] (binding), [[concepts/texture-resolution]] (how much it buys), [[concepts/texture-conformance]] (the drop-in gate + repair tools).
- Live editing: [[entities/live-edit-brush]] (brush, per-voxel mode, click-vs-drag gate, undo/Clear, overlay) + [[concepts/oriented-brush-rasterizer]] (the axis-oriented stamp rules) + [[concepts/smooth-terrain-brush]] (terrain + object relaxation); water: [[concepts/water-plane]].
- Before trusting a measurement, a "clean run", or a test-only fix: [[concepts/measurement-discipline]].
- Suite/startup speed: [[concepts/load-time-field-build]]; test selection and the terrain-only live-edit coverage gap: [[concepts/focused-test-groups]].
- Verifying the GUI or live input (invisible to headless `--shot`): [[concepts/x11-input-injection]] + `tools/test_inject_iso.py`.
- Producing demo frames or stills: [[concepts/demo-capture]] + `tools/record_demo.py`.
- To add or edit world objects, follow [[concepts/voxel-object-authoring]] and the `voxel-object` skill.
- Full developer documentation: `docs/` (start at `docs/index.md`); engineering conventions for agents: `AGENTS.md`.
