---
name: voxel-object
description: Author detailed voxel objects (props, structures, trees, rocks, stamps) in voxelforge either as SDF code in src/voxel/common.hpp (bake path) or at runtime via the vf_mcp tools add_voxels/write_object/read_object/delete_object (no recompile). Verify with vf_slice ASCII cross-sections, --probe point queries, and a final screenshot gate judged via ascii_view.py pixel maps plus ctest. Use when asked to create, add, edit, modify, or debug any world object, prop, building, vegetation, or stamp.
---

# Voxel object authoring (layer by layer, CPU-first verification)

There are **two authoring paths**, and an AI agent should pick by need:

1. **Baker-side SDF authoring** (permanent, richest). Objects are defined as
   analytic truth in `src/voxel/common.hpp` (SDF functions or literal 0.1 m-cell
   "stamps") and swept into `.vxw` record layers by the offline bake
   (`ninja -C build world`). Use this for terrain-coupled, shader-quality props
   (houses, trees, fences) — see the loop below.
2. **Runtime object authoring via the MCP server `vf_mcp`** (immediate, no
   recompile, AI-driven). An agent creates or modifies arbitrary objects by
   calling MCP tools that write `.vxw` records directly. This is the path for
   chat/MCP edits and is documented in the **"Runtime object authoring"**
   section below. **The `.vxw` binary is never hand-written** — it is always
   produced by these tools or by the bake.

Both paths converge on the same on-disk format and the same `VoxelField`
consumer; rendered detail caps at `VOXEL = 0.1 m` - sub-10 cm modelling effort
is invisible, so build chunky and readable.

## Where things live

| What | Where |
|---|---|
| Object SDFs (`houseAt`, `treeAt`, `rocksAt`, `bushesAt`, …), primitives, stamps | `src/voxel/common.hpp` |
| Composition into the world | the sweep section of `tools/heightmap_gen.cpp` — call your shape there and add a layer output like the existing ones |
| Palette / surface response | `kPalette[9]`, `kMaterialReflection[9]` (common.hpp; mirror in `shaders/svo_raymarch.comp`) |
| Placement | constexpr spot arrays (`kTreeSpots`) or hash-grid scatter (`bushesAt`, cell `kBushCell`) |
| Terrain ground height | `LayeredWorld::field().terrainHeightAtWorld(x, z)` at runtime, or `sharedHeightmap().sample(x, z)` in baker code - anchor objects so they hug slopes |

Material ids: `0` grass dark, `1` grass light, `2` soil, `3` sand, `4` rock,
`5` light rock, `6` wood, `7` roof shingle, `8` foliage. Pick from these; a new
id means extending both arrays (and the shader legend stays in sync via the
bake, not by hand).

## Contracts

- Object functions return `ObjHit { float d; uint8_t mat; }`; `d < 0` means
  inside. Combine layers with `min` into a running `best`, tracking `mat`.
- Carve openings by intersecting with the negated shape:
  `wall = max(wall, -door)` (see `houseAt`).
- Every evaluation is hot: one bake calls shape SDFs ~10M times per regen.
  Give scatter objects a cheap reject first (distance-squared check against
  the spot, vertical cull) exactly like `treesAt`/`rocksAt` do.
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

## Runtime object authoring (MCP / `vf_mcp`)

The MCP server exposes tools that write `.vxw` records live; the running
`voxelforge` instance hot-reloads within ~1 s (it polls `world.json` + layer
mtimes). Use these when an agent must **create or modify arbitrary objects**
without a recompile/bake. The `.vxw` binary is produced by the tool, never
hand-written.

### Tools

- `add_voxels` — append an arbitrary voxel shape to the live `ai_edits.vxw`
  layer. Args: `anchor:[x,y,z]` (lattice cells) **or** `ground:[x,z]` (snaps to
  terrain), plus `cells:[{dx,dy,dz, mat?, r?,g?,b?, refl?, rough?}, ...]`
  relative to the anchor. `mat` is a palette id (0-8); omit `r,g,b` to derive
  colour from the palette, omit `refl,rough` for the material's default surface
  response. Best for one-off props placed at a picked spot.
- `write_object` — create **or overwrite** a standalone named object layer
  `<name>.vxw` from **absolute** voxels
  `[{x,y,z, mat?, r?,g?,b?, refl?, rough?}, ...]` (lattice coords `0..1023`).
  Registers + enables the layer in `world.json` so it joins the merge and
  hot-reloads. Use this for a reusable, named object (a chair, a bridge, a
  creature) the agent can later `read`/`delete`.
- `read_object` — dump an existing object layer's voxels (absolute `x,y,z` +
  `mat` + `r,g,b` + `refl,rough`) as JSON. The read-modify-write loop for
  **modifying** an object is: `read_object` -> edit the cell list ->
  `write_object` with the same `name` (overwrites in place).
- `delete_object` — remove a named object layer file + its manifest entry
  (never `landscape`/`packed`).

Fuzzy tool names from small models are normalized onto these
(`create_object`, `voxel_art`, `new_layer`, `inspect_object`, `remove_object`,
…), so an agent can use natural phrasing.

### Creature scale & materials

Real-world sizes in meters; divide by `VOXEL = 0.1 m` for voxel counts.

| Creature | Length (x) | Width (z) | Height (y) | Body material |
|---|---|---|---|---|
| horse | ~2.0 | ~0.9 | ~1.4 (+legs) | wood `6` + custom brown `rgb:[120,72,40]` |
| cow | ~2.4 | ~1.1 | ~1.5 | soil `2` / wood `6` brown |
| sheep | ~1.2 | ~0.7 | ~0.9 | foliage `8` (curly) |
| dog | ~1.0 | ~0.4 | ~0.6 | wood `6` brown |
| bird | ~0.3 | ~0.3 | ~0.3 | rock `4` + foliage `8` |

Palette browns are limited (soil `2`, wood `6` reddish, rock `4/5` grey), so for
living creatures prefer an explicit `rgb:[r,g,b]` on primitives — the brick
still tags `mat` for material response, so keep `mat` in the same family as the
colour (brown body → `mat:6`, dark legs → `mat:6` or `2`).

### Worked example: a standing horse

This is the canonical "compose a complex object" recipe. One `write_object`
call with `shapes`, all relative to a `ground:[x,z]` anchor. Copy, then nudge
offsets/sizes to taste; verify with `read_object` (AABB) before rendering.

```json
{
  "name": "horse",
  "ground": [6.5, 12.5],
  "shapes": [
    { "type": "ellipsoid", "at": [0, 4, 0],  "radii": [0.9, 0.5, 0.4],  "mat": 6, "rgb": [120,72,40] },
    { "type": "ellipsoid", "at": [-6, 4, 0], "radii": [0.5, 0.45, 0.38], "mat": 6, "rgb": [120,72,40] },
    { "type": "box",       "at": [8, 6, 0],  "size": [5, 7, 4],         "mat": 6, "rgb": [120,72,40] },
    { "type": "ellipsoid", "at": [12, 12, 0],"radii": [0.35,0.35,0.3],  "mat": 6, "rgb": [120,72,40] },
    { "type": "cylinder",  "at": [6, 0, 3],  "radius": 0.12, "height": 0.9, "mat": 6, "rgb": [110,66,36] },
    { "type": "cylinder",  "at": [6, 0, -3], "radius": 0.12, "height": 0.9, "mat": 6, "rgb": [110,66,36] },
    { "type": "cylinder",  "at": [-6, 0, 3], "radius": 0.12, "height": 0.9, "mat": 6, "rgb": [110,66,36] },
    { "type": "cylinder",  "at": [-6, 0, -3],"radius": 0.12, "height": 0.9, "mat": 6, "rgb": [110,66,36] }
  ]
}
```

Add ears (`box` at `[12,15,±2]`, `size:[1,2,1]`), a tail (`box` at
`[-10,8,0]`, `size:[3,1,1]`), and a `foliage(8)` mane (a thin `box` along the
neck top) to read as a horse rather than a log with sticks. `read_object` then
reports e.g. `size 19x12x9 voxels (~1.9m x 1.2m x 0.9m)` — sanity-check that
the length/height match a horse before you spend a render.

The same pattern scales to any creature: pick a base anchor with `ground`, then
stack `ellipsoid` (body/head/haunches), `box` (neck/torso/snout), and
`cylinder` (legs/neck) primitives with relative `at` offsets. For a one-off prop
you can instead call the individual `add_box`/`add_ellipsoid`/`add_cylinder`
tools (each takes an optional `rgb` for custom colour) or `add_voxels` for
fully hand-authored shapes.

### The VXW record the agent is authoring

Each record is one 0.1 m surface voxel:

| Field | Meaning |
|---|---|
| `x,y,z` | lattice index; world pos = `-worldSize/2 + (i+0.5)*voxelSize` (`512` ≈ world center). Valid `0..1023`. |
| `mat` | palette id 0-8 (grass dark/light, soil, sand, rock, light rock, wood, roof, foliage). Drives brick material + `isObjectSurface()`. |
| `r,g,b` | 0-255 sRGB. Omit to inherit `kPalette[mat]`. |
| `refl,rough` | 0-255 → 0..1 surface response. Omit to inherit `kMaterialReflection[mat]`. |

Records are surface voxels only (the tool rasterizes a thin shell band);
`EditableWorld::append` dedupes by `(x,y,z)` (first wins) and preserves any
existing `ai_edits` content across regens.

### Workflow

1. Find placement: `ground {x,z}` → suggested anchor cell; or `probe {x,y,z}`
   to confirm a spot is empty/solid.
2. Author: `add_voxels` (relative, quick) **or** `write_object` (absolute,
   named, reusable). Keep shapes readable at 0.1 m; hundreds–thousands of
   voxels is normal, tens of thousands is fine for a one-shot `write_object`.
3. Verify: `./build/voxelforge --selftest --width 640 --height 360` or a
   `--shot` from a framing camera; judge via `ascii_view.py` (never a vision
   model). `read_object` lets you inspect exactly what landed.
4. Modify: `read_object` -> adjust cells (change `mat`, move `x,y,z`) ->
   `write_object` with the same name. Iterate.
5. Regression gate: `ctest --test-dir build` (unit_tests + visual_check).

### Gotchas (runtime path)
- `write_object` coordinates are **absolute lattice cells**; `add_voxels`
  `cells` are **relative to the anchor** — mixing them up drops objects into
  the void or onto the world center.
- Layers merge first-wins-a-cell in manifest order; a new object layer is
  inserted before `landscape`, so it overrides terrain where they overlap
  (good for carving into a hill, but terrain will not "win" back).
- Use `mat` from the 9-entry palette; custom `r,g,b` with a `mat` still tags the
  brick material (`isObjectSurface`), so keep colour and `mat` family matched.
- The app only reloads when `world.json` or an enabled layer file changes mtime;
  `write_object`/`delete_object` rewrite the manifest, so the reload fires
  automatically.

## The loop: one layer per iteration

### Layer check (fast CPU path - no GPU, no window, no rebake needed)

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

`world.json` layers ARE the runtime truth: probe/slice evaluate the VoxelField which
only lazy-loads `assets/heightmap.png`.

### Final acceptance (once, after the last layer)

1. Bake and rebuild: `ninja -C build world voxelforge` (regenerates the layer
   family + `world.json`; never hand-edit the derived `.vxw` layers).
   Data-only changes can skip the recompile: edit/add an object `.vxw` layer
   directly; the app hot-reloads it live within ~0.5 s.
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
- World bounds are +/-51.2 m; water level `-0.9`. Objects below water need
  underwater materials to look right.
- Changing constants? `WORLD/VOXEL/GRID_N/BRICK_N` are load-bearing across the
  bake, worldfile meta check, and shaders - do not touch them casually.
- After clean checkout or asset deletion run `ninja -C build world` first;
  field-consuming tools/tests abort with "run 'ninja -C build world'" otherwise.

Full authoring context: `docs/architecture.md`, `docs/contributing.md`
("Add an authored world object"), format details in `docs/world-format.md`.
