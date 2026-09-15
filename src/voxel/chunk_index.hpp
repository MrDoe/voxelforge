#pragma once
// Canonical chunk indexing shared by every world path.
//
// A chunk spans CHUNK_N^3 lattice cells (64^3); the chunk grid is GRID_N^3
// (16^3). The index is z-major:
//
//   ci = (cz * GRID_N + cy) * GRID_N + cx
//
// matching the GPU SVO uGrid layout (common_svo.glsl map()) and
// LayeredWorld's chunkRangeFromAABB. The surfel/splat path uses the same
// convention (it used to be x-major, which required two different
// derivations for the same chunk).
#include "voxel/common.hpp"

namespace vf::voxel {

constexpr int kChunkGridN = GRID_N;
constexpr int kChunkCount = kChunkGridN * kChunkGridN * kChunkGridN;

inline constexpr int chunkIndexOf(int cx, int cy, int cz)
{
    return (cz * kChunkGridN + cy) * kChunkGridN + cx;
}

// Lattice cell (0..WORLD/VOXEL-1) -> chunk index.
inline constexpr int chunkIndexOfCell(int x, int y, int z)
{
    return chunkIndexOf(x / CHUNK_N, y / CHUNK_N, z / CHUNK_N);
}

inline constexpr void chunkCoordsOf(int ci, int& cx, int& cy, int& cz)
{
    cx = ci % kChunkGridN;
    cy = (ci / kChunkGridN) % kChunkGridN;
    cz = ci / (kChunkGridN * kChunkGridN);
}

// Lattice cell -> coordinate inside its chunk (0..CHUNK_N-1).
inline constexpr int chunkLocalCoord(int c) { return c % CHUNK_N; }

} // namespace vf::voxel
