// Deterministic intent-builder generators. These are PURE functions: same
// inputs (including seed) -> byte-identical VoxelRecord vectors. No scenery
// facts live here; callers sample ground through a functor and choose names.
#pragma once

#include "voxel/common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace vf::ai::builders {

using HeightFn = std::function<float(float, float)>;

// The live_edit_check carve keep-out: world x -3..3, z -1..6. Builders refuse
// to author there unless explicitly overridden, because reeds in that region
// once shifted the carved-water colour by 22 blue codes.
inline bool inKeepOut(float x, float z)
{
    return x >= -3.f && x <= 3.f && z >= -1.f && z <= 6.f;
}

inline int cellOf(float w)
{
    return int(std::floor((w + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL));
}
inline float worldOf(int c)
{
    return -0.5f * vf::voxel::WORLD + (float(c) + 0.5f) * vf::voxel::VOXEL;
}

// A seeded tree: trunk + a clumped canopy. The seed changes the CLUMP COUNT
// as well as their positions, so two seeds are observably different.
inline std::vector<vf::voxel::VoxelRecord> tree(glm::ivec3 anchor, float heightM,
                                                float canopyRM, uint32_t seed)
{
    using namespace vf::voxel;
    std::vector<VoxelRecord> out;
    const int hCells = std::clamp(int(std::lround(heightM / VOXEL)), 4, 120);
    const int rCells = std::clamp(int(std::lround(canopyRM / VOXEL)), 2, 60);
    const int N = int(WORLD / VOXEL);
    auto put = [&](int x, int y, int z, uint8_t mat) {
        if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= N)
            return;
        out.push_back(makeVoxelRecord(x, y, z, mat));
    };
    // Trunk: bark (17), a 1-cell core plus four neighbours at the base.
    for (int y = 0; y < hCells; ++y)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                if (dx * dx + dz * dz <= 1)
                    put(anchor.x + dx, anchor.y + y, anchor.z + dz, 17);
    // Canopy: one solid ellipsoid core plus seeded clumps around it.
    const int cy = anchor.y + hCells - 1;
    for (int dz = -rCells; dz <= rCells; ++dz)
        for (int dy = -rCells; dy <= rCells / 2; ++dy)
            for (int dx = -rCells; dx <= rCells; ++dx) {
                const float q = float(dx * dx + dz * dz) / float(rCells * rCells) +
                                float(dy * dy) / float(rCells * rCells * 4);
                if (q <= 1.f)
                    put(anchor.x + dx, cy + dy, anchor.z + dz, 8);
            }
    uint32_t s = seed ? seed : 1u;
    auto next = [&]() { s = s * 1664525u + 1013904223u; return s >> 16; };
    const int clumps = 8 + int(seed % 7u);
    for (int k = 0; k < clumps; ++k) {
        const int ox = int(next() % uint32_t(2 * rCells + 1)) - rCells;
        const int oy = int(next() % uint32_t(rCells + 1)) - rCells / 2;
        const int oz = int(next() % uint32_t(2 * rCells + 1)) - rCells;
        for (int dz = -2; dz <= 2; ++dz)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (dx * dx + dy * dy + dz * dz <= 5)
                        put(anchor.x + ox + dx, cy + oy + dy, anchor.z + oz + dz, 8);
    }
    return out;
}

// A path sampled PER CELL against the height functor (not just centreline
// heights): a ridge crossing the path lifts only the cells that stand on it.
inline std::vector<vf::voxel::VoxelRecord> path(std::array<float, 2> a,
                                                std::array<float, 2> b,
                                                float stepM, int mat,
                                                const HeightFn& height)
{
    using namespace vf::voxel;
    std::vector<VoxelRecord> out;
    if (!(stepM > 0.f) || !height)
        return out;
    const float dx = b[0] - a[0], dz = b[1] - a[1];
    const float len = std::sqrt(dx * dx + dz * dz);
    const int steps = std::max(1, int(std::ceil(len / stepM)));
    const int N = int(WORLD / VOXEL);
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const float wx = a[0] + t * dx, wz = a[1] + t * dz;
        const int cx = cellOf(wx), cz = cellOf(wz);
        const float h = height(wx, wz);
        const int cy = int(std::floor((h + 0.5f * WORLD) / VOXEL));
        for (int layer = 0; layer < 2; ++layer) {
            const int y = cy + layer;
            if (cx < 0 || cz < 0 || y < 0 || cx >= N || cz >= N || y >= N)
                continue;
            out.push_back(makeVoxelRecord(cx, y, cz, uint8_t(mat)));
        }
    }
    return out;
}

} // namespace vf::ai::builders
