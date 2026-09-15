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

// Per-chunk index bases for the chunk-local handle space. Handles stored in
// the pools are chunk-relative, so a chunk's nodes/bricks can be relocated
// (live patch) without rewriting any other chunk's handles.
struct GpuChunkInfo {
    uint32_t nodeBase = 0;   // + node index -> payload/childBase
    uint32_t childBase = 0;  // + child slot -> handles
    uint32_t brickBase = 0;  // + brick index -> bricks (BRICK_WORDS units)
    uint32_t reserved = 0;
};

struct GpuWorld {
    std::vector<uint32_t> childBase; // per node
    std::vector<uint32_t> payload;   // per node
    std::vector<uint32_t> handles;   // flat child pool
    std::vector<uint32_t> bricks;    // packed voxels
    std::vector<int32_t> chunkGrid;  // GRID_N^3 root handles (-1 = empty)
    std::vector<GpuChunkInfo> chunkInfo; // GRID_N^3 bases (uChunkInfo)

    size_t memoryBytes() const
    {
        return (childBase.size() + payload.size() + handles.size() + bricks.size() +
                chunkGrid.size() + chunkInfo.size() * 4) * 4;
    }
};

// Per-chunk SVO octree pool (chunk-local handles). Kept resident across
// reloads so only chunks touched by a changed layer need rebuilding; the
// ChunkStore rebuilds one pool from its sparse cells after an edit.
struct ChunkPool {
    std::vector<uint32_t> childBase, payload, handles, bricks;
    int32_t root = -1; // chunk root handle (-1 = empty)

    uint32_t allocNode()
    {
        payload.push_back(0);
        childBase.push_back(uint32_t(handles.size()));
        handles.resize(handles.size() + 8, kEmptyHandle);
        return uint32_t((payload.size() - 1) << 2);
    }
    uint32_t emitBrick(const uint32_t* data)
    {
        bricks.insert(bricks.end(), data, data + BRICK_WORDS);
        return uint32_t(((bricks.size() / BRICK_WORDS) - 1) << 2 | 1);
    }
};

} // namespace vf::voxel
