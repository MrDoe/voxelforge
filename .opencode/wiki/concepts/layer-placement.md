---
title: layer placement and selected-layer rotation
tags: [placement, rotation, move, translation, provenance, layer-ids, trackball, world-manifest]
sourceRefs: [src/voxel/worldfile.cpp, src/voxel/worldfile.hpp, src/voxel/layered_world.cpp, src/voxel/layered_world.hpp, src/voxel/voxel_field.cpp, src/voxel/picking.cpp, src/voxel/surfelize.cpp, src/render/splat_pass.cpp, src/app/main.cpp, shaders/common_surfel.glsl, tests/test_worldfile.cpp, tests/test_picking.cpp, tests/test_surfelize.cpp, tests/visual_check.py]
lastReviewed: 2026-09-25
---

Runtime placement of `.vxw` object/scatter layers is manifest-driven: `pos`
is a world-space translation in metres and `rot`/`rotX`/`rotZ` are yaw/pitch/roll
in degrees. `transformRecords()` applies `Ry(yaw)·Rx(pitch)·Rz(roll)` around
the authored layer's exact cell-centre bottom-center, then applies `pos`.
Identity is an exact passthrough; the placement hash dirties both the old and
new occupied chunks. See [[entities/live-edit-brush]] for the surrounding
editing workflow.

## Exact ownership, not overlap

`LayeredWorld` allocates each enabled placeable file a stable non-zero 8-bit
owner ID. It starts from an FNV-style filename hash, probes collisions, and
retains prior IDs across layer toggles/reloads. The first claimant of a lattice
cell keeps that cell and supplies its ID. The ID then propagates through:

1. the sparse merged object field and EDT;
2. `VoxelField::Sample::layer` and `PickHit::layer`;
3. `ChunkStore` cell tags and live surfel refreshes;
4. full and micro surfels, including geometry rebuilt after a store edit.

Consequently one plain LMB click in Rotate mode reads the picked cell owner
and activates that file; it never starts a rotation by itself. The AABB may
center and size the trackball but must never decide ownership. The projected
AABB centre anchors the interaction surface and its farthest corner supplies
the radius; the outer, wide-horizontal, and tall-vertical rings map to local
Y/yaw, local X/pitch, and local Z/roll. Terrain and live-added cells with no
base-file owner use ID 0 and are excluded from layer rotation.

## Packed surfel contract

The 80-byte, five-`vec4` surfel layout remains unchanged. `mat_ao.w` stores
baked AO plus one metadata lane:

- water: `AO + 2`;
- owned object: `AO + 8 + 16*ownerId`, with IDs 1–254;
- terrain/unowned geometry: plain `AO`.

Packing happens after AO smoothing, which would otherwise average the metadata
away. `shaders/common_surfel.glsl` is the shared decoder used by the forward
vertex stage, GPU-cull compute stage, and tile bin/render stages. This is why a
preview can select one exact file without moving terrain, water, or an
overlapping object in another file.

## Preview matches the commit

The preview pivot is the canonical source bottom-center plus manifest `pos`,
not the current bounding-box centre. Each ring drag composes on the object's
own local X/Y/Z axis (`Rnext = Rcurrent * Rlocal`) and converts back to the
manifest Euler convention only at the persistence boundary. If the absolute
placement changes from `Rold` to `Rnew`, an already placed point is mapped as:

```
preview = pivotPlaced + (Rnew * transpose(Rold)) * (current - pivotPlaced)
```

Adding the drag delta directly to the current point double-counts `Rold` and
does not match `transformRecords()`. `SplatPass::setRotatePreview()` carries
the enabled flag, pivot, relative rotation, and target owner ID. Forward,
GPU-cull, and tile paths apply the same owner test. During the short preview,
conservative CPU chunk culling is bypassed and micro surfels are force-included
so stale source-chunk bounds cannot erase the moving object.

On ring release, yaw/pitch/roll deltas are staged in the app rather than
written. The object stays activated for another adjustment; **Apply rotation**
adds the staged angles to the manifest and starts the rebuild, while **Cancel**
clears the preview. The final GPU preview remains enabled until
`applyWorldReload()` swaps in the committed world, preventing an old-pose flash.
Move mode follows the same staged/Apply flow for the selected axis and `pos`;
its scene gizmo draws all three colored world-axis handles and hit-tests each
one independently. Entering Rotate hides World Layers, and ring hit
testing runs before ImGui mouse capture so the Toolbox cannot block an
overlapping ring. The screen helper must use view-space depth exactly like
`splat.vert`; normalizing the world-space direction before projection shifts
the gizmo away from the rendered object. Camera movement is applied before
picking, gizmo projection, and render-push construction, so a moving camera
cannot leave the ring one frame behind. Picking and gizmo coordinates use
ImGui's logical `DisplaySize` (GLFW cursor pixels), so high-DPI windows do not
split the hit-test from the rendered object. The same flow is reachable from
the searchable inspector and focused toolbox in [[entities/live-edit-brush]].
When a ring overlaps a panel, its window is input-blocked for that pointer
position so the click-drag cannot activate an unrelated control.

## Verification and headless hooks

- `worldfile` tests pin exact translation, all three rotation axes, canonical
  pivots, local-axis/Euler roundtrips, and `Rnew·transpose(Rold)` preview math.
- picking tests assert the selected object ID resolves to the expected file;
  surfel tests assert base and micro ownership survive full/live paths.
- `VF_TEST_ROTATE_LIVE="yaw,pitch,roll"` plus `VF_ROTATE_LAYER=<file>` is a
  persistent preview-only hook. It is explicitly excluded from interactive
  ring accumulation and mouse-release commit, so a headless shot cannot
  mutate `world.json`.
- `VF_TEST_ROTATE` is the separate commit-and-rebuild hook.
- `VF_SPLAT_DEBUG=16` paints the selected owner green, other object owners red,
  and terrain/water dark blue. `visual_check.py` renders before/after ownership
  masks: green must move while red stays fixed.

## Gotchas

- Headless `--cam` values are space-separated. The quoted comma form silently
  leaves the camera at its default and produces an invalid flat comparison.
- Exact 90-degree lattice steps are cell-clean when the bottom-center pivot is
  at a cell centre. Even-width layers pivot on a cell edge and can split
  boundary-cell pre-images; prefer odd extents for cell-exact test objects.
- Never restore the old “bit 3 means any object” rotation test. It cannot
  distinguish two object files and caused whole selected chunks to move.
