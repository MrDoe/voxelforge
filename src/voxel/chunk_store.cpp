#include "voxel/chunk_store.hpp"
#include "voxel/worldfile.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <queue>
#include <unordered_set>

namespace vf::voxel {

namespace {

// Distance-transform cap in voxels (matches the bake: air beyond this is far).
constexpr int kFar = 32;
constexpr int kMaxDist = kFar * 10; // tenths of a voxel
// Live-rebuild band: edits refresh SDF values within this many cells (beyond
// it the previous values stay, which is what keeps a stamp's cost proportional
// to the brush instead of the whole chunk).
constexpr int kLiveBand = 12;

inline uint32_t blockKeyOf(int lx, int ly, int lz)
{
    return uint32_t((lx >> 3) | ((ly >> 3) << 3) | ((lz >> 3) << 6));
}

inline uint32_t cellIndexInBrick(int lx, int ly, int lz)
{
    return (uint32_t(lz & 7) * BRICK_N + uint32_t(ly & 7)) * BRICK_N +
           uint32_t(lx & 7);
}

inline void paletteOf(uint8_t mat, uint8_t& r, uint8_t& g, uint8_t& b,
                      uint8_t& refl, uint8_t& rough)
{
    const glm::vec3& c = kPalette[std::min(int(mat), kPaletteN - 1)];
    const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
    r = uint8_t(c.r * 255.0f);
    g = uint8_t(c.g * 255.0f);
    b = uint8_t(c.b * 255.0f);
    refl = uint8_t(rr.x);
    rough = uint8_t(rr.y);
}

// 26-neighbour offsets with chamfer costs in tenths of a voxel.
struct Nb {
    int dx, dy, dz, cost;
};
const std::vector<Nb>& neighbours()
{
    static const std::vector<Nb> n = [] {
        std::vector<Nb> v;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy && !dz)
                        continue;
                    int m = std::abs(dx) + std::abs(dy) + std::abs(dz);
                    v.push_back({ dx, dy, dz, m == 1 ? 10 : (m == 2 ? 14 : 17) });
                }
        return v;
    }();
    return n;
}

} // namespace

void ChunkStore::clear()
{
    m_chunks.clear();
    m_colTop.clear();
    m_colMat.clear();
    m_dirty.clear();
    m_loaded = false;
}

void ChunkStore::adopt(const std::vector<std::unique_ptr<ChunkPool>>& pools,
                       const std::vector<int16_t>& colTop,
                       const std::vector<uint8_t>& colMat)
{
    auto t0 = std::chrono::steady_clock::now();
    clear();
    m_latN = int(WORLD / VOXEL);
    m_colTop = colTop;
    m_colMat = colMat;
    m_chunks.resize(kChunkCount);

    size_t nBricks = 0, nBoxes = 0, nChunks = 0;
    for (int ci = 0; ci < kChunkCount; ++ci) {
        const ChunkPool* p = ci < int(pools.size()) ? pools[size_t(ci)].get() : nullptr;
        if (!p || p->root == int32_t(kEmptyHandle))
            continue;
        Chunk& c = m_chunks[size_t(ci)];
        ++nChunks;
        if (p->root == int32_t(kSolidHandle)) {
            c.state = Chunk::State::Solid;
            continue;
        }
        c.state = Chunk::State::Explicit;
        c.slotOf.assign(kBricksPerChunk, kNoBrick);
        // Walk the pool octree (chunk-local handles) and lift every brick and
        // solid sub-box into canonical sparse data.
        std::function<void(uint32_t, int, int, int, int)> walk =
            [&](uint32_t h, int lx, int ly, int lz, int side) {
                if (h == kEmptyHandle)
                    return;
                if (h == kSolidHandle) {
                    c.boxes.push_back({ int16_t(lx), int16_t(ly), int16_t(lz),
                                        int16_t(side), 2, true });
                    return;
                }
                if (handleIsBrick(h)) {
                    const uint32_t slot = uint32_t(c.bricks.size() / BRICK_WORDS);
                    const uint32_t* src = p->bricks.data() + size_t(h >> 2) * BRICK_WORDS;
                    c.bricks.insert(c.bricks.end(), src, src + BRICK_WORDS);
                    c.slotOf[blockKeyOf(lx, ly, lz)] = slot;
                    return;
                }
                const uint32_t ni = h >> 2;
                const int half = side / 2;
                for (int i = 0; i < 8; ++i) {
                    walk(p->handles[p->childBase[ni] + uint32_t(i)],
                         lx + (i & 1) * half, ly + ((i >> 1) & 1) * half,
                         lz + ((i >> 2) & 1) * half, half);
                }
            };
        walk(uint32_t(p->root), 0, 0, 0, CHUNK_N);
        nBricks += c.bricks.size() / BRICK_WORDS;
        nBoxes += c.boxes.size();
    }
    m_loaded = true;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    spdlog::info("chunk_store: adopted {} chunks, {} bricks, {} solid boxes ({:.0f} ms)",
                 nChunks, nBricks, nBoxes, ms);
}

void ChunkStore::ensureExplicit(int ci)
{
    Chunk& c = m_chunks[size_t(ci)];
    if (c.state == Chunk::State::Explicit)
        return;
    c.slotOf.assign(kBricksPerChunk, kNoBrick);
    if (c.state == Chunk::State::Solid)
        c.boxes.push_back({ 0, 0, 0, int16_t(CHUNK_N), 2, true });
    c.state = Chunk::State::Explicit;
    c.poolValid = false;
}

uint32_t ChunkStore::ensureBrick(int ci, Chunk& c, uint32_t key)
{
    if (c.slotOf[key] != kNoBrick)
        return c.slotOf[key];
    const uint32_t slot = uint32_t(c.bricks.size() / BRICK_WORDS);
    c.bricks.resize(c.bricks.size() + BRICK_WORDS, 0);
    uint32_t* words = c.bricks.data() + size_t(slot) * BRICK_WORDS;
    // Materialise the block's current cells (solid boxes / empty air) so the
    // new brick starts coherent, then the caller overwrites its target cell.
    const int gx0 = (ci % kChunkGridN) * CHUNK_N;
    const int gz0 = (ci / (kChunkGridN * kChunkGridN)) * CHUNK_N;
    const int bx = int(key & 7) * BRICK_N, by = int((key >> 3) & 7) * BRICK_N,
              bz = int((key >> 6) & 7) * BRICK_N;
    for (int z = 0; z < BRICK_N; ++z)
        for (int y = 0; y < BRICK_N; ++y)
            for (int x = 0; x < BRICK_N; ++x) {
                const int lx = bx + x, ly = by + y, lz = bz + z;
                StoreCell cell; // default air
                for (const SolidBox& b : c.boxes) {
                    if (lx >= b.x && lx < b.x + b.side && ly >= b.y &&
                        ly < b.y + b.side && lz >= b.z && lz < b.z + b.side) {
                        cell.solid = true;
                        cell.sdfRaw = -1;
                        if (b.terrain)
                            cell.mat = m_colMat[size_t(gz0 + lz) * m_latN +
                                                size_t(gx0 + lx)];
                        else
                            cell.mat = b.mat;
                        paletteOf(cell.mat, cell.r, cell.g, cell.b, cell.reflectivity,
                                  cell.roughness);
                        break;
                    }
                }
                encodeCell(words + cellIndexInBrick(lx, ly, lz) * 2, cell);
            }
    c.slotOf[key] = slot;
    return slot;
}

void ChunkStore::decodeCell(const Chunk& c, uint32_t slot, int lx, int ly, int lz,
                            StoreCell& out) const
{
    const uint32_t* w = c.bricks.data() + size_t(slot) * BRICK_WORDS +
                        size_t(cellIndexInBrick(lx, ly, lz)) * 2;
    const int8_t raw = int8_t((w[0] >> 24) & 0xFFu);
    // raw == 0 counts as solid: the bake truncates int(d/VOXEL), so surface
    // cells can quantise to 0 and the SVO DDA tests `sdf <= 0` as a hit
    out.solid = raw <= 0;
    out.obj = ((w[1] >> 24) & 0x80u) != 0;
    out.sdfRaw = raw;
    out.r = uint8_t(w[0] & 0xFFu);
    out.g = uint8_t((w[0] >> 8) & 0xFFu);
    out.b = uint8_t((w[0] >> 16) & 0xFFu);
    out.a = uint8_t(w[1] & 0xFFu);
    out.reflectivity = uint8_t((w[1] >> 8) & 0xFFu);
    out.roughness = uint8_t((w[1] >> 16) & 0xFFu);
    out.mat = uint8_t((w[1] >> 24) & 0x7Fu);
    // word1 byte 0 is the per-cell texture override (layered_world encodes the
    // record's reserved byte there); it rides StoreCell::tags into the store
    // surfel path's tan_aspect.w
    out.tags = out.a;
}

void ChunkStore::encodeCell(uint32_t* words, const StoreCell& cell)
{
    const uint8_t sdfByte = uint8_t(int8_t(std::clamp<int>(cell.sdfRaw, -127, 127)));
    words[0] = uint32_t(cell.r) | (uint32_t(cell.g) << 8) | (uint32_t(cell.b) << 16) |
               (uint32_t(sdfByte) << 24);
    // word1 byte 0 is the texture override slot (tags); `a` and tags are the
    // same byte on purpose (see decodeCell)
    words[1] = uint32_t(cell.tags) | (uint32_t(cell.reflectivity) << 8) |
               (uint32_t(cell.roughness) << 16) |
               ((uint32_t(cell.mat) | (cell.obj ? 0x80u : 0u)) << 24);
}

StoreCell ChunkStore::cellAt(int x, int y, int z) const
{
    StoreCell out;
    if (x < 0 || y < 0 || z < 0 || x >= m_latN || y >= m_latN || z >= m_latN)
        return out;
    const int ci = chunkIndexOfCell(x, y, z);
    if (ci < 0 || ci >= int(m_chunks.size()))
        return out;
    const Chunk& c = m_chunks[size_t(ci)];
    if (c.state == Chunk::State::Empty)
        return out;
    const int lx = x & (CHUNK_N - 1), ly = y & (CHUNK_N - 1), lz = z & (CHUNK_N - 1);
    if (c.state == Chunk::State::Solid) {
        out.solid = true;
        out.sdfRaw = -127;
        out.mat = m_colMat.empty() ? 2 : m_colMat[size_t(z) * m_latN + size_t(x)];
        paletteOf(out.mat, out.r, out.g, out.b, out.reflectivity, out.roughness);
        return out;
    }
    const uint32_t slot = c.slotOf[blockKeyOf(lx, ly, lz)];
    if (slot != kNoBrick) {
        decodeCell(c, slot, lx, ly, lz, out);
        return out;
    }
    for (const SolidBox& b : c.boxes) {
        if (lx >= b.x && lx < b.x + b.side && ly >= b.y && ly < b.y + b.side &&
            lz >= b.z && lz < b.z + b.side) {
            out.solid = true;
            out.sdfRaw = -1;
            out.mat = b.terrain ? m_colMat[size_t(z) * m_latN + size_t(x)] : b.mat;
            paletteOf(out.mat, out.r, out.g, out.b, out.reflectivity, out.roughness);
            return out;
        }
    }
    return out;
}

VoxelField::Sample ChunkStore::sample(int x, int y, int z) const
{
    VoxelField::Sample s;
    const StoreCell c = cellAt(x, y, z);
    s.d = float(c.sdfRaw) * VOXEL;
    s.mat = c.mat;
    s.tex = c.tags;
    s.obj = c.obj;
    if (c.obj && m_layerSource) {
        const VoxelField::Sample source = m_layerSource->sample(x, y, z);
        if (source.obj)
            s.layer = source.layer;
    }
    return s;
}

VoxelField::Sample ChunkStore::sampleWorld(glm::vec3 p) const
{
    return sample(int(std::floor((p.x + 0.5f * WORLD) / VOXEL)),
                  int(std::floor((p.y + 0.5f * WORLD) / VOXEL)),
                  int(std::floor((p.z + 0.5f * WORLD) / VOXEL)));
}

void ChunkStore::markDirty(int ci, int x, int y, int z)
{
    Chunk& c = m_chunks[size_t(ci)];
    c.lo[0] = std::min(c.lo[0], x);
    c.lo[1] = std::min(c.lo[1], y);
    c.lo[2] = std::min(c.lo[2], z);
    c.hi[0] = std::max(c.hi[0], x);
    c.hi[1] = std::max(c.hi[1], y);
    c.hi[2] = std::max(c.hi[2], z);
    if (!c.dirty) {
        c.dirty = true;
        m_dirty.push_back(ci);
    }
    c.poolValid = false;
}

void ChunkStore::apply(const StoreEdit& e)
{
    if (e.x < 0 || e.y < 0 || e.z < 0 || e.x >= m_latN || e.y >= m_latN ||
        e.z >= m_latN)
        return;
    const int ci = chunkIndexOfCell(e.x, e.y, e.z);
    ensureExplicit(ci);
    Chunk& c = m_chunks[size_t(ci)];
    const int lx = e.x & (CHUNK_N - 1), ly = e.y & (CHUNK_N - 1),
              lz = e.z & (CHUNK_N - 1);
    const uint32_t slot = ensureBrick(ci, c, blockKeyOf(lx, ly, lz));
    uint32_t* w = c.bricks.data() + size_t(slot) * BRICK_WORDS +
                  size_t(cellIndexInBrick(lx, ly, lz)) * 2;
    StoreCell cell;
    cell.sdfRaw = int8_t((w[0] >> 24) & 0xFFu);
    cell.solid = cell.sdfRaw <= 0; // raw == 0 is solid (see decodeCell)
    cell.obj = ((w[1] >> 24) & 0x80u) != 0;
    cell.r = uint8_t(w[0] & 0xFFu);
    cell.g = uint8_t((w[0] >> 8) & 0xFFu);
    cell.b = uint8_t((w[0] >> 16) & 0xFFu);
    cell.a = uint8_t(w[1] & 0xFFu);
    cell.reflectivity = uint8_t((w[1] >> 8) & 0xFFu);
    cell.roughness = uint8_t((w[1] >> 16) & 0xFFu);
    cell.mat = uint8_t((w[1] >> 24) & 0x7Fu);
    cell.tags = cell.a; // word1 byte 0 = per-cell texture override (see decodeCell)

    switch (e.mode) {
    case StoreEdit::Mode::Set:
        cell.solid = true;
        cell.sdfRaw = -1;
        cell.obj = !e.terrain; // live adds are objects; terrain sculpt stays terrain
        cell.mat = e.mat;
        cell.tags = e.tags;
        if (e.hasColor) {
            cell.r = e.r; cell.g = e.g; cell.b = e.b;
            cell.reflectivity = e.reflectivity;
            cell.roughness = e.roughness;
        } else {
            paletteOf(cell.mat, cell.r, cell.g, cell.b, cell.reflectivity,
                      cell.roughness);
        }
        break;
    case StoreEdit::Mode::Clear:
        cell.solid = false;
        cell.sdfRaw = int8_t(kFar);
        cell.obj = false;
        cell.tags = 0; // cleared cells fall back to the material's atlas slot
        break;
    case StoreEdit::Mode::Paint:
        cell.mat = e.mat;
        cell.tags = e.tags;
        if (e.hasColor) {
            cell.r = e.r; cell.g = e.g; cell.b = e.b;
            cell.reflectivity = e.reflectivity;
            cell.roughness = e.roughness;
        } else {
            paletteOf(cell.mat, cell.r, cell.g, cell.b, cell.reflectivity,
                      cell.roughness);
        }
        break;
    }
    encodeCell(w, cell);
    c.edited = true; // persisted in the async store overlay
    markDirty(ci, e.x, e.y, e.z);

    // The SDF band can cross into a neighbour chunk when the edit is within
    // kFar cells of a face; mark those neighbours dirty too.
    int ccx, ccy, ccz;
    chunkCoordsOf(ci, ccx, ccy, ccz);
    if (lx < kLiveBand && ccx > 0)
        markDirty(chunkIndexOf(ccx - 1, ccy, ccz), e.x, e.y, e.z);
    if (lx >= CHUNK_N - kLiveBand && ccx < kChunkGridN - 1)
        markDirty(chunkIndexOf(ccx + 1, ccy, ccz), e.x, e.y, e.z);
    if (ly < kLiveBand && ccy > 0)
        markDirty(chunkIndexOf(ccx, ccy - 1, ccz), e.x, e.y, e.z);
    if (ly >= CHUNK_N - kLiveBand && ccy < kChunkGridN - 1)
        markDirty(chunkIndexOf(ccx, ccy + 1, ccz), e.x, e.y, e.z);
    if (lz < kLiveBand && ccz > 0)
        markDirty(chunkIndexOf(ccx, ccy, ccz - 1), e.x, e.y, e.z);
    if (lz >= CHUNK_N - kLiveBand && ccz < kChunkGridN - 1)
        markDirty(chunkIndexOf(ccx, ccy, ccz + 1), e.x, e.y, e.z);
}

void ChunkStore::apply(const std::vector<StoreEdit>& edits)
{
    for (const StoreEdit& e : edits)
        apply(e);
}

SmoothTerrainEdits ChunkStore::makeSmoothEdits(glm::ivec3 center,
                                               float radiusM,
                                               float strength) const
{
    SmoothTerrainEdits result;
    if (!m_loaded || m_latN <= 0 || !std::isfinite(radiusM) ||
        !std::isfinite(strength) || radiusM <= 0.0f || strength <= 0.0f)
        return result;

    // The UI's useful range is already modest; cap the public helper as well
    // so a malformed headless value cannot turn one click into a million-cell
    // column scan.
    radiusM = std::min(radiusM, 16.0f);
    strength = std::clamp(strength, 0.0f, 1.0f);

    const int cx = std::clamp(center.x, 0, m_latN - 1);
    const int cy = std::clamp(center.y, 0, m_latN - 1);
    const int cz = std::clamp(center.z, 0, m_latN - 1);
    const int radiusCells = std::max(1, int(std::ceil(radiusM / VOXEL)));

    // A picked object is smoothed with the SAME relaxation the terrain path
    // below uses, generalised to an arbitrary surface axis. The axis and the
    // outward side come from the picked cell's exposed faces (the store SDF
    // gradient only breaks the two-sided tie of a thin wall), so a roof is a Y
    // surface and a wall is an X or Z surface, and the footprint is always the
    // 2D plane of that surface: one click costs O(pi r^2) surface samples
    // instead of an O(r^3) volume scan.
    //
    // Relaxing a surface POSITION rather than filtering per-voxel occupancy is
    // what makes this exact for splats. A bump is removed in a single stamp
    // (the span from the neighbour level down to the bump is cleared as one
    // contiguous run, so no unsupported floater is left behind), a notch is
    // filled, and a flat face, a 1-cell wall, a post or a staircase is its own
    // neighbourhood average and therefore survives untouched instead of being
    // eroded. The min/max clamp is the same fixed point the terrain path has,
    // so a second stamp over the same spot is a no-op. Only object cells are
    // ever cleared and every fill is an object cell, so terrain columns, the
    // height texture and the water bed are never touched.
    const StoreCell picked = cellAt(cx, cy, cz);
    if (picked.solid && picked.obj) {
        result.objectSurface = true;
        static constexpr int kUnit[3][3] = { { 1, 0, 0 }, { 0, 1, 0 },
                                             { 0, 0, 1 } };
        const int centre[3] = { cx, cy, cz };

        // Surface axis. A face the surface can be relaxed along is ONE-SIDED:
        // solid on one side of the cell, air on the other. An axis that is air
        // on BOTH sides is only a 1-cell cross-section (the rod of a post, the
        // thickness of a wall), never a face, so it is kept purely as a
        // fallback. Counting exposed faces instead would make a 1x1 spike look
        // like a wall in X and Z and smooth the wrong plane. Among one-sided
        // axes the strongest SDF gradient wins, which picks the flat face of a
        // corner over its bevel.
        int axis = -1;
        float bestFace = 0.0f;
        int thinAxis = -1;
        float bestThin = -1.0f;
        int openAxes = 0;
        for (int a = 0; a < 3; ++a) {
            const bool posAir = !cellAt(cx + kUnit[a][0], cy + kUnit[a][1],
                                        cz + kUnit[a][2]).solid;
            const bool negAir = !cellAt(cx - kUnit[a][0], cy - kUnit[a][1],
                                        cz - kUnit[a][2]).solid;
            if (!posAir && !negAir)
                continue; // enclosed along this axis
            ++openAxes;
            const float grad = std::abs(
                sample(cx + kUnit[a][0], cy + kUnit[a][1], cz + kUnit[a][2]).d -
                sample(cx - kUnit[a][0], cy - kUnit[a][1], cz - kUnit[a][2]).d);
            if (posAir != negAir) {
                if (grad > bestFace) {
                    bestFace = grad;
                    axis = a;
                }
            } else if (grad > bestThin) {
                bestThin = grad;
                thinAxis = a;
            }
        }
        // The thin fallback needs exactly one open axis: that is a wall face
        // (thin across the wall, enclosed along the wall plane). A diagonal or
        // rod is thin across two or three axes and has no surface plane, so it
        // is left alone - a 45-degree staircase must not be faired into a
        // zigzag.
        if (axis < 0 && openAxes == 1)
            axis = thinAxis;
        if (axis < 0) {
            if (getenv("VF_TRACE"))
                spdlog::info("smooth object: center=({},{},{}) axis=none "
                             "(interior or rod), no relaxation",
                             center.x, center.y, center.z);
            return result; // interior or rod geometry: nothing to relax
        }

        // Outward side. A one-sided surface is unambiguous; for a thin wall,
        // where both sides are exposed, the SDF gradient points the same way
        // the surfel normals do.
        const int sign =
            (sample(cx + kUnit[axis][0], cy + kUnit[axis][1],
                    cz + kUnit[axis][2]).d -
             sample(cx - kUnit[axis][0], cy - kUnit[axis][1],
                    cz - kUnit[axis][2]).d) >= 0.0f
                ? +1
                : -1;

        // The surface plane: u/v are the two axes perpendicular to the chosen
        // one, so the circular footprint lives in that plane.
        //   axis X -> (u,v) = (Y,Z)   axis Y -> (X,Z)   axis Z -> (X,Y)
        const int uAxis = (axis == 0) ? 1 : 0;
        const int vAxis = (axis == 2) ? 1 : 2;
        auto planeCell = [&](int u, int s, int v) {
            int p[3] = { 0, 0, 0 };
            p[uAxis] = u;
            p[axis] = s;
            p[vAxis] = v;
            return cellAt(p[0], p[1], p[2]);
        };
        // A surface cell of this row: object, with air on the outward side.
        auto isSurface = [&](int u, int s, int v) {
            const StoreCell c = planeCell(u, s, v);
            if (!c.solid || !c.obj)
                return false;
            return !planeCell(u, s + sign, v).solid;
        };

        // A SHORT object run that RESTS ON SOLID GROUND is a bump, not a
        // feature. Its lateral rows carry no object surface of their own, so
        // they contribute the supported ground level instead, and one stamp
        // settles the bump onto that ground. A tall post is longer than the
        // bump run and an overhanging slab edge is not grounded, so both keep
        // their height. Only an upward-facing Y surface has ground beside it;
        // walls and ceilings do not.
        constexpr int kBumpRunCells = 4;
        bool bumpMode = false;
        if (axis == 1 && sign > 0) {
            int run = 0;
            int s = centre[axis];
            while (s >= 0 && planeCell(centre[uAxis], s, centre[vAxis]).obj) {
                ++run;
                --s;
            }
            bumpMode = run > 0 && run <= kBumpRunCells && s >= 0 &&
                       planeCell(centre[uAxis], s, centre[vAxis]).solid;
        }

        // A usable surface sample: either an object surface, or - for a bump
        // that rests on the ground - the supported ground level of that row.
        struct SurfaceColumn {
            bool initialized = false;
            bool valid = false;
            bool ground = false;
            int pos = -1;
            StoreCell surface {};
        };
        const int cu = centre[uAxis];
        const int cv = centre[vAxis];
        const int u0 = std::max(0, cu - radiusCells - 1);
        const int u1 = std::min(m_latN - 1, cu + radiusCells + 1);
        const int v0 = std::max(0, cv - radiusCells - 1);
        const int v1 = std::min(m_latN - 1, cv + radiusCells + 1);
        const int nu = u1 - u0 + 1;
        const int nv = v1 - v0 + 1;
        std::vector<SurfaceColumn> columns(size_t(nu) * size_t(nv));

        auto findSurface = [&](int u, int v) {
            SurfaceColumn out;
            const int s0 = std::clamp(centre[axis], 0, m_latN - 1);
            // Local walk first: the rows of a surface around the pick sit within
            // a few cells of the picked axis coordinate, so this keeps a click
            // in the sub-millisecond range instead of 1024 taps per row.
            constexpr int kWalk = 24;
            for (int k = 0; k <= kWalk; ++k) {
                for (int dir = 0; dir < 2; ++dir) {
                    const int s = s0 + (dir == 0 ? sign : -sign) * k;
                    if (s < 0 || s > m_latN - 1)
                        continue;
                    if (isSurface(u, s, v)) {
                        out.valid = true;
                        out.pos = s;
                        out.surface = planeCell(u, s, v);
                        return out;
                    }
                }
            }
            // Full fallback, walked against the outward side so a surface far
            // from the pick is still found.
            for (int s = (sign > 0 ? m_latN - 1 : 0); s >= 0 && s < m_latN;
                 s -= sign) {
                if (isSurface(u, s, v)) {
                    out.valid = true;
                    out.pos = s;
                    out.surface = planeCell(u, s, v);
                    return out;
                }
            }
            // Bump fallback: this row has no object surface, so stand in the
            // supported ground under the pick. Ground rows are samples only,
            // never targets, so an object brush never sculpts terrain.
            if (bumpMode) {
                for (int s = s0; s >= 0; --s) {
                    const StoreCell g = planeCell(u, s, v);
                    if (g.solid && !g.obj) {
                        out.valid = true;
                        out.ground = true;
                        out.pos = s;
                        out.surface = g;
                        return out;
                    }
                }
            }
            return out;
        };

        auto surfaceAt = [&](int u, int v) -> const SurfaceColumn& {
            static const SurfaceColumn invalid;
            if (u < u0 || u > u1 || v < v0 || v > v1)
                return invalid;
            SurfaceColumn& c = columns[size_t(v - v0) * size_t(nu) +
                                         size_t(u - u0)];
            if (!c.initialized) {
                c = findSurface(u, v);
                c.initialized = true;
            }
            return c;
        };

        static constexpr int kNeighbour[8][2] = {
            { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
            { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
        };

        result.edits.reserve(size_t(radiusCells * radiusCells) * 2u);
        int validRows = 0;
        int sampledRows = 0;
        int observedMin = m_latN;
        int observedMax = -1;
        for (int v = v0; v <= v1; ++v) {
            for (int u = u0; u <= u1; ++u) {
                const float du = float(u - cu) * VOXEL;
                const float dv = float(v - cv) * VOXEL;
                const float distance = std::sqrt(du * du + dv * dv);
                if (distance > radiusM)
                    continue;
                const SurfaceColumn& current = surfaceAt(u, v);
                if (!current.valid || current.ground)
                    continue; // ground rows are samples, never targets
                ++validRows;
                observedMin = std::min(observedMin, current.pos);
                observedMax = std::max(observedMax, current.pos);

                float sum = 0.0f;
                float weightSum = 0.0f;
                int minPos = current.pos;
                int maxPos = current.pos;
                // Ground may stand in for a missing object surface only when
                // the whole 3x3 ring is bare. If any lateral row carries an
                // object surface - the adjacent step of a staircase, the rest
                // of a wall top - that surface is the neighbourhood and the
                // ground must not drag the feature down.
                bool anyObjectNeighbour = false;
                for (const auto& d : kNeighbour) {
                    const SurfaceColumn& s = surfaceAt(u + d[0], v + d[1]);
                    if (s.valid && !s.ground) {
                        anyObjectNeighbour = true;
                        break;
                    }
                }
                for (const auto& d : kNeighbour) {
                    const SurfaceColumn& sampleCol =
                        surfaceAt(u + d[0], v + d[1]);
                    if (!sampleCol.valid)
                        continue;
                    if (sampleCol.ground && anyObjectNeighbour)
                        continue;
                    const float w = (d[0] == 0 || d[1] == 0) ? 1.0f : 0.5f;
                    // Ground stands in for a missing object surface, but at
                    // half weight so object-to-object smoothing dominates.
                    const float gw = sampleCol.ground ? 0.5f * w : w;
                    sum += float(sampleCol.pos) * gw;
                    weightSum += gw;
                    minPos = std::min(minPos, sampleCol.pos);
                    maxPos = std::max(maxPos, sampleCol.pos);
                }
                // A surface with no lateral object neighbour (a post, a lone
                // 1-cell column) has nothing to average against: keep it.
                if (weightSum <= 0.0f)
                    continue;
                ++sampledRows;

                const float q =
                    std::clamp(1.0f - distance / radiusM, 0.0f, 1.0f);
                const float falloff = q * q * (3.0f - 2.0f * q);
                const float relaxed = float(current.pos) +
                    (sum / weightSum - float(current.pos)) * strength * falloff;
                int target = std::clamp(int(std::lround(relaxed)), minPos, maxPos);
                target = std::clamp(target, 0, m_latN - 1);
                if (target == current.pos)
                    continue;

                if (target > current.pos) {
                    // Never raise object geometry into terrain or through
                    // another object: the whole row is left alone instead.
                    bool blocked = false;
                    for (int s = current.pos + 1; s <= target; ++s) {
                        const StoreCell c = planeCell(u, s, v);
                        if (c.solid && !c.obj) {
                            blocked = true;
                            break;
                        }
                    }
                    if (blocked)
                        continue;
                    for (int s = current.pos + 1; s <= target; ++s) {
                        if (planeCell(u, s, v).solid)
                            continue; // already object geometry
                        StoreEdit e;
                        int p[3] = { 0, 0, 0 };
                        p[uAxis] = u;
                        p[axis] = s;
                        p[vAxis] = v;
                        e.x = p[0];
                        e.y = p[1];
                        e.z = p[2];
                        e.mode = StoreEdit::Mode::Set;
                        // Object-owned fill: never terrain, so the height
                        // texture and the water bed stay correct.
                        e.terrain = false;
                        e.mat = current.surface.mat;
                        e.tags = current.surface.tags;
                        e.r = current.surface.r;
                        e.g = current.surface.g;
                        e.b = current.surface.b;
                        e.reflectivity = current.surface.reflectivity;
                        e.roughness = current.surface.roughness;
                        e.hasColor = true;
                        result.edits.push_back(e);
                    }
                } else {
                    // Lower: clear the whole object run down to the new surface
                    // level. Terrain inside the span is left alone, so a bump
                    // sitting on ground settles onto that ground.
                    for (int s = target + 1; s <= current.pos; ++s) {
                        const StoreCell c = planeCell(u, s, v);
                        if (!c.solid || !c.obj)
                            continue;
                        StoreEdit e;
                        int p[3] = { 0, 0, 0 };
                        p[uAxis] = u;
                        p[axis] = s;
                        p[vAxis] = v;
                        e.x = p[0];
                        e.y = p[1];
                        e.z = p[2];
                        e.mode = StoreEdit::Mode::Clear;
                        result.edits.push_back(e);
                    }
                }
            }
        }
        if (getenv("VF_TRACE")) {
            spdlog::info("smooth object: center=({},{},{}) radius={:.2f} "
                         "strength={:.2f} axis={} sign={:+d} bump={} rows={} "
                         "sampled={} posRange=[{},{}] edits={}",
                         center.x, center.y, center.z, radiusM, strength, axis,
                         sign, bumpMode, validRows, sampledRows, observedMin,
                         observedMax, result.edits.size());
        }
        return result;
    }

    // Include one ring outside the footprint for the height samples. The
    // targets themselves are still restricted to the circular brush below.
    const int x0 = std::max(0, cx - radiusCells - 1);
    const int x1 = std::min(m_latN - 1, cx + radiusCells + 1);
    const int z0 = std::max(0, cz - radiusCells - 1);
    const int z1 = std::min(m_latN - 1, cz + radiusCells + 1);
    const int nx = x1 - x0 + 1;
    const int nz = z1 - z0 + 1;

    struct Column {
        bool initialized = false;
        bool valid = false;
        int top = -1;
        StoreCell surface {};
    };
    std::vector<Column> columns(size_t(nx) * size_t(nz));

    auto findColumn = [&](int x, int z) {
        Column out;
        if (x < 0 || z < 0 || x >= m_latN || z >= m_latN)
            return out;

        // The adopted column top is an excellent first guess and remains close
        // to the live surface after ordinary edits. Search a generous band on
        // both sides first; the full scan fallback handles unusual tall edits
        // without making the common terrain brush pay for 1024 taps/column.
        int guess = std::clamp(center.y, 0, m_latN - 1);
        if (m_colTop.size() == size_t(m_latN) * size_t(m_latN)) {
            const int16_t baseTop = m_colTop[size_t(z) * size_t(m_latN) + size_t(x)];
            // A column with no landscape record is not a terrain-sculpt target;
            // in particular, do not mistake a promoted Solid underground chunk
            // for an editable surface.
            if (baseTop < 0)
                return out;
            guess = std::clamp(int(baseTop), 0, m_latN - 1);
        }
        constexpr int kSearchBand = 192;
        auto tryCell = [&](int y, Column& target) {
            const StoreCell c = cellAt(x, y, z);
            if (!c.solid)
                return false;
            // An object owns the top of its column. Do not turn an object into
            // terrain, and do not use an object roof as a terrain height sample.
            if (c.obj) {
                target.valid = false;
                target.top = -1;
                return true; // blocked: stop rather than sampling below it
            }
            target.valid = true;
            target.top = y;
            target.surface = c;
            return true;
        };

        // A raised terrain column is contiguous above the baked top, so do
        // not return the first Set cell above `guess`: track the highest solid
        // in the fast band and only accept it after an air gap proves that the
        // surface ended. If the band ends while still solid, the raise may
        // exceed it; the true top-down fallback below resolves that rare case.
        Column raised;
        bool haveRaised = false;
        bool airAfterRaised = false;
        const int upHi = std::min(m_latN - 1, guess + kSearchBand);
        for (int y = std::min(m_latN - 1, guess + 1); y <= upHi; ++y) {
            Column candidate;
            if (tryCell(y, candidate)) {
                if (!candidate.valid)
                    return candidate;
                raised = candidate;
                haveRaised = true;
                airAfterRaised = false;
            } else if (haveRaised) {
                airAfterRaised = true;
            }
        }
        if (haveRaised) {
            if (airAfterRaised)
                return raised;
            for (int y = m_latN - 1; y >= 0; --y) {
                Column global;
                if (!tryCell(y, global))
                    continue;
                if (!global.valid || global.top > raised.top)
                    return global;
                break;
            }
            return raised;
        }

        // No live surface above the adopted guess. Walk down to find a lowered
        // terrain top, still stopping at an object rather than sampling below
        // it. A very large drop outside this band uses the full fallback.
        for (int y = guess; y >= std::max(0, guess - kSearchBand); --y) {
            Column lower;
            if (tryCell(y, lower))
                return lower;
        }
        for (int y = m_latN - 1; y >= 0; --y) {
            Column global;
            if (tryCell(y, global))
                return global;
        }
        return out;
    };

    auto columnAt = [&](int x, int z) -> const Column& {
        if (x < x0 || x > x1 || z < z0 || z > z1) {
            static const Column invalid;
            return invalid;
        }
        Column& c = columns[size_t(z - z0) * size_t(nx) + size_t(x - x0)];
        if (!c.initialized) {
            c = findColumn(x, z);
            c.initialized = true;
        }
        return c;
    };

    static constexpr int kNeighbour[8][2] = {
        { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
        { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
    };

    result.edits.reserve(size_t(radiusCells * radiusCells) * 2u);
    int validColumns = 0;
    int sampledColumns = 0;
    int observedMin = m_latN;
    int observedMax = -1;
    int observedMinX = cx;
    int observedMinZ = cz;
    int observedMaxX = cx;
    int observedMaxZ = cz;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x - cx) * VOXEL;
            const float dz = float(z - cz) * VOXEL;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > radiusM)
                continue;
            const Column& current = columnAt(x, z);
            if (!current.valid)
                continue;
            ++validColumns;
            if (current.top < observedMin) {
                observedMin = current.top;
                observedMinX = x;
                observedMinZ = z;
            }
            if (current.top > observedMax) {
                observedMax = current.top;
                observedMaxX = x;
                observedMaxZ = z;
            }

            float sum = 0.0f;
            float weightSum = 0.0f;
            int minTop = current.top;
            int maxTop = current.top;
            for (const auto& d : kNeighbour) {
                const int nxSample = x + d[0];
                const int nzSample = z + d[1];
                const Column& sample = columnAt(nxSample, nzSample);
                if (!sample.valid)
                    continue;
                const float w = (d[0] == 0 || d[1] == 0) ? 1.0f : 0.5f;
                sum += float(sample.top) * w;
                weightSum += w;
                minTop = std::min(minTop, sample.top);
                maxTop = std::max(maxTop, sample.top);
            }
            if (weightSum <= 0.0f)
                continue;
            ++sampledColumns;

            float q = std::clamp(1.0f - distance / radiusM, 0.0f, 1.0f);
            const float falloff = q * q * (3.0f - 2.0f * q);
            const float relaxed = float(current.top) +
                (sum / weightSum - float(current.top)) * strength * falloff;
            if (getenv("VF_TRACE") && x == cx && z == cz) {
                spdlog::info("smooth centre sample: top={} avg={:.3f} min={} max={} "
                             "falloff={:.3f} relaxed={:.3f}",
                             current.top, sum / weightSum, minTop, maxTop,
                             falloff, relaxed);
            }
            int target = std::clamp(int(std::lround(relaxed)), minTop, maxTop);
            target = std::clamp(target, 0, m_latN - 1);
            if (target == current.top)
                continue;

            // Never raise through or lower past an object in the affected
            // vertical span. This keeps smoothing a terrain pass from
            // swallowing props/trees that share the column.
            bool objectInSpan = false;
            for (int y = std::min(current.top, target);
                 y <= std::max(current.top, target); ++y) {
                const StoreCell c = cellAt(x, y, z);
                if (c.solid && c.obj) {
                    objectInSpan = true;
                    break;
                }
            }
            if (objectInSpan)
                continue;

            if (target > current.top) {
                result.riseCells = std::max(result.riseCells,
                                            target - current.top);
                for (int y = current.top + 1; y <= target; ++y) {
                    StoreEdit e;
                    e.x = x;
                    e.y = y;
                    e.z = z;
                    e.mode = StoreEdit::Mode::Set;
                    e.terrain = true;
                    e.mat = current.surface.mat;
                    e.tags = current.surface.tags;
                    e.r = current.surface.r;
                    e.g = current.surface.g;
                    e.b = current.surface.b;
                    e.reflectivity = current.surface.reflectivity;
                    e.roughness = current.surface.roughness;
                    e.hasColor = true;
                    result.edits.push_back(e);
                }
            } else {
                for (int y = target + 1; y <= current.top; ++y) {
                    StoreEdit e;
                    e.x = x;
                    e.y = y;
                    e.z = z;
                    e.mode = StoreEdit::Mode::Clear;
                    result.edits.push_back(e);
                }
            }
        }
    }
    if (getenv("VF_TRACE")) {
        const StoreCell centre = cellAt(cx, std::clamp(center.y, 0, m_latN - 1), cz);
        spdlog::info("smooth terrain: center=({},{},{}) radius={:.2f} strength={:.2f} "
                     "centreCell=(solid={}, obj={}, mat={}) valid={} sampled={} "
                     "topRange=[{},{}] at=({}, {})..({}, {}) edits={} rise={}",
                     center.x, center.y, center.z, radiusM, strength,
                     centre.solid, centre.obj, int(centre.mat), validColumns,
                     sampledColumns, observedMin, observedMax,
                     observedMinX, observedMinZ, observedMaxX, observedMaxZ,
                     result.edits.size(), result.riseCells);
    }
    return result;
}

bool ChunkStore::chunkDirty(int ci) const
{
    return ci >= 0 && ci < int(m_chunks.size()) && m_chunks[size_t(ci)].dirty;
}

// ---------------------------------------------------------------------------
// Rebuild: dense materialisation -> local signed distance band -> sparse
// canonical data -> octree pool.
// ---------------------------------------------------------------------------
void ChunkStore::rebuildChunk(int ci)
{
    Chunk& c = m_chunks[size_t(ci)];
    if (c.state == Chunk::State::Empty) {
        c.dirty = false;
        return;
    }
    if (c.state == Chunk::State::Solid) {
        // Uniform underground fill: nothing to re-derive, and an edit that
        // landed inside would have converted the chunk to Explicit in apply().
        // Neighbours marked dirty near a face hit this path.
        c.dirty = false;
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    const int ccx = ci % kChunkGridN, ccy = (ci / kChunkGridN) % kChunkGridN,
              ccz = ci / (kChunkGridN * kChunkGridN);
    const int gx0 = ccx * CHUNK_N, gy0 = ccy * CHUNK_N, gz0 = ccz * CHUNK_N;

    // Rebuild region: the edit AABB (global lattice coords) expanded by
    // kLiveBand, clamped to the chunk and snapped to 8^3 block boundaries so
    // every block is fully inside or outside (the dense scratch arrays then
    // stay coherent at the borders). Only this region is re-materialised,
    // re-transformed and re-derived; untouched blocks keep their canonical
    // bytes verbatim.
    int rlx = 0, rly = 0, rlz = 0, rhx = CHUNK_N, rhy = CHUNK_N, rhz = CHUNK_N;
    if (c.lo[0] <= c.hi[0]) {
        auto snapLo = [](int v) {
            const int s = v - kLiveBand;
            return (s >= 0 ? s / BRICK_N : -(((-s) + BRICK_N - 1) / BRICK_N)) * BRICK_N;
        };
        auto snapHi = [](int v) { return ((v + kLiveBand) / BRICK_N + 1) * BRICK_N; };
        const int glx0 = std::max(gx0, snapLo(c.lo[0]));
        const int gly0 = std::max(gy0, snapLo(c.lo[1]));
        const int glz0 = std::max(gz0, snapLo(c.lo[2]));
        const int glx1 = std::min(gx0 + CHUNK_N, snapHi(c.hi[0]));
        const int gly1 = std::min(gy0 + CHUNK_N, snapHi(c.hi[1]));
        const int glz1 = std::min(gz0 + CHUNK_N, snapHi(c.hi[2]));
        rlx = std::max(0, glx0 - gx0);
        rly = std::max(0, gly0 - gy0);
        rlz = std::max(0, glz0 - gz0);
        rhx = std::min(CHUNK_N, glx1 - gx0);
        rhy = std::min(CHUNK_N, gly1 - gy0);
        rhz = std::min(CHUNK_N, glz1 - gz0);
    }
    auto blockOutside = [&](int bx, int by, int bz) {
        return bx + BRICK_N <= rlx || bx >= rhx || by + BRICK_N <= rly ||
               by >= rhy || bz + BRICK_N <= rlz || bz >= rhz;
    };

    // --- dense materialisation ---------------------------------------------
    // kind: 0 air, 1 solid from box/state (terrain/uniform), 2 explicit solid
    std::vector<uint8_t> kind(size_t(CHUNK_N) * CHUNK_N * CHUNK_N, 0);
    // cells that a brick defines (air or solid): bricks win over overlapping
    // SolidBoxes for BOTH signs, so a box must never re-solidify a cell an
    // edit cleared (the local rebuild has to match a full rebuild either way)
    std::vector<uint8_t> fromBrick(kind.size(), 0);
    std::vector<uint32_t> words(size_t(CHUNK_N) * CHUNK_N * CHUNK_N * 2, 0);
    auto di = [&](int lx, int ly, int lz) {
        return (size_t(lz) * CHUNK_N + size_t(ly)) * CHUNK_N + size_t(lx);
    };
    // Only Explicit chunks carry a slot table; Solid chunks (promoted terrain
    // interiors) have none, and neighbours can be Solid when they are marked
    // dirty near a face.
    const bool hasSlots =
        c.state == Chunk::State::Explicit && c.slotOf.size() == size_t(kBricksPerChunk);
    for (int key = 0; key < kBricksPerChunk; ++key) {
        const uint32_t s = hasSlots ? c.slotOf[size_t(key)] : kNoBrick;
        if (s == kNoBrick)
            continue;
        const int bx = (key & 7) * BRICK_N, by = ((key >> 3) & 7) * BRICK_N,
                  bz = ((key >> 6) & 7) * BRICK_N;
        if (blockOutside(bx, by, bz))
            continue; // untouched block: copied verbatim during write-back
        const uint32_t* src = c.bricks.data() + size_t(s) * BRICK_WORDS;
        for (int z = 0; z < BRICK_N; ++z)
            for (int y = 0; y < BRICK_N; ++y)
                for (int x = 0; x < BRICK_N; ++x) {
                    const size_t k = di(bx + x, by + y, bz + z);
                    const size_t si = (size_t(z) * BRICK_N + y) * BRICK_N + x;
                    words[k * 2] = src[si * 2];
                    words[k * 2 + 1] = src[si * 2 + 1];
                    fromBrick[k] = 1;
                    // raw <= 0 is solid (see decodeCell)
                    kind[k] = int8_t(words[k * 2] >> 24) <= 0 ? 2 : 0;
                }
    }
    auto fillBoxCell = [&](size_t k, int x, int z, const SolidBox& b) {
        StoreCell cell;
        cell.solid = true;
        cell.sdfRaw = -1;
        cell.a = 0; // no texture override: boxes are bulk-filled regions
        cell.mat = b.terrain ? m_colMat[size_t(z) * m_latN + size_t(x)] : b.mat;
        paletteOf(cell.mat, cell.r, cell.g, cell.b, cell.reflectivity, cell.roughness);
        encodeCell(words.data() + k * 2, cell);
    };
    for (const SolidBox& b : c.boxes) {
        const int z0 = std::max(int(b.z), rlz), z1 = std::min(int(b.z) + b.side, rhz);
        const int y0 = std::max(int(b.y), rly), y1 = std::min(int(b.y) + b.side, rhy);
        const int x0 = std::max(int(b.x), rlx), x1 = std::min(int(b.x) + b.side, rhx);
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const size_t k = di(x, y, z);
                    if (fromBrick[k])
                        continue; // bricks win over overlapping boxes
                    kind[k] = 1;
                    fillBoxCell(k, x + gx0, z + gz0, b);
                }
    }
    if (c.state == Chunk::State::Solid)
        for (int lz = rlz; lz < rhz; ++lz)
            for (int ly = rly; ly < rhy; ++ly)
                for (int lx = rlx; lx < rhx; ++lx) {
                    const size_t k = di(lx, ly, lz);
                    if (fromBrick[k])
                        continue;
                    kind[k] = 1;
                    fillBoxCell(k, lx + gx0, lz + gz0,
                                SolidBox { 0, 0, 0, int16_t(CHUNK_N), 2, true });
                }

    std::vector<uint8_t> solid(kind.size(), 0);
    for (int lz = rlz; lz < rhz; ++lz)
        for (int ly = rly; ly < rhy; ++ly)
            for (int lx = rlx; lx < rhx; ++lx) {
                const size_t k = di(lx, ly, lz);
                solid[k] = kind[k] != 0 ? 1 : 0;
            }

    // --- local signed distance band ----------------------------------------
    // Two-phase chamfer distance transform (3-4-5 weights, one forward + one
    // backward raster pass per phase) over the chunk: air cells get distance
    // to the nearest solid, solid cells the negated distance to the nearest
    // air. Seeds are sign boundaries plus the SDF stored in the neighbouring
    // chunks at the faces. O(N) with no priority queue; the ~2% metric error
    // is far below the int8 quantisation.
    const size_t nCells = kind.size();
    std::vector<int16_t> airD(nCells, int16_t(kMaxDist + 1));
    std::vector<int16_t> solD(nCells, int16_t(kMaxDist + 1));
    auto seed = [&](std::vector<int16_t>& d, size_t k, int v) {
        v = std::min(v, kMaxDist);
        if (v < d[k])
            d[k] = int16_t(v);
    };
    const int dirs6[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                              { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
    auto outsideRegion = [&](int lx, int ly, int lz) {
        return lx < rlx || lx >= rhx || ly < rly || ly >= rhy || lz < rlz ||
               lz >= rhz;
    };
    for (int lz = rlz; lz < rhz; ++lz)
        for (int ly = rly; ly < rhy; ++ly)
            for (int lx = rlx; lx < rhx; ++lx) {
                const size_t k = di(lx, ly, lz);
                const bool isSolid = solid[k] != 0;
                std::vector<int16_t>& dist = isSolid ? solD : airD;
                for (const auto& d : dirs6) {
                    const int nx = lx + d[0], ny = ly + d[1], nz = lz + d[2];
                    if (outsideRegion(nx, ny, nz)) {
                        // outside the rebuild region (or the chunk): the
                        // canonical cell (and its stored SDF) constrains the
                        // band at the border
                        const int gx = gx0 + nx, gy = gy0 + ny, gz = gz0 + nz;
                        if (gx < 0 || gy < 0 || gz < 0 || gx >= m_latN ||
                            gy >= m_latN || gz >= m_latN)
                            continue;
                        const StoreCell oc = cellAt(gx, gy, gz);
                        if (oc.solid != isSolid)
                            seed(dist, k, 10);
                        else {
                            const int od = std::abs(int(oc.sdfRaw)) * 10;
                            if (od < kMaxDist)
                                seed(dist, k, std::max(10, od + 10));
                        }
                        continue;
                    }
                    const size_t nk = di(nx, ny, nz);
                    if ((solid[nk] != 0) != isSolid)
                        seed(dist, k, 10);
                }
            }
    const std::vector<Nb>& nbs = neighbours();
    // one forward + one backward pass per phase; only same-kind cells carry
    // distances, and a pass only relaxes from neighbours earlier in its scan
    auto chamfer = [&](std::vector<int16_t>& d, bool solidPass, bool forward) {
        const int sz = forward ? 1 : -1;
        for (int lz = forward ? rlz : rhz - 1; lz >= rlz && lz < rhz; lz += sz)
            for (int ly = forward ? rly : rhy - 1; ly >= rly && ly < rhy; ly += sz)
                for (int lx = forward ? rlx : rhx - 1; lx >= rlx && lx < rhx;
                     lx += sz) {
                    const size_t k = di(lx, ly, lz);
                    if ((solid[k] != 0) != solidPass)
                        continue;
                    int best = d[k];
                    for (const Nb& n : nbs) {
                        // keep only neighbours scanned earlier in this pass
                        if (forward) {
                            if (n.dz > 0 || (n.dz == 0 && n.dy > 0) ||
                                (n.dz == 0 && n.dy == 0 && n.dx > 0))
                                continue;
                        } else {
                            if (n.dz < 0 || (n.dz == 0 && n.dy < 0) ||
                                (n.dz == 0 && n.dy == 0 && n.dx < 0))
                                continue;
                        }
                        const int nx = lx + n.dx, ny = ly + n.dy, nz = lz + n.dz;
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= CHUNK_N ||
                            ny >= CHUNK_N || nz >= CHUNK_N)
                            continue;
                        const size_t nk = di(nx, ny, nz);
                        if ((solid[nk] != 0) != solidPass)
                            continue;
                        const int cand = int(d[nk]) + n.cost;
                        if (cand < best)
                            best = cand;
                    }
                    d[k] = int16_t(std::min(best, kMaxDist));
                }
    };
    chamfer(airD, false, true);
    chamfer(airD, false, false);
    chamfer(solD, true, true);
    chamfer(solD, true, false);
    for (int lz = rlz; lz < rhz; ++lz)
        for (int ly = rly; ly < rhy; ++ly)
            for (int lx = rlx; lx < rhx; ++lx) {
                const size_t k = di(lx, ly, lz);
                const int d = solid[k] ? -int(solD[k]) : int(airD[k]);
                words[k * 2] = (words[k * 2] & 0x00FFFFFFu) |
                               (uint32_t(uint8_t(
                                   int8_t(std::clamp(d / 10, -127, kFar))))
                                << 24);
            }

    // --- sparse canonical data ---------------------------------------------
    // Blocks outside the region are copied verbatim (their dense scratch was
    // never materialised); region blocks are re-derived from the dense grid.
    std::vector<uint32_t> newBricks;
    newBricks.reserve(c.bricks.size());
    std::vector<uint32_t> newSlot(kBricksPerChunk, kNoBrick);
    std::vector<SolidBox> newBoxes;
    for (const SolidBox& b : c.boxes) {
        // Drop a box that is fully inside the rebuild region (it is re-derived
        // from the dense grid). A box that straddles the region boundary is
        // kept: the region blocks are re-derived anyway, and query order
        // (bricks before boxes) makes the overlap harmless — dropping it would
        // leave its out-of-region cells uncovered.
        const bool fullyInside =
            b.x >= rlx && b.x + b.side <= rhx && b.y >= rly && b.y + b.side <= rhy &&
            b.z >= rlz && b.z + b.side <= rhz;
        if (!fullyInside)
            newBoxes.push_back(b);
    }
    for (int key = 0; key < kBricksPerChunk; ++key) {
        const int bx = (key & 7) * BRICK_N, by = ((key >> 3) & 7) * BRICK_N,
                  bz = ((key >> 6) & 7) * BRICK_N;
        if (blockOutside(bx, by, bz)) {
            const uint32_t s = hasSlots ? c.slotOf[size_t(key)] : kNoBrick;
            if (s == kNoBrick)
                continue;
            const uint32_t slot = uint32_t(newBricks.size() / BRICK_WORDS);
            const uint32_t* src = c.bricks.data() + size_t(s) * BRICK_WORDS;
            newBricks.insert(newBricks.end(), src, src + BRICK_WORDS);
            newSlot[size_t(key)] = slot;
            continue;
        }
        bool allAir = true, allBox = true, anySolid = false, anyBand = false;
        for (int z = 0; z < BRICK_N; ++z)
            for (int y = 0; y < BRICK_N; ++y)
                for (int x = 0; x < BRICK_N; ++x) {
                    const size_t k = di(bx + x, by + y, bz + z);
                    const bool s = kind[k] != 0;
                    if (s) {
                        allAir = false;
                        anySolid = true;
                        if (kind[k] != 1)
                            allBox = false;
                    } else {
                        allBox = false;
                        const int d = int(int8_t((words[k * 2] >> 24) & 0xFFu));
                        if (d < kFar)
                            anyBand = true;
                    }
                }
        // A kept SolidBox that straddles the region boundary still covers this
        // block's cells and cellAt checks bricks before boxes: dropping an
        // all-air brick here would resurrect the box's solid for every cell the
        // edit just cleared (the localized rebuild must match a full rebuild in
        // both directions). Force a brick over such blocks.
        bool boxCovered = false;
        for (const SolidBox& kb : newBoxes)
            if (kb.x < bx + BRICK_N && bx < kb.x + kb.side &&
                kb.y < by + BRICK_N && by < kb.y + kb.side &&
                kb.z < bz + BRICK_N && bz < kb.z + kb.side) {
                boxCovered = true;
                break;
            }
        if (allAir && !anyBand && !boxCovered)
            continue; // no brick needed: deep air
        if (allBox) {
            newBoxes.push_back({ int16_t(bx), int16_t(by), int16_t(bz), BRICK_N, 2,
                                 true });
            continue;
        }
        (void)anySolid;
        const uint32_t slot = uint32_t(newBricks.size() / BRICK_WORDS);
        newBricks.resize(newBricks.size() + BRICK_WORDS, 0);
        uint32_t* dst = newBricks.data() + size_t(slot) * BRICK_WORDS;
        for (int z = 0; z < BRICK_N; ++z)
            for (int y = 0; y < BRICK_N; ++y)
                for (int x = 0; x < BRICK_N; ++x) {
                    const size_t k = di(bx + x, by + y, bz + z);
                    const size_t si = (size_t(z) * BRICK_N + y) * BRICK_N + x;
                    dst[si * 2] = words[k * 2];
                    dst[si * 2 + 1] = words[k * 2 + 1];
                }
        newSlot[size_t(key)] = slot;
    }
    c.bricks = std::move(newBricks);
    c.slotOf = std::move(newSlot);
    c.boxes = std::move(newBoxes);

    buildPoolOnly(ci);
    c.dirty = false;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (getenv("VF_TRACE"))
        spdlog::info("chunk rebuild {} state {} region local x[{}..{}] y[{}..{}] z[{}..{}]",
                     ci, int(c.state), rlx, rhx, rly, rhy, rlz, rhz);
    if (getenv("VF_TRACE"))
        spdlog::info("chunk_store: rebuilt chunk {} in {:.1f} ms", ci, ms);
}

bool ChunkStore::blockCoveredByBox(const Chunk& c, int lx, int ly, int lz) const
{
    for (const SolidBox& b : c.boxes) {
        if (lx >= b.x && lx < b.x + b.side && ly >= b.y && ly < b.y + b.side &&
            lz >= b.z && lz < b.z + b.side)
            return true;
    }
    return false;
}

// Build the chunk's octree pool from canonical sparse data. A per-block class
// grid (empty / solid / explicit) keeps the structure chunk-local so no
// neighbours are needed.
void ChunkStore::buildPoolOnly(int ci)
{
    Chunk& c = m_chunks[size_t(ci)];
    c.poolValid = false;
    if (c.state == Chunk::State::Empty) {
        c.pool.reset();
        c.poolValid = true;
        return;
    }
    std::array<uint8_t, kBricksPerChunk> cls {};
    for (int key = 0; key < kBricksPerChunk; ++key) {
        const uint32_t slot =
            c.state == Chunk::State::Explicit ? c.slotOf[size_t(key)] : kNoBrick;
        if (slot != kNoBrick) {
            const uint32_t* w = c.bricks.data() + size_t(slot) * BRICK_WORDS;
            bool allSolid = true, anyBand = false;
            for (int i = 0; i < BRICK_VOXELS; ++i) {
                const int8_t raw = int8_t((w[i * 2] >> 24) & 0xFFu);
                if (raw > 0) { // raw <= 0 is solid (see decodeCell)
                    allSolid = false;
                    if (raw < kFar)
                        anyBand = true;
                }
            }
            cls[size_t(key)] = allSolid ? 1 : (anyBand ? 2 : 0);
            continue;
        }
        if (c.state == Chunk::State::Solid) {
            cls[size_t(key)] = 1;
            continue;
        }
        const int bx = (key & 7) * BRICK_N, by = ((key >> 3) & 7) * BRICK_N,
                  bz = ((key >> 6) & 7) * BRICK_N;
        cls[size_t(key)] = blockCoveredByBox(c, bx + BRICK_N / 2, by + BRICK_N / 2,
                                             bz + BRICK_N / 2)
                               ? 1
                               : 0;
    }

    auto pool = std::make_unique<ChunkPool>();
    std::function<int32_t(int, int, int, int)> build = [&](int bx, int by, int bz,
                                                           int side) -> int32_t {
        const int bpa = side / BRICK_N;
        bool allEmpty = true, allSolid = true;
        for (int k = 0; k < bpa; ++k)
            for (int j = 0; j < bpa; ++j)
                for (int i = 0; i < bpa; ++i) {
                    const uint8_t kk = cls[size_t(blockKeyOf(bx + i * BRICK_N,
                                                             by + j * BRICK_N,
                                                             bz + k * BRICK_N))];
                    allEmpty &= (kk == 0);
                    allSolid &= (kk == 1);
                }
        if (allEmpty)
            return -1;
        if (allSolid && side > BRICK_N)
            return int32_t(kSolidHandle);
        if (side == BRICK_N) {
            const uint8_t kk = cls[size_t(blockKeyOf(bx, by, bz))];
            if (kk == 0)
                return -1;
            if (kk == 1)
                return int32_t(kSolidHandle);
            const uint32_t slot = c.slotOf[size_t(blockKeyOf(bx, by, bz))];
            if (slot == kNoBrick)
                return -1;
            uint32_t data[BRICK_WORDS];
            std::memcpy(data, c.bricks.data() + size_t(slot) * BRICK_WORDS,
                        sizeof(data));
            return int32_t(pool->emitBrick(data));
        }
        const int half = side / 2;
        const uint32_t nodeH = pool->allocNode();
        const uint32_t base = pool->childBase[nodeIndexOf(nodeH)];
        uint32_t validMask = 0, solidMask = 0;
        for (int i = 0; i < 8; ++i) {
            const int ox = bx + (i & 1) * half, oy = by + ((i >> 1) & 1) * half,
                      oz = bz + ((i >> 2) & 1) * half;
            const int32_t ch = build(ox, oy, oz, half);
            pool->handles[base + uint32_t(i)] = uint32_t(ch);
            if (ch >= 0 || ch == int32_t(kSolidHandle))
                validMask |= 1u << i;
            if (ch == int32_t(kSolidHandle))
                solidMask |= 1u << i;
        }
        if (validMask == 0)
            return -1;
        if (solidMask == 0xFFu)
            return int32_t(kSolidHandle);
        pool->payload[nodeIndexOf(nodeH)] = validMask | (solidMask << 8);
        return int32_t(nodeH);
    };
    pool->root = build(0, 0, 0, CHUNK_N);
    c.pool = std::move(pool);
    c.poolValid = true;
}

const std::unique_ptr<ChunkPool>& ChunkStore::pool(int ci)
{
    Chunk& c = m_chunks[size_t(ci)];
    if (!c.poolValid)
        buildPoolOnly(ci);
    return c.pool;
}

void ChunkStore::rebuildDirty()
{
    for (int ci : m_dirty)
        rebuildChunk(ci);
    m_dirty.clear();
}

void ChunkStore::rebuildFull(int ci)
{
    if (ci < 0 || ci >= int(m_chunks.size()))
        return;
    Chunk& c = m_chunks[size_t(ci)];
    const int lo[3] = { c.lo[0], c.lo[1], c.lo[2] };
    const int hi[3] = { c.hi[0], c.hi[1], c.hi[2] };
    c.lo[0] = c.lo[1] = c.lo[2] = 0;
    c.hi[0] = c.hi[1] = c.hi[2] = CHUNK_N - 1; // full chunk
    rebuildChunk(ci);
    for (int i = 0; i < 3; ++i) {
        c.lo[i] = lo[i];
        c.hi[i] = hi[i];
    }
}

ChunkStore::Stats ChunkStore::stats() const
{
    Stats s;
    for (const Chunk& c : m_chunks) {
        if (c.state == Chunk::State::Empty)
            continue;
        ++s.chunks;
        s.bricks += c.bricks.size() / BRICK_WORDS;
        s.solidBoxes += c.boxes.size();
    }
    return s;
}

uint64_t ChunkStore::chunkHash(int ci) const
{
    if (ci < 0 || ci >= int(m_chunks.size()))
        return 0;
    const Chunk& c = m_chunks[size_t(ci)];
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(uint64_t(c.state));
    mix(c.bricks.size());
    for (uint32_t w : c.bricks)
        mix(w);
    for (const SolidBox& b : c.boxes) {
        mix(uint64_t(uint16_t(b.x)) | (uint64_t(uint16_t(b.y)) << 16) |
            (uint64_t(uint16_t(b.z)) << 32) | (uint64_t(uint16_t(b.side)) << 48));
        mix(uint64_t(b.mat) | (uint64_t(b.terrain) << 8));
    }
    return h;
}

// ---------------------------------------------------------------------------
// VXW v2 store-overlay section (opaque to worldfile; schema owned here).
// Layout (little-endian):
//   u32 chunkCount
//   per chunk: u32 ci, u8 state, u8 pad[3], u32 boxCount, u32 brickCount,
//              boxes[boxCount] { i16 x,y,z,side; u8 mat; u8 terrain },
//              bricks[brickCount] { u32 blockKey; u32 words[BRICK_WORDS] }
// ---------------------------------------------------------------------------
namespace {

// v2: adds the edit AABB per chunk.
// v3: word1 byte 0 is now the per-cell texture override (was alpha=255
//     filler). A stale v2 overlay carries garbage in that byte (the writer
//     never initialized it), which would paint random atlas layers over the
//     restored geometry, so the schema bumps to reject it.
constexpr uint32_t kOverlaySchema = 3;

void put32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 24));
}

struct SecReader {
    const uint8_t* p = nullptr;
    size_t n = 0, off = 0;
    bool get(void* d, size_t k)
    {
        if (off + k > n)
            return false;
        std::memcpy(d, p + off, k);
        off += k;
        return true;
    }
    template <typename T>
    bool pod(T& v)
    {
        return get(&v, sizeof(T));
    }
};

bool writeOverlayBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    WorldFileData d;
    d.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    Section s;
    s.type = worldfile::kSectionStoreChunks;
    s.data = bytes;
    d.sections.push_back(std::move(s));
    const std::string tmp = path + ".tmp";
    if (!worldfile::write(tmp, d))
        return false;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

bool ChunkStore::hasEditedChunks() const
{
    for (const Chunk& c : m_chunks)
        if (c.edited)
            return true;
    return false;
}

std::vector<int> ChunkStore::editedChunks() const
{
    std::vector<int> out;
    for (int i = 0; i < int(m_chunks.size()); ++i)
        if (m_chunks[size_t(i)].edited)
            out.push_back(i);
    return out;
}

bool ChunkStore::chunkEditBounds(int ci, int lo[3], int hi[3]) const
{
    if (ci < 0 || ci >= int(m_chunks.size()))
        return false;
    const Chunk& c = m_chunks[size_t(ci)];
    if (c.lo[0] > c.hi[0])
        return false;
    for (int i = 0; i < 3; ++i) {
        lo[i] = c.lo[i];
        hi[i] = c.hi[i];
    }
    return true;
}

std::vector<uint8_t> ChunkStore::serializeEdited() const
{
    std::vector<uint8_t> out;
    const std::vector<int> chunks = editedChunks();
    put32(out, kOverlaySchema); // schema version
    put32(out, uint32_t(chunks.size()));
    for (int ci : chunks) {
        const Chunk& c = m_chunks[size_t(ci)];
        put32(out, uint32_t(ci));
        out.push_back(uint8_t(c.state));
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        // edit AABB (global lattice, i32) so a reload can refresh exactly the
        // region the session had refreshed, keeping the unedited surfels from
        // the GPU-seeded cache
        int32_t bounds[6] = { c.lo[0], c.lo[1], c.lo[2], c.hi[0], c.hi[1], c.hi[2] };
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(bounds),
                   reinterpret_cast<const uint8_t*>(bounds) + sizeof(bounds));
        put32(out, uint32_t(c.boxes.size()));
        uint32_t nb = 0;
        for (uint32_t s : c.slotOf)
            if (s != kNoBrick)
                ++nb;
        put32(out, nb);
        for (const SolidBox& b : c.boxes) {
            int16_t xy[4] = { b.x, b.y, b.z, b.side };
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(xy),
                       reinterpret_cast<const uint8_t*>(xy) + 8);
            out.push_back(b.mat);
            out.push_back(b.terrain ? 1 : 0);
        }
        for (int key = 0; key < kBricksPerChunk; ++key) {
            const uint32_t s = c.slotOf[size_t(key)];
            if (s == kNoBrick)
                continue;
            put32(out, uint32_t(key));
            const uint32_t* w = c.bricks.data() + size_t(s) * BRICK_WORDS;
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(w),
                       reinterpret_cast<const uint8_t*>(w + BRICK_WORDS));
        }
    }
    return out;
}

bool ChunkStore::applyEdited(const std::vector<uint8_t>& data)
{
    SecReader r { data.data(), data.size() };
    uint32_t schema = 0, chunkCount = 0;
    if (!r.pod(schema) || schema != kOverlaySchema)
        return false; // older/unknown overlay schema: ignore (not an error)
    if (!r.pod(chunkCount) || chunkCount > uint32_t(kChunkCount))
        return false;
    for (uint32_t k = 0; k < chunkCount; ++k) {
        uint32_t ci = 0, boxCount = 0, brickCount = 0;
        uint8_t state = 0, pad[3] = {};
        if (!r.pod(ci) || ci >= uint32_t(kChunkCount))
            return false;
        if (!r.get(&state, 1) || !r.get(pad, 3))
            return false;
        int32_t bounds[6] = {};
        if (!r.get(bounds, sizeof(bounds)))
            return false;
        if (!r.pod(boxCount) || !r.pod(brickCount))
            return false;
        if (boxCount > uint32_t(kBricksPerChunk) || brickCount > uint32_t(kBricksPerChunk))
            return false;
        Chunk& c = m_chunks[size_t(ci)];
        for (int i = 0; i < 3; ++i) {
            c.lo[i] = bounds[i];
            c.hi[i] = bounds[3 + i];
        }
        c.state = state <= 2 ? Chunk::State(state) : Chunk::State::Empty;
        c.bricks.clear();
        c.slotOf.assign(kBricksPerChunk, kNoBrick);
        c.boxes.clear();
        c.pool.reset();
        c.poolValid = false;
        c.dirty = false;
        for (uint32_t b = 0; b < boxCount; ++b) {
            int16_t xy[4];
            uint8_t mb[2];
            if (!r.get(xy, 8) || !r.get(mb, 2))
                return false;
            SolidBox box;
            box.x = xy[0];
            box.y = xy[1];
            box.z = xy[2];
            box.side = xy[3];
            box.mat = mb[0];
            box.terrain = mb[1] != 0;
            if (box.side <= 0 || box.x < 0 || box.y < 0 || box.z < 0 ||
                box.x + box.side > CHUNK_N || box.y + box.side > CHUNK_N ||
                box.z + box.side > CHUNK_N)
                return false;
            c.boxes.push_back(box);
        }
        for (uint32_t b = 0; b < brickCount; ++b) {
            uint32_t key = 0;
            if (!r.pod(key) || key >= uint32_t(kBricksPerChunk))
                return false;
            const uint32_t slot = uint32_t(c.bricks.size() / BRICK_WORDS);
            c.bricks.resize(c.bricks.size() + BRICK_WORDS);
            if (!r.get(c.bricks.data() + size_t(slot) * BRICK_WORDS,
                       BRICK_WORDS * sizeof(uint32_t)))
                return false;
            c.slotOf[size_t(key)] = slot;
        }
        c.edited = true;
    }
    return true;
}

bool ChunkStore::saveOverlay(const std::string& path) const
{
    return writeOverlayBytes(path, serializeEdited());
}

bool ChunkStore::loadOverlay(const std::string& path)
{
    WorldFileData d;
    if (!worldfile::read(path, d))
        return false;
    for (const Section& s : d.sections)
        if (s.type == worldfile::kSectionStoreChunks)
            return applyEdited(s.data);
    return false;
}

// --- async overlay writer ---------------------------------------------------

OverlayWriter::~OverlayWriter()
{
    flush();
}

void OverlayWriter::queue(const ChunkStore& store, const std::string& path)
{
    std::vector<uint8_t> bytes = store.serializeEdited();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_path = path;
        m_bytes = std::move(bytes);
        m_pending = true;
        if (m_running) {
            m_cv.notify_all();
            return;
        }
        m_running = true;
        m_th = std::thread([this] {
            for (;;) {
                std::string p;
                std::vector<uint8_t> b;
                {
                    std::unique_lock<std::mutex> lk(m_mtx);
                    m_cv.wait(lk, [&] { return m_pending || m_stop; });
                    if (m_stop && !m_pending)
                        break;
                    p = m_path;
                    b = std::move(m_bytes);
                    m_pending = false;
                }
                if (!writeOverlayBytes(p, b))
                    spdlog::error("chunk_store: failed to write overlay '{}'", p);
            }
            std::lock_guard<std::mutex> lk(m_mtx);
            m_running = false;
            m_cv.notify_all();
        });
    }
    m_cv.notify_all();
}

void OverlayWriter::flush()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_th.joinable())
        m_th.join();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_running = false;
        m_stop = false;
    }
}

} // namespace vf::voxel
