# Fix distorted object rendering: replace per-column parity fill with per-component 3D flood fill

## Diagnosis (confirmed via offline replication of the algorithm)

Replicated `LayeredWorld`'s interior fill in Python against the real `.vxw`
layers (288k object records). Evidence:

1. **Giant false-solid intervals.** Sorted-x even/odd interval histogram shows
   fills of 280-310 cells (28-31 m, ~31k occurrences) and 490-500+ cells
   (~17k). Even-odd pairing mispairs shell crossings when several objects
   interleave along the same (y,z) column (tree foliage vs house roof vs
   fence), producing phantom solid slabs tens of meters long that bridge
   separate objects. In the SVO these become invisible walls: rays terminate
   early and whole areas shade wrongly -> "totally distorted".
2. **Striped canopies.** Cross-section at canopy mid-height shows alternating
   filled/hollow z-rows inside the same crown (columns whose crossing count
   ends up odd/mispaired stay hollow). -> "trees look wrong".

Both symptoms share one root cause: per-column ray-cast parity cannot know
which crossings belong to which object.

## Fix

Replace the per-(y,z)-column parity fill in `src/voxel/layered_world.cpp`
(`synthesize()`, the `m_objColX` consumer) with a per-connected-component 3D
flood fill:

1. Label object record cells into connected components
   (26-neighbourhood, BFS over an object-only hash set built from the
   coordinates already collected in `m_objColX`; ~288k cells, trivial).
2. For each component compute its integer bounding box (+1 margin).
3. Flood-fill AIR inward from the box boundary faces through non-record
   cells (6-connectivity, explicit std::vector stack, visited bitset sized to
   the box).
4. Unreached non-record cells inside the box = enclosed interior -> set the
   corresponding bits in the existing `m_objSolid` bitmap.
   Keep the existing `y < colTop` skip so terrain interior semantics are
   unchanged.
5. Everything downstream stays as-is: `m_blockSolid` marking, `fillBrick`
   solidity/appearance, SVO build.

Properties: cannot bridge separate objects (components are separated by real
air gaps), fills trunk+canopy unions correctly, immune to crossing-count
parity failures, degrades gracefully to the old hollow behaviour for
accidentally open shells (never worse than pre-fix).

## Steps

1. Edit `synthesize()` in `src/voxel/layered_world.cpp`: delete the parity
   loop; add component labeling + per-component flood fill writing into
   `m_objSolid`. Keep `m_objColX` collection in `load()` as the coordinate
   source (or store a flat object-record coord vector alongside).
2. `ninja -C build` (compiles layered_world.cpp + shaders; incremental).
3. Verify:
   - Re-run the Python replica (with the new algorithm) -> canopies solid,
     interval-length histogram shows no bucket beyond ~object diameter
     (<~60 cells), zero >=100-cell intervals.
   - `./build/voxelforge --compare` -> PASSED (meanDiff < 14/255).
   - `ctest --test-dir build` -> unit_tests + visual_check PASS.
   - Screenshots: high-res hero + close-ups of tree0/tree3/house/rocks;
     ASCII-inspect canopies (no stripes, no bridges).
   - Determinism: two identical `--shot` runs diff == 0 pixels.

## Risks / notes

- Performance: flood fill is linear in summed bbox volumes (est. < 30M cell
  visits). Current synthesize is 0.64 s; budget < 1.5 s to keep the 0.5 s
  hot-reload path snappy. Parallelize per component with the existing thread
  pattern only if needed.
- Memory: per-component visited bitsets are freed after each component;
  `m_objSolid` (134 MB) lifetime unchanged.
- Deep canopy interiors farther than the brick window from any shell keep the
  soil-brown fallback color; invisible behind the intact opaque shell unless
  screenshots later show pinhole speckle (optional follow-up: inherit each
  component's dominant material as fallback).
