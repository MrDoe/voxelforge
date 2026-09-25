#pragma once
// Surfelize: derive one anisotropic Gaussian parent surfel per outer voxel
// surface cell, plus optional small hard-edge crease bridges, from the
// records-derived VoxelField.
//
// The surfel set is chunk-sorted (16^3 chunks) so the renderer can
// frustum- and distance-cull at chunk granularity with a single draw
// call per visible chunk range, and can pick an LOD level per chunk.
//
// Usage:
//   SurfelSet set = buildSurfels(field, params);
//   // set.surfels + set.chunkRange -> upload to an SSBO, draw per chunk.
#include "voxel/common.hpp"
#include "voxel/chunk_store.hpp"
#include "voxel/voxel_field.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vf::voxel {

struct SurfelParams {
    // In-plane radius (m). Sets the Gaussian kernel footprint: baseRadius
    // 0.14 (1.4 cells) makes the cell corner (0.0707 m off-centre) sit at
    // d2 = 0.26, deep inside the Gaussian peak, so neighbouring disks
    // overlap generously and the renderer's source-over accumulation fills
    // the surface with no gaps (voxel-silhouette coverage: a disk covers
    // less of its voxel than the voxel's projected square). Larger also
    // helps; cost is overdraw.
    float baseRadius = 1.4f * VOXEL;
    // Radius reduction for opaque object parents on genuine hard edges only
    // (two or more non-opposite exposed lattice faces). Smooth curvature keeps
    // its coverage radius. The app exposes this as VF_EDGE_SHRINK.
    float edgeShrink = 0.0f;
    // Add small tangent-aligned bridges on the geometric crease. They live in
    // the always-on base+edge range, not the distance-culled micro tail, so a
    // tightened parent cannot open a hole farther away. edgeShrink == 0 keeps
    // the exact historical footprint and emits no bridges.
    bool edgeFill = true;
    float heightfieldBlend = 0.55f;   // blend terrain-top cells toward the analytic
                                       // two-scale heightfield normal (parity with the
                                       // current shader look)
    bool smoothNormals = true;
    bool terrainHeightfieldNormals = true;
    // Micro-detail: convert texture detail into real micro-surfel geometry.
    // Each base surfel spawns 0-2 deterministic child disks (same material,
    // inherited baked shadow/AO, jittered tangent offset + micro-facet normal)
    // so close-up surfaces read as moss grain, pebbles, bark relief and
    // leaflets with true parallax/occlusion instead of flat shader noise.
    // Default OFF (unit tests pin exact base counts); the app enables it.
    // The live-edit path honours it too: LiveEditor::chunkRun regenerates the
    // chunk's micro tail after every stamp via the shared buildMicroSurfels.
    bool microDetail = false;
    // Sun direction TOWARD the sun (unit): shadows + bent AO are baked
    // per-surfel at build time, so a sun change needs a rebuild (the app
    // passes its --sun direction through here on every reload).
    glm::vec3 sunDir { 0.449f, 0.8338f, 0.3207f };
    // LOD rings: extra merged-terrain surfel arrays per chunk (2x2x2 and
    // 4x4x4 cell blocks, baked CPU-side from the already-shaded base set).
    // The renderer picks a ring per chunk by chunk-AABB distance, cutting
    // far instance counts 4x / 16x. Default OFF (unit tests pin exact base
    // counts); the app enables it.
    bool lodRings = false;
    // Material-split LOD merging: a merged block emits a second surfel for
    // its dominant minority material (>=25% of members, >=2 cells) so
    // material borders (shorelines, snow/rock lines) stay readable at LOD
    // distance instead of collapsing to the majority colour. Off = the old
    // single-majority disk per block.
    bool lodMaterialSplit = true;
    // Anisotropic footprints: stretch each disk along the local crease
    // (perpendicular to the direction the surface bends toward), up to 1.6x,
    // with the radii in normal_rV.w (across) / pos_rU.w (along) and the
    // tangent in tan_aspect.xyz. Flat neighbourhoods stay isotropic; foliage
    // (mat 8) is exempt. Default OFF (unit tests pin exact base values); the
    // app enables it (VF_ANISO=0 disables).
    bool anisotropy = false;
};

struct Surfel {
    glm::vec4 pos_rU;     // xyz = world position (m), w = radiusU (m, along tangent)
    glm::vec4 normal_rV;  // xyz = geometric surface normal, w = radiusV (m)
    glm::vec4 bent_sh;    // xyz = baked bent (AO) normal, w = baked shadow 0..1
    glm::vec4 mat_ao;     // x=mat(float), y=refl, z=rough, w=packed AO metadata
    glm::vec4 tan_aspect; // xyz = in-plane tangent (unit; radiusU runs along it),
                          // w = per-cell texture override. Zero xyz = isotropic.
};

// Keep the GLSL contract in shaders/common_surfel.glsl in sync. A zero layer
// means terrain, water or unowned live-edit geometry. Object IDs occupy the
// 8-bit VoxelField object value and therefore stay inside the existing 80-byte
// surfel GPU layout instead of adding a sixth 16-byte record.
inline constexpr float kSurfelObjectAoBase = 8.0f;
inline constexpr float kSurfelLayerAoStep = 16.0f;

inline bool surfelIsObject(float packedAo)
{
    return packedAo >= kSurfelObjectAoBase - 0.5f;
}

inline uint8_t surfelLayerId(float packedAo)
{
    if (!surfelIsObject(packedAo))
        return 0;
    const float lane = std::floor(
        (packedAo - kSurfelObjectAoBase) / kSurfelLayerAoStep + 0.5f);
    return uint8_t(std::clamp(lane, 0.0f, 255.0f));
}

inline float surfelBakedAo(float packedAo)
{
    if (packedAo > 1.5f && packedAo < 5.5f)
        return packedAo - 2.0f;
    if (surfelIsObject(packedAo))
        return packedAo - (kSurfelObjectAoBase +
                          kSurfelLayerAoStep * float(surfelLayerId(packedAo)));
    return packedAo;
}

inline float packSurfelAo(float ao, uint8_t layerId)
{
    const float baked = std::clamp(ao, 0.0f, 1.0f);
    if (layerId == 0)
        return baked;
    return baked + kSurfelObjectAoBase + kSurfelLayerAoStep * float(layerId);
}

inline bool operator==(const Surfel& a, const Surfel& b)
{
    return a.pos_rU == b.pos_rU && a.normal_rV == b.normal_rV &&
           a.bent_sh == b.bent_sh && a.mat_ao == b.mat_ao &&
           a.tan_aspect == b.tan_aspect;
}

struct SurfelSet {
    std::vector<Surfel> surfels;
    // chunkRange[0..GRID_N^3-1] = start index into surfels for each
    // chunk; the last element is the total count. Monotonically
    // non-decreasing; empty chunks have range[i] == range[i+1].
    std::vector<uint32_t> chunkRange; // GRID_N^3 + 1
    // edgeStart[c] = absolute index where chunk c's small hard-edge bridges
    // begin (= its base-parent range end). The bridges stay in the always-on
    // opaque range; only the following micro-detail tail is distance-culled.
    std::vector<uint32_t> edgeStart; // GRID_N^3 + 1, or empty
    // microStart[c] = absolute index where chunk c's micro-detail surfels
    // begin (= its base+edge range end); microStart[GRID_N^3] = total count.
    // Lets the renderer skip sub-pixel micro geometry in distant chunks.
    // Empty when microDetail is off (no split). Monotonically
    // non-decreasing, always within [chunkRange[c], chunkRange[c+1]].
    std::vector<uint32_t> microStart; // GRID_N^3 + 1, or empty
    // LOD rings (only when params.lodRings): GRID_N^3 + 1 absolute offsets
    // into surfels for merged-terrain surfel runs per chunk (same layout as
    // chunkRange; equal entries = no merged surfel in that chunk). lod1 =
    // 2x2x2 cell merges, lod2 = 4x4x4. Emitted after the base+micro stream;
    // object-only chunks keep empty ranges (draw-time fallback to base).
    std::vector<uint32_t> lod1Range, lod2Range;
    size_t lod1Count = 0, lod2Count = 0;
    // Per-chunk object presence (GRID_N^3 flags): 1 when the chunk contains at
    // least one object-field surface surfel. The renderer keeps micro-detail
    // visible farther out for these chunks (foliage/props read as blobs when
    // their micros are culled, terrain grain does not).
    std::vector<uint8_t> objectChunks;
    float buildMs = 0.0f;
    size_t terrainCount = 0;
    size_t objectCount = 0;
    size_t edgeParentCount = 0;
    size_t edgeBridgeCount = 0;
};

// Build one parent surfel per outer voxel surface cell from the
// records-derived field, plus enabled hard-edge bridges. Uses only the public
// VoxelField API (colTops + objectBlockMask + sample).
SurfelSet buildSurfels(const VoxelField& field, const SurfelParams& params = {});

// Live-edit path: build the surfels of ONE chunk from the runtime store.
// The store is the merged base world + runtime edits, so the output reflects
// terrain and object edits alike. Normals come from the store's SDF gradient;
// sun shadow + AO reuse the same bakes as buildSurfels. Deterministic order
// (packed cell key). Returns empty for an empty/nonexistent chunk.
std::vector<Surfel> buildChunkSurfels(const ChunkStore& store, int chunk,
                                      const SurfelParams& params = {});

// Region variant for the live editor: only cells inside the lattice AABB
// [lo, hi) are considered (clamped to the chunk). `keys` are the packed
// lattice cells parallel to the one-surfel-per-parent `surfels` array.
// `edgeKeys`/`edgeSurfels` hold the small derived crease bridges separately;
// multiple edge children may refer to the same parent key.
struct SurfelRange {
    std::vector<uint64_t> keys;
    std::vector<Surfel> surfels;
    std::vector<uint64_t> edgeKeys;
    std::vector<Surfel> edgeSurfels;
};
SurfelRange buildChunkSurfelsRange(const ChunkStore& store, int chunk, glm::ivec3 lo,
                                   glm::ivec3 hi, const SurfelParams& params = {});

// Parallel convenience wrapper: build the requested chunks concurrently.
// Output order follows the input chunk list; every chunk's surfels are
// deterministic and independent (read-only store access).
std::vector<std::vector<Surfel>> buildChunksSurfels(
    const ChunkStore& store, const std::vector<int>& chunks,
    const SurfelParams& params = {});

// Deterministic micro-detail children of a key-sorted base run: the same
// hash rules the bake uses (0-3 child disks per base surfel, material-
// dependent, inheriting baked shadow/AO/bent). `keys` is parallel to `base`
// (both base-only, as returned by buildChunkSurfelsRange). `obj` is an
// optional parallel flag array used only for the terrain/object counters in
// `*objCount`. Shared by the bake and the live store path so a cell always
// yields the same micro geometry.
std::vector<Surfel> buildMicroSurfels(const std::vector<uint64_t>& keys,
                                      const std::vector<Surfel>& base,
                                      const uint8_t* obj = nullptr,
                                      size_t* objCount = nullptr);

// Water surface surfels: ONE fixed-level plane (WATER_LEVEL, normal +Y, mat
// id 0, mat_ao.w = 3 = ao 1 + water flag) subdivided into a regular grid over
// the whole world. Append after the opaque set; the append offset is the
// water range start.
//
// The grid is a pure coverage raster over the plane - the fragment shader
// intersects the analytic plane per fragment and shades it identically no
// matter which cell covered it, and the depth test against the opaque
// prepass hides the cells standing on dry land/objects. So the plane needs no
// per-column terrain test at build time and no flood patch after a carve: a
// dug channel is simply below the level and the plane that was always there
// becomes visible. Default 0.2 m keeps the water grain at the micro-surfel
// scale of the banks.
inline constexpr float kWaterSurfelSpacing = 0.20f;
// One water-plane cell at (wx, wz) (the grid point covering the lattice
// column below it).
Surfel makeWaterSurfel(float wx, float wz,
                       float spacing = kWaterSurfelSpacing);
std::vector<Surfel> buildWaterSurfels(float spacing = kWaterSurfelSpacing);

} // namespace vf::voxel
