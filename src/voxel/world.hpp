#pragma once
// Chunked sparse-voxel world with per-chunk octrees over 8^3 bricks.
//
// GPU pools (mirrored by svo_raymarch.comp):
//   childBase[] : per node, index of its 8 contiguous child handles
//   payload[]   : per node { validMask bits0..7, solidMask bits8..15 }
//   handles[]   : uint32 child handles
//   bricks[]    : 2 x uint32 per voxel (1024 per brick):
//                 word0 { r | g<<8 | b<<16 | sdfByte<<24 }
//                 word1 { a | refl<<8 | rough<<16 | matId<<24 }
//
// Handle encoding (low 2 bits):
//   0b00 node   -> index = h >> 2 (into childBase/payload)
//   0b01 brick  -> index = h >> 2 (into bricks, in units of 1024 uint32)
//   0b10 solid  -> terminal fully-solid cell
//   0xFFFFFFFF  -> empty terminal
#include "voxel/common.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace vf::voxel {

constexpr uint32_t kEmptyHandle = 0xFFFFFFFFu;
constexpr uint32_t kSolidHandle = 0xFFFFFFFEu;
constexpr int BRICK_N = 8;
constexpr int BRICK_VOXELS = BRICK_N * BRICK_N * BRICK_N;
constexpr int BRICK_WORDS = BRICK_VOXELS * 2;

inline bool handleIsNode(uint32_t h) { return (h & 3) == 0; }
inline bool handleIsBrick(uint32_t h) { return (h & 3) == 1; }
inline uint32_t nodeIndexOf(uint32_t h) { return h >> 2; }
inline uint32_t brickIndexOf(uint32_t h) { return h >> 2; }

struct GpuWorld {
    std::vector<uint32_t> childBase; // per node
    std::vector<uint32_t> payload;   // per node
    std::vector<uint32_t> handles;   // flat child pool
    std::vector<uint32_t> bricks;    // packed voxels
    std::vector<int32_t> chunkGrid;  // GRID_N^3 root handles (-1 = empty)

    size_t memoryBytes() const
    {
        return (childBase.size() + payload.size() + handles.size() + bricks.size() +
                chunkGrid.size()) * 4;
    }
};

class World {
public:
    void build();

    const GpuWorld& gpu() const { return m_gpu; }

    // CPU point query mirroring the shader traversal (meters).
    float sample(glm::vec3 p) const;

    struct Stats {
        size_t nodes = 0, bricks = 0, activeChunks = 0;
        float buildSeconds = 0.0f;
    };
    Stats stats() const;

private:
    GpuWorld m_gpu;
    Stats m_stats;

private:
    glm::vec3 m_worldMin { -0.5f * WORLD };
};

} // namespace vf::voxel
