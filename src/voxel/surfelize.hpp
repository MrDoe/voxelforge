#pragma once
// Surfelize: derive one anisotropic Gaussian surfel per outer voxel
// surface cell from the records-derived VoxelField.
//
// The surfel set is chunk-sorted (16^3 chunks) so the renderer can
// frustum- and distance-cull at chunk granularity with a single draw
// call per visible chunk range, and can pick an LOD level per chunk.
//
// Usage:
//   SurfelSet set = buildSurfels(field, params);
//   // set.surfels + set.chunkRange -> upload to an SSBO, draw per chunk.
#include "voxel/common.hpp"
#include "voxel/voxel_field.hpp"
#include <cstdint>
#include <vector>

namespace vf::voxel {

struct SurfelParams {
    // In-plane radius (m). Must satisfy (halfDiagonal / baseRadius)^2 < coreD2
    // (splat core threshold, default 0.55): with 0.1 m cells the corner sits at
    // 0.0707 m, so baseRadius 0.14 puts the corner at d2 = 0.26, well inside
    // the opaque core, with margin for tilted-neighbour wedge gaps and for
    // voxel-silhouette coverage (a disk covers less of its voxel than the
    // voxel's projected square). Larger also helps; cost is overdraw.
    float baseRadius = 1.4f * VOXEL;
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
    bool microDetail = false;
    // Sun direction TOWARD the sun (unit): shadows + bent AO are baked
    // per-surfel at build time, so a sun change needs a rebuild (the app
    // passes its --sun direction through here on every reload).
    glm::vec3 sunDir { 0.449f, 0.8338f, 0.3207f };
};

struct Surfel {
    glm::vec4 pos_rU;     // xyz = world position (m), w = radiusU (m)
    glm::vec4 normal_rV;  // xyz = geometric surface normal, w = radiusV (m)
    glm::vec4 bent_sh;    // xyz = baked bent (AO) normal, w = baked shadow 0..1
    glm::vec4 mat_ao;     // x=mat(float), y=refl, z=rough, w=baked AO +2 if water
};

inline bool operator==(const Surfel& a, const Surfel& b)
{
    return a.pos_rU == b.pos_rU && a.normal_rV == b.normal_rV &&
           a.bent_sh == b.bent_sh && a.mat_ao == b.mat_ao;
}

struct SurfelSet {
    std::vector<Surfel> surfels;
    // chunkRange[0..GRID_N^3-1] = start index into surfels for each
    // chunk; the last element is the total count. Monotonically
    // non-decreasing; empty chunks have range[i] == range[i+1].
    std::vector<uint32_t> chunkRange; // GRID_N^3 + 1
    // microStart[c] = absolute index where chunk c's micro-detail surfels
    // begin (= its base range end); microStart[GRID_N^3] = total count.
    // Lets the renderer skip sub-pixel micro geometry in distant chunks.
    // Empty when microDetail is off (no split). Monotonically
    // non-decreasing, always within [chunkRange[c], chunkRange[c+1]].
    std::vector<uint32_t> microStart; // GRID_N^3 + 1, or empty
    float buildMs = 0.0f;
    size_t terrainCount = 0;
    size_t objectCount = 0;
};

// Build one surfel per outer voxel surface cell from the records-derived
// field. Uses only the public VoxelField API (colTops + objectBlockMask
// + sample).
SurfelSet buildSurfels(const VoxelField& field, const SurfelParams& params = {});

// Water surface surfels on a regular grid (spacing m) wherever the terrain
// top sits below WATER_LEVEL. Normal +Y, mat id 0, mat_ao.w = 3 (ao 1 +
// water flag 2; the splat shader branches to water shading on the flag).
// Append after the opaque set; the append offset is the water range start.
// Default 0.2 m keeps water grain at the micro-surfel scale of the banks.
std::vector<Surfel> buildWaterSurfels(const VoxelField& field, float spacing = 0.20f);

} // namespace vf::voxel
