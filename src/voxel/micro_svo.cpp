// micro_svo.cpp - bake a MicroField into its own self-contained SVO world.
//
// Mirrors the world bake's structure (LayeredWorld::buildInto): every chunk
// gets a private ChunkPool, an empty box short-circuits to -1, a full box to a
// solid terminal, and only mixed boxes subdivide down to BRICK_N. Building per
// chunk keeps every handle CHUNK-LOCAL, so the merged world records a base per
// chunk (GpuChunkInfo) and no cross-chunk index is ever offset-adjusted.
#include "voxel/micro_svo.hpp"
#include "voxel/world.hpp"

#include <functional>
#include <memory>

namespace vf::voxel {

namespace {

// Cells along one micro chunk edge. A chunk descends to BRICK_N cubes, so with
// BRICK_N = 8 a two-level chunk covers 16 cells: chunk voxel = 16 * world voxel.
constexpr int kMicroChunkCells = 16;

// 0 = empty, 1 = fully solid, 2 = mixed (subdivide). Reads the dense field.
int classifyMicroBox(const MicroField& f, int x0, int y0, int z0, int side)
{
    bool any = false, all = true;
    for (int z = z0; z < z0 + side; ++z)
        for (int y = y0; y < y0 + side; ++y)
            for (int x = x0; x < x0 + side; ++x) {
                const bool s = f.inside(x, y, z) && f.solid[f.idx(x, y, z)] != 0;
                any = any || s;
                all = all && s;
                if (any && !all)
                    return 2;
            }
    if (any)
        return 1;
    return 0;
}

} // namespace

void synthesizeMicroSvo(const MicroField& f, glm::vec3 worldOffset,
                        MicroWorld& out)
{
    out.voxel = f.voxel;
    out.fieldDim = f.dim;
    out.gridN = microGridN(f.extent(), f.voxel);
    const float chunkM = float(kMicroChunkCells) * f.voxel;
    out.chunkM = chunkM;
    out.worldMin = f.origin + worldOffset;
    out.worldMax = out.worldMin + f.extent();

    const int gridN = out.gridN;
    const size_t numChunks = size_t(gridN) * size_t(gridN) * size_t(gridN);
    out.chunkGrid.assign(numChunks, -1);
    out.chunkInfo.assign(numChunks, GpuChunkInfo {});

    std::vector<std::unique_ptr<ChunkPool>> pools(numChunks);
    for (int cz = 0; cz < gridN; ++cz)
        for (int cy = 0; cy < gridN; ++cy)
            for (int cx = 0; cx < gridN; ++cx) {
                const size_t ci = (size_t(cz) * size_t(gridN) + size_t(cy)) *
                                      size_t(gridN) + size_t(cx);
                const int x0 = cx * kMicroChunkCells, y0 = cy * kMicroChunkCells,
                          z0 = cz * kMicroChunkCells;
                const int cls = classifyMicroBox(f, x0, y0, z0, kMicroChunkCells);
                if (cls == 0)
                    continue; // chunkGrid stays -1
                if (cls == 1) {
                    out.chunkGrid[ci] = int32_t(kSolidHandle);
                    continue;
                }

                auto pool = std::make_unique<ChunkPool>();
                std::function<int32_t(int, int, int, int)> build =
                    [&](int bx, int by, int bz, int side) -> int32_t {
                    const int c = classifyMicroBox(f, bx, by, bz, side);
                    if (c == 0)
                        return -1;
                    if (c == 1)
                        return int32_t(kSolidHandle);
                    if (side == BRICK_N) {
                        uint32_t data[BRICK_WORDS];
                        fillMicroBrick(f, data, bx, by, bz);
                        return int32_t(pool->emitBrick(data));
                    }
                    const int half = side / 2;
                    const uint32_t nodeH = pool->allocNode();
                    const uint32_t base = pool->childBase[nodeIndexOf(nodeH)];
                    uint32_t validMask = 0, solidMask = 0;
                    for (int i = 0; i < 8; ++i) {
                        const int ox = bx + (i & 1) * half;
                        const int oy = by + ((i >> 1) & 1) * half;
                        const int oz = bz + ((i >> 2) & 1) * half;
                        const int32_t ch = build(ox, oy, oz, half);
                        pool->handles[base + size_t(i)] = uint32_t(ch);
                        if (ch >= 0 || ch == int32_t(kSolidHandle))
                            validMask |= 1u << i;
                        if (ch == int32_t(kSolidHandle))
                            solidMask |= 1u << i;
                    }
                    if (validMask == 0)
                        return -1;
                    if (solidMask == 0xFF)
                        return int32_t(kSolidHandle);
                    pool->payload[nodeIndexOf(nodeH)] = validMask | (solidMask << 8);
                    return int32_t(nodeH);
                };
                pool->root = build(x0, y0, z0, kMicroChunkCells);
                if (pool->root >= 0) {
                    out.chunkGrid[ci] = pool->root;
                    pools[ci] = std::move(pool);
                }
            }

    // deterministic merge; handles stay chunk-local, bases recorded per chunk
    out.handles.clear();
    out.childBase.clear();
    out.payload.clear();
    out.bricks.clear();
    size_t nodeOff = 0, hOff = 0, bOff = 0;
    for (size_t ci = 0; ci < numChunks; ++ci) {
        const ChunkPool* p = pools[ci].get();
        if (!p || p->root < 0)
            continue;
        out.chunkInfo[ci].nodeBase = uint32_t(nodeOff);
        out.chunkInfo[ci].childBase = uint32_t(hOff);
        out.chunkInfo[ci].brickBase = uint32_t(bOff);
        out.handles.insert(out.handles.end(), p->handles.begin(), p->handles.end());
        out.childBase.insert(out.childBase.end(), p->childBase.begin(),
                             p->childBase.end());
        out.payload.insert(out.payload.end(), p->payload.begin(), p->payload.end());
        out.bricks.insert(out.bricks.end(), p->bricks.begin(), p->bricks.end());
        nodeOff += p->payload.size();
        hOff += p->handles.size();
        bOff += p->bricks.size() / BRICK_WORDS;
    }
}

} // namespace vf::voxel