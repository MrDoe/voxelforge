#pragma once
// VoxelField: records-derived geometry oracle - the single runtime source of
// truth for world geometry.
//
// Everything is derived from .vxw voxel records (no analytic scene(), no
// heightmap PNG):
//   - terrain: per-column top record (lattice y) + its material, collected
//     from the landscape layer,
//   - objects: connected components of object records flood-filled to solid
//     volumes. Each component gets an exact-ish signed distance transform on
//     its bounding grid (two 26-neighbour Dijkstra passes + an exterior air
//     flood for the sign), quantised to int8 voxels and stored in a sparse
//     hash keyed by lattice cell.
//
// Consumers: chunked-SVO brick baking (LayeredWorld::synthesize), the GPU
// terrain-height texture, picking, probes and tests.
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"
#include <cstdint>
#include <vector>

namespace vf::voxel {

class VoxelField {
public:
    // colTop/colMat: per-column landscape tops (lattice y, -1 = none) and the
    // top record's material. objCells/objMats: packed keys + material of every
    // non-landscape record cell (priority-merged union).
    void build(const std::vector<VoxelRecord>& records,
               const std::vector<int16_t>& colTop, const std::vector<uint8_t>& colMat,
               const std::vector<uint32_t>& objCells, const std::vector<uint8_t>& objMats);

    bool valid() const { return m_built; }

    struct Sample {
        float d = 1e9f;
        uint8_t mat = 0;
        bool obj = false; // true when the object field is the closest surface
    };

    // Signed distance (meters, negative inside solids) + material at a lattice
    // cell centre / world point. Terrain-less columns report +1e9.
    Sample sample(int cx, int cy, int cz) const;
    Sample sampleWorld(glm::vec3 p) const;

    // Terrain queries (records-derived replacement for the analytic heightmap).
    bool terrainColumn(int x, int z) const {
        return x >= 0 && z >= 0 && x < m_latN && z < m_latN &&
               m_colTop[size_t(z) * m_latN + size_t(x)] >= 0;
    }
    float terrainTopY(int x, int z) const { // world Y of the top surface plane
        return -0.5f * WORLD + (float(m_colTop[size_t(z) * m_latN + size_t(x)]) + 1.0f) * VOXEL;
    }
    uint8_t terrainMat(int x, int z) const { return m_colMat[size_t(z) * m_latN + size_t(x)]; }
    int latN() const { return m_latN; }
    const std::vector<int16_t>& colTops() const { return m_colTop; }

    // Terrain-height texture for the GPU (latN x latN, RG32F):
    // R = world Y of the column's top surface, G = material / 255.
    const std::vector<glm::vec2>& heightTexture() const { return m_heightTex; }

    // Byte grid (kGBlocks^3): 1 where the block holds any object-field cell.
    // OR-ed into LayeredWorld's presence grid for octree subdivision.
    const std::vector<uint8_t>& objectBlockMask() const { return m_objBlock; }
    static constexpr int blockSize() { return 4; } // must match LayeredWorld kBlockSize
    static constexpr int blocksPerChunkAxis() { return 64 / 4; }
    static constexpr int globalBlocks() { return 16 * blocksPerChunkAxis(); }

    // Coarse object-only signed distance volume for GPU shadows:
    // kObjVolN^3 texels over the whole world, int8 snorm metres
    // (see kObjVolScale). Empty space stores +127 (= far away).
    static constexpr int kObjVolN = 256;
    static constexpr float kObjVolMax = 1.26f; // metres encoded at +-127
    const std::vector<int8_t>& objectVolume() const { return m_objVol; }

    size_t objectCellsStored() const { return m_stored; }

private:
    int m_latN = int(WORLD / VOXEL);
    bool m_built = false;

    std::vector<int16_t> m_colTop;
    std::vector<uint8_t> m_colMat;
    std::vector<glm::vec2> m_heightTex;

    // sparse object field: key = packed lattice cell (+1), value =
    // uint32(uint8(sdfRaw)) | uint32(mat) << 8 ; sdf meters = int8(raw)*VOXEL
    std::vector<uint32_t> m_okey;
    std::vector<uint32_t> m_oval;
    size_t m_omask = 0;
    size_t m_stored = 0;

    std::vector<uint8_t> m_objBlock;
    std::vector<int8_t> m_objVol;

    inline size_t oslot(uint32_t k) const { return (size_t(k) * 2654435761u) & m_omask; }
    inline bool ofind(uint32_t k, uint32_t& v) const {
        if (m_omask == 0)
            return false;
        size_t i = oslot(k);
        while (m_okey[i]) {
            if (m_okey[i] - 1u == k) {
                v = m_oval[i];
                return true;
            }
            i = (i + 1) & m_omask;
        }
        return false;
    }
    void oinsert(uint32_t k, uint32_t v);

    // Bilinearly interpolated terrain height at a lattice cell centre - mirrors
    // the GPU heightAt() over m_heightTex, so baked bricks and the shader see
    // the SAME smooth terrain (no stair-step divergence).
    float smoothTerrainY(float wx, float wz) const;
    bool anyTerrainNear(int cx, int cz) const;
};

} // namespace vf::voxel
