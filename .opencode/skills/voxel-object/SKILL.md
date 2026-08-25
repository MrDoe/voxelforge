---
name: voxel-object
description: Author detailed voxel objects (props, structures, trees, rocks, stamps) in voxelforge layer by layer as SDF code in src/voxel/common.hpp, checking each layer with vf_slice ASCII cross-sections and --probe point queries, then a final screenshot gate judged via ascii_view.py pixel maps plus ctest. Use when asked to create, add, edit, or debug any world object, prop, building, vegetation, or stamp.
---

# Voxel object authoring (layer by layer, CPU-first verification)

All scene geometry is CPU truth in `src/voxel/common.hpp`. There is no mesh
import by design: objects are analytic SDF functions or literal "stamps" made
of 0.1 m cells. Rendered detail caps at `VOXEL = 0.1 m` - sub-10 cm modelling
effort is invisible, so build chunky and readable.

## Where things live

| What | Where |
|---|---|
| Object SDFs (`houseAt`, `treeAt`, `rocksAt`, `bushesAt`), primitives, stamps | `src/voxel/common.hpp` |
| Composition into the world | `scene(p)` at the bottom of `common.hpp` (min-combine your object like the others) |
| Palette / surface response | `kPalette[9]`, `kMaterialReflection[9]` (same file) |
| Placement | constexpr spot arrays (`kTreeSpots`) or hash-grid scatter (`bushesAt`, cell `kBushCell`) |
| Terrain ground height | `sharedHeightmap().sample(x, z)` - anchor objects to it so they hug slopes |

Material ids: `0` grass dark, `1` grass light, `2` soil, `3` sand, `4` rock,
`5` light rock, `6` wood, `7` roof shingle, `8` foliage. Pick from these; a new
id means extending both arrays (and the shader legend stays in sync via the
bake, not by hand).

## Contracts

- Object functions return `ObjHit { float d; uint8_t mat; }`; `d < 0` means
  inside. Combine layers with `min` into a running `best`, tracking `mat`.
- Carve openings by intersecting with the negated shape:
  `wall = max(wall, -door)` (see `houseAt`).
- Every scene() evaluation is hot: bake calls it ~10M times per regen and AO
  taps call it per frame. Give scatter objects a cheap reject first
  (distance-squared check against the spot, vertical cull) exactly like
  `treesAt`/`rocksAt` do.
- Determinism only: use `hash2`-based variation, never wall-clock or RNG state.

## Primitives available

Box `sdBoxF`, logs along X/Z `sdLogX`/`sdLogZ`, cylinder `sdCylY`, capsule
`sdCapsule`, ellipsoid `sdEllipsoid`, tapered cone `sdConeY`, smooth union
`smin(a,b,k)` (sparingly - it softens distances). Comment each construction
layer bottom-up like `houseAt` does: `// L0: ...`, `// L1..Ln: ...`.

## Voxel stamps (literal block-by-block building)

For signs, mosaics, ruins, pixel-art-like shapes:

```cpp
static const StampCell kSignCells[] = {
    // dx,dy,dz in 0.1 m cells from origin, mat = palette id
    { 0, 0, 0, 6 }, { 1, 0, 0, 6 }, { 2, 0, 0, 5 }, ...
};
inline ObjHit signAt(glm::vec3 p) {
    glm::vec3 o(kSignPos.x, sharedHeightmap().sample(kSignPos.x, kSignPos.y), kSignPos.y);
    StampHit s = stampAt(p, o, kSignCells, std::size(kSignCells));
    return { s.d, s.mat };
}
```

Keep stamps <= ~1000 cells and >= 1 cell thick. Distance semantics: exact cube
distance near cells; conservative UNDERestimate outside the AABB (safe);
`+VOXEL` inside empty pockets. Cells are exactly one voxel -> survive sdfByte
quantization by construction.

## The loop: one layer per iteration

### Layer check (fast CPU path - no GPU, no window, NO world.vxw regen)

1. Implement ONE layer in `common.hpp`.
2. Rebuild the slice tool (it compiles scene truth directly):
   `ninja -C build vf_slice`
3. Look through it:
   ```bash
   ./build/vf_slice --axis z --center X Y Z --span 10        # side section
   ./build/vf_slice --axis x --center X Y Z --span 10        # end section
   ./build/vf_slice --axis y --center X Y Z --span 8         # horizontal cut
   ```
   Solid cells print as material glyphs (`w` wood, `#` rock, ...), `+` marks
   the within-15 cm shell. Verify: silhouette, thickness, carve-outs, material
   assignment, symmetry - before moving on.
4. Spot-check exact points without rendering:
   `./build/voxelforge --probe X Y Z` prints signed distance + material id
   (exits before Vulkan init - works anywhere).
5. Fix or advance to the next layer. Only iterate upward: foundation ->
   body -> openings -> roof/details, mirroring how `houseAt` is structured.

`world.vxw` is NOT needed for steps 2-4: probe/slice evaluate `scene()` which
only lazy-loads `assets/heightmap.png`.

### Final acceptance (once, after the last layer)

1. Bake and rebuild: `ninja -C build world voxelforge` (regenerates the layer
   family + `world.json` + merged `world.vxw` from the new scene; never hand-edit
   `world.vxw` itself — it's derived). Data-only changes can skip the recompile:
   edit/add an object `.vxw` layer directly; splats pick it up on next launch,
   raymarch after the next `ninja -C build world`.
2. Render 2-3 close shots framing the object from different sides:
   ```bash
   ./build/voxelforge --shot /tmp/opencode/vf/<obj>_a.ppm \
       --cam <px> <py> <pz> <tx> <ty> <tz> --width 640 --height 360
   ```
3. Judge them ONLY via pixel evidence - never a vision model (it reports
   details that do not exist):
   ```bash
   python3 .opencode/skills/voxel-object/scripts/ascii_view.py <shot.ppm> 96 40
   python3 .opencode/skills/voxel-object/scripts/ascii_view.py <shot.ppm> 60 30 --crop <x> <y> <w> <h>
   ```
   Check the glyph map: object present and correctly placed/sized, no black
   holes in silhouette, expected hue families (wood warm `y/Y`, foliage `g`,
   rock neutral `#`). Screenshots exist to catch render-side integration bugs
   (LOD gating, culling, palette/shading under sun+fog+ACES) that CPU checks
   cannot see - historically real (the LOD t0 bug hid entire grass cards).
4. Regression gate: `ctest --test-dir build` (unit_tests + visual_check on the
   hero/house/water canonical shots must stay green).

## Gotchas

- **Tall objects need a bake-band extension**: `nearObject()` in
  `tools/heightmap_gen.cpp` decides which columns get an expanded y-sampling
  band (otherwise only terrain +/- 3 voxels are recorded there). If your
  object rises well above local ground outside existing radii, extend that
  lambda's spots/bounds. Raymarch/SVO picks up everything automatically; this
  affects the explicit voxel records (splat source).
- **Splat-mode sync**: `App::buildSplatData` (src/app/main.cpp) hardcodes
  `houseAt/treesAt/rocksAt/bushesAt` for sub-splat normals. Add your object
  function there too if splat mode should shade it correctly.
- World bounds are +/-51.2 m; water level `-0.9`. Objects below water need
  underwater materials to look right.
- Changing constants? `WORLD/VOXEL/GRID_N/BRICK_N` are load-bearing across the
  bake, worldfile meta check, and shaders - do not touch them casually.
- After clean checkout or asset deletion run `ninja -C build world` first;
  `vf_tests` fails at build step otherwise.
