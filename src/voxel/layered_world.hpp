#pragma once
// Layered world: the single runtime world source.
//
// Loads the layer family described by assets/world.json (record-only .vxw
// files: landscape.vxw, object layers, scatter, ai_edits.vxw) and synthesizes
// the chunked-SVO GpuWorld directly from those voxels. There is no merged
// cache: every renderer path (SVO ray-march, splats, probes) consumes the same
// loaded state, so layer edits / enable toggles / MCP appends are reflected
// everywhere at once.
//
// Synthesis rules (the analytic scene() SDF is truth):
//   - scene(p) is a single signed distance field (heightmap + every object
//     SDF) that is NEGATIVE inside all solids (terrain interior, object shells
//     and their enclosed interiors). Bricks bake scene(p) directly, so objects
//     read solid at every distance with no hollow-voxel / flood-leak holes.
//   - the per-brick SDF (quantised to VOXEL) is sphere-traced exactly like the
//     dense raymarch path; appearance is the exact record colour where one
//     exists, otherwise the palette of scene(p)'s material,
//   - octree presence (blockSolid) is extended into enclosed interiors by
//     sampling scene() at block centres/corners, seeded from the object
//     connected-component boxes,
//   - air below WATER_LEVEL is marked as water volume (mat id 9, non-hit).
//
// reloadIfChanged() stats the manifest + layer files and rebuilds when any of
// them changed on disk - this is how MCP edits and GUI checkboxes reach a
// running instance without an external repack step.
#include "voxel/world.hpp"
#include "voxel/worldfile.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace vf::voxel {

class LayeredWorld {
public:
    struct Stats {
        size_t records = 0;
        size_t nodes = 0, bricks = 0, activeChunks = 0;
        double buildSeconds = 0.0;
        size_t memoryBytes = 0;
    };

    // Read manifest + enabled layers, merge with priority, synthesize the SVO.
    // Returns false when no readable manifest exists (caller falls back to the
    // procedural World::build path).
    bool load(const std::string& manifestPath);

    // Rebuild if any input file changed since the last load. Returns true
    // when a rebuild happened.
    bool reloadIfChanged();

    bool loaded() const { return m_loaded; }

    const GpuWorld& gpu() const { return m_gpu; }
    const std::vector<VoxelRecord>& records() const { return m_records; }
    const std::vector<worldfile::WorldLayer>& layers() const { return m_layersMeta; }
    const Stats& stats() const { return m_stats; }

private:
    bool synthesize();

    std::string m_manifestPath;
    bool m_loaded = false;

    GpuWorld m_gpu;
    std::vector<VoxelRecord> m_records;                    // priority-merged union
    std::vector<worldfile::WorldLayer> m_layersMeta;       // enabled manifest entries
    std::vector<int16_t> m_colTop;                         // per-column landscape top (lattice y)

    // packed cellKeys of object (non-landscape) records; grouped into
    // connected components and flood-filled to solid interiors in synthesize().
    std::vector<uint32_t> m_objCells;
    std::vector<uint8_t> m_objMats;                        // material per object cell (parallel to m_objCells)
    std::vector<uint8_t> m_blockSolid;                     // global presence grid (records+interior)

    struct Signature {
        std::string path;
        unsigned long long mtime = 0;                      // nanoseconds since epoch
        unsigned long long size = 0;
    };
    std::vector<Signature> m_signatures;

    Stats m_stats;
};

} // namespace vf::voxel
