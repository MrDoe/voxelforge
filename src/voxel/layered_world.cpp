#include "voxel/layered_world.hpp"
#include "voxel/common.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <unordered_set>

namespace vf::voxel {

namespace {

constexpr int kLatN = int(WORLD / VOXEL);
constexpr int kBlockSize = 4; // presence-block granularity (cells)
constexpr int kBlocksPerAxis = CHUNK_N / kBlockSize;
constexpr int kGBlocks = GRID_N * kBlocksPerAxis; // global blocks per world axis

inline uint32_t cellKey(uint32_t x, uint32_t y, uint32_t z)
{
    return (x << 20) | (y << 10) | z;
}

// Open-addressing key -> packed appearance map, read-only after build so
// worker threads can query it lock-free.
struct CellMap {
    std::vector<uint32_t> keys; // key + 1; 0 = empty slot
    std::vector<uint64_t> vals; // r|g<<8|b<<16|refl<<24|rough<<32|mat<<40
    std::vector<uint32_t> idxs; // record index per slot (appearance inheritance)
    size_t mask = 0;

    static uint64_t pack(const VoxelRecord& v)
    {
        return uint64_t(v.r) | (uint64_t(v.g) << 8) | (uint64_t(v.b) << 16) |
               (uint64_t(v.reflectivity) << 24) | (uint64_t(v.roughness) << 32) |
               (uint64_t(v.materialId) << 40);
    }
    void build(const std::vector<VoxelRecord>& recs)
    {
        size_t cap = 1024;
        while (cap < recs.size() * 2)
            cap <<= 1;
        keys.assign(cap, 0);
        vals.assign(cap, 0);
        idxs.assign(cap, 0);
        mask = cap - 1;
        for (size_t i = 0; i < recs.size(); ++i)
            insert(cellKey(recs[i].x, recs[i].y, recs[i].z), pack(recs[i]), uint32_t(i));
    }
    inline size_t slot(uint32_t k) const { return (size_t(k) * 2654435761u) & mask; }
    void insert(uint32_t k, uint64_t val, uint32_t idx)
    {
        size_t i = slot(k);
        while (keys[i]) {
            if (keys[i] - 1 == k) {
                vals[i] = val;
                idxs[i] = idx;
                return;
            }
            i = (i + 1) & mask;
        }
        keys[i] = k + 1;
        vals[i] = val;
        idxs[i] = idx;
    }
    // found: appearance packed into `out`, record index into `idx`
    inline bool find(uint32_t k, uint64_t& out, uint32_t& idx) const
    {
        size_t i = slot(k);
        while (keys[i]) {
            if (keys[i] - 1 == k) {
                out = vals[i];
                idx = idxs[i];
                return true;
            }
            i = (i + 1) & mask;
        }
        return false;
    }
};

inline uint32_t paletteWordLo(uint8_t mat)
{
    const glm::vec3& c = kPalette[std::min(int(mat), 8)];
    return uint32_t(c.r * 255.f) | (uint32_t(c.g * 255.f) << 8) |
           (uint32_t(c.b * 255.f) << 16);
}
inline uint32_t paletteWordHi(uint8_t mat)
{
    const glm::vec2& rr = kMaterialReflection[std::min(int(mat), 8)];
    return 255u | (uint32_t(rr.x) << 8) | (uint32_t(rr.y) << 16) | (uint32_t(mat) << 24);
}

inline void wordsFromPacked(uint64_t p, int sdfRaw, uint32_t w[2])
{
    w[0] = uint32_t(p & 0xFFFFFFu) | (uint32_t(sdfRaw & 0xFF) << 24);
    w[1] = 255u | (uint32_t((p >> 24) & 0xFFu) << 8) |
           (uint32_t((p >> 32) & 0xFFu) << 16) | (uint32_t((p >> 40) & 0xFFu) << 24);
}

unsigned long long sigOf(const std::filesystem::path& p)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec)
        return 0;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch())
                  .count();
    return static_cast<unsigned long long>(ns) * 0x9E3779B97F4A7C15ull;
}

struct BuildCtx {
    const std::vector<VoxelRecord>* records = nullptr;
    const CellMap* map = nullptr;
    const VoxelField* field = nullptr;                // records-derived geometry oracle
    const uint8_t* blockSolid = nullptr;              // global presence grid (records+interior)
    std::vector<std::vector<uint32_t>> chunkRecords;  // per-chunk record indices
    uint32_t ugLo = 0, ugHi = 0;                      // underground interior words
    uint32_t waterLo = 0, waterHi = 0;                // submerged volume words
};

// 0 = empty, 1 = fully underground-solid, 2 = mixed (subdivide / brick).
// `blocks` may be null when the caller already knows the box holds no records.
int classifyBox(const BuildCtx& c, const uint8_t* blocks, bool anyKnown, int x0, int y0,
                int z0, int side)
{
    bool any = anyKnown;
    if (!any && blocks) {
        const int bn = side / kBlockSize;
        const int bx0 = x0 / kBlockSize, by0 = y0 / kBlockSize, bz0 = z0 / kBlockSize;
        for (int gz = bz0; gz < bz0 + bn && !any; ++gz)
            for (int gy = by0; gy < by0 + bn && !any; ++gy)
                for (int gx = bx0; gx < bx0 + bn && !any; ++gx)
                    any |= blocks[(size_t(gz) * kGBlocks + size_t(gy)) * kGBlocks +
                                  size_t(gx)] != 0;
    }
    if (any)
        return 2;
    // no geometry here: solid iff entirely below the lowest landscape top
    int topMin = 32767;
    for (int z = z0; z < z0 + side; ++z)
        for (int x = x0; x < x0 + side; ++x) {
            int16_t t = c.field->colTops()[size_t(z) * kLatN + size_t(x)];
            if (t < 0) {
                topMin = -1;
                goto noTerrain;
            }
            if (t < topMin)
                topMin = t;
        }
noTerrain:
    if (topMin < 0)
        return 0; // no landscape layer anywhere under this box
    return y0 + side - 1 < topMin ? 1 : 0;
}

void fillBrick(const BuildCtx& c, uint32_t* data, int bx0, int by0, int bz0)
{
    // Bake the VoxelField signed distance into the brick. The field is derived
    // purely from the layer records: terrain columns give the vertical distance,
    // object components contribute a flood-filled signed grid - negative inside
    // every solid (shells and enclosed interiors), so objects read solid at
    // every distance and there is no hollow-voxel / flood-leak hole. Appearance
    // is the exact record colour where one exists, otherwise the palette of the
    // field's material.
    for (int bz = 0; bz < BRICK_N; ++bz)
        for (int by = 0; by < BRICK_N; ++by)
            for (int bx = 0; bx < BRICK_N; ++bx) {
                int cx = bx0 + bx, cy = by0 + by, cz = bz0 + bz;
                VoxelField::Sample h = c.field->sample(cx, cy, cz);
                float worldYc = -0.5f * WORLD + (float(cy) + 0.5f) * VOXEL;
                uint32_t w[2];

                if (h.d < 0.0f) {
                    // solid: exact record colour if present, else palette material
                    uint64_t p;
                    uint32_t ridx;
                    if (c.map->find(cellKey(uint32_t(cx), uint32_t(cy), uint32_t(cz)), p,
                                    ridx))
                        wordsFromPacked(p, int(h.d / VOXEL), w);
                    else {
                        w[0] = paletteWordLo(h.mat) |
                               (uint32_t(int32_t(int(h.d / VOXEL)) & 0xFF) << 24);
                        w[1] = paletteWordHi(h.mat);
                    }
                } else if (worldYc < WATER_LEVEL) {
                    // water volume (shader-only surface at y = WATER_LEVEL)
                    int sraw = h.d > 32.0f * VOXEL ? 32 : int(h.d / VOXEL);
                    w[0] = c.waterLo | (uint32_t(sraw & 0xFF) << 24);
                    w[1] = c.waterHi;
                } else {
                    // air: distance to nearest surface + material for hit shading
                    int sraw = h.d > 32.0f * VOXEL ? 32 : int(h.d / VOXEL);
                    w[0] = paletteWordLo(h.mat) | (uint32_t(sraw & 0xFF) << 24);
                    w[1] = paletteWordHi(h.mat);
                }
                size_t i = (size_t(bz) * BRICK_N + size_t(by)) * BRICK_N + bx;
                data[i * 2] = w[0];
                data[i * 2 + 1] = w[1];
            }
}

// World-space AABB -> set of chunk indices (with a margin so SDF tails at the
// chunk boundary are rebuilt too).
inline std::vector<int> chunkRangeFromAABB(const WorldAABB& b)
{
    std::vector<int> out;
    auto clampC = [](int c) { return c < 0 ? 0 : (c > GRID_N - 1 ? GRID_N - 1 : c); };
    auto toChunk = [&](float x) {
        int c = int(std::floor((x + 0.5f * WORLD) / CHUNK_N));
        return clampC(c);
    };
    int margin = 2;
    int cx0 = clampC(toChunk(b.lo.x) - margin), cx1 = clampC(toChunk(b.hi.x) + margin);
    int cy0 = clampC(toChunk(b.lo.y) - margin), cy1 = clampC(toChunk(b.hi.y) + margin);
    int cz0 = clampC(toChunk(b.lo.z) - margin), cz1 = clampC(toChunk(b.hi.z) + margin);
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                out.push_back(cx + cy * GRID_N + cz * GRID_N * GRID_N);
    return out;
}

} // namespace

bool LayeredWorld::load(const std::string& manifestPath)
{
    m_manifestPath = manifestPath;
    std::vector<worldfile::WorldLayer> manifest;
    if (!worldfile::loadManifest(manifestPath, manifest))
        return false;

    std::string dir = manifestPath;
    size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);

    WorldFileMeta expected{ WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    m_layersMeta.clear();
    m_signatures.clear();
    m_records.clear();
    m_colTop.assign(size_t(kLatN) * kLatN, -1);
    m_colMat.assign(size_t(kLatN) * kLatN, 0);
    m_objCells.clear();
    m_objMats.clear();
    m_blockSolid.clear();

    std::unordered_set<uint32_t> claimed;
    claimed.reserve(1u << 22);

    auto addSig = [&](const std::string& file) {
        std::filesystem::path p = std::filesystem::path(dir) / file;
        std::error_code ec;
        unsigned long long sz = static_cast<unsigned long long>(
            ec ? 0ull : std::filesystem::file_size(p, ec));
        m_signatures.push_back({ p.string(), sigOf(p), sz });
    };

    addSig(manifestPath);

    // per-layer record AABB + enabled-state, used to derive the dirty chunk set
    // for incremental rebuilds.
    std::map<std::string, WorldAABB> curBox;
    std::map<std::string, bool> curEnabled;
    std::string landscapeFile;
    for (const auto& l : manifest) {
        curEnabled[l.file] = l.enabled;
        if (l.name == "landscape" || l.file == "landscape.vxw")
            landscapeFile = l.file;
    }

    for (const worldfile::WorldLayer& l : manifest) {
        if (l.role == "packed" || !l.enabled)
            continue;
        WorldFileData data;
        if (!worldfile::read(dir + l.file, data)) {
            spdlog::error("layered_world: cannot read layer '{}'", l.file);
            continue;
        }
        if (data.meta.worldSize != expected.worldSize ||
            data.meta.voxelSize != expected.voxelSize ||
            data.meta.gridN != expected.gridN) {
            spdlog::error("layered_world: layer '{}' meta mismatch", l.file);
            continue;
        }
        addSig(l.file);
        m_layersMeta.push_back(l);
        const bool isLandscape = (l.name == "landscape" || l.file == "landscape.vxw");
        WorldAABB& box = curBox[l.file];
        for (VoxelRecord& v : data.voxels) {
            uint32_t key = cellKey(v.x, v.y, v.z);
            if (claimed.insert(key).second)
                m_records.push_back(v);
            glm::vec3 wp = v.position(expected);
            box.lo = glm::min(box.lo, wp);
            box.hi = glm::max(box.hi, wp);
            if (isLandscape) {
                int16_t& top = m_colTop[size_t(v.z) * kLatN + size_t(v.x)];
                if (v.y > top) {
                    top = int16_t(v.y);
                    m_colMat[size_t(v.z) * kLatN + size_t(v.x)] = v.materialId;
                }
            } else {
                m_objCells.push_back(cellKey(v.x, v.y, v.z));
                m_objMats.push_back(v.materialId);
            }
        }
    }

    // --- decide full vs incremental rebuild -------------------------------
    bool full = !m_hasFullBuild;
    WorldAABB dirty;
    bool dirtyValid = false;
    for (const auto& kv : curEnabled) {
        bool changed = true;
        auto pit = m_prevEnabled.find(kv.first);
        if (pit != m_prevEnabled.end() && pit->second == kv.second) {
            auto pbox = m_prevBox.find(kv.first);
            auto cbox = curBox.find(kv.first);
            if (pbox != m_prevBox.end() && cbox != curBox.end() &&
                pbox->second.lo == cbox->second.lo && pbox->second.hi == cbox->second.hi)
                changed = false;
        }
        if (changed) {
            if (kv.first == landscapeFile)
                full = true;
            auto cbox = curBox.find(kv.first);
            if (cbox != curBox.end()) {
                dirty.lo = glm::min(dirty.lo, cbox->second.lo);
                dirty.hi = glm::max(dirty.hi, cbox->second.hi);
                dirtyValid = true;
            }
        }
    }
    for (const auto& kv : m_prevEnabled) {
        if (curEnabled.find(kv.first) == curEnabled.end()) {
            auto pbox = m_prevBox.find(kv.first);
            if (pbox != m_prevBox.end()) {
                dirty.lo = glm::min(dirty.lo, pbox->second.lo);
                dirty.hi = glm::max(dirty.hi, pbox->second.hi);
                dirtyValid = true;
            }
            if (kv.first == landscapeFile)
                full = true;
        }
    }

    std::vector<int> dirtyChunks;
    if (dirtyValid && !full) {
        dirtyChunks = chunkRangeFromAABB(dirty);
        if (dirtyChunks.empty() || dirtyChunks.size() > 1024)
            full = true;
    }
    if (full)
        dirtyChunks.clear();

    if (!synthesize(full, dirtyChunks)) {
        m_loaded = false;
        return false;
    }

    m_prevBox = std::move(curBox);
    m_prevEnabled = std::move(curEnabled);
    m_hasFullBuild = true;
    m_loaded = true;
    return true;
}

bool LayeredWorld::reloadIfChanged()
{
    if (!m_loaded)
        return false;
    for (const Signature& s : m_signatures) {
        std::error_code ec;
        unsigned long long sz = static_cast<unsigned long long>(
            ec ? 0ull : std::filesystem::file_size(s.path, ec));
        if (ec || sigOf(s.path) != s.mtime || sz != s.size) {
            spdlog::info("layered_world: '{}' changed on disk, rebuilding", s.path);
            return load(m_manifestPath);
        }
    }
    return false;
}

bool LayeredWorld::synthesize(bool full, const std::vector<int>& dirty)
{
    auto t0 = std::chrono::steady_clock::now();
    if (m_records.empty()) {
        spdlog::warn("layered_world: no records - refusing to synthesize");
        return false;
    }

    CellMap map;
    map.build(m_records);

    BuildCtx ctx;
    ctx.records = &m_records;
    ctx.map = &map;
    ctx.field = &m_field;
    ctx.chunkRecords.resize(size_t(GRID_N) * GRID_N * GRID_N);
    ctx.ugLo = paletteWordLo(2);
    ctx.ugHi = paletteWordHi(2);
    {
        glm::vec3 wc(0.06f, 0.22f, 0.28f); // matches analytic water tint
        ctx.waterLo = uint32_t(wc.r * 255.f) | (uint32_t(wc.g * 255.f) << 8) |
                      (uint32_t(wc.b * 255.f) << 16);
        ctx.waterHi = 255u | (130u << 8) | (25u << 16) | (9u << 24); // shiny, mat 9
    }

    // records-derived geometry oracle: terrain columns + flood-filled object
    // components with a signed distance transform. Replaces the old analytic
    // scene() sampling entirely.
    m_field.build(m_records, m_colTop, m_colMat, m_objCells, m_objMats);

    // global presence grid (record cells + object interiors) for the SVO builder
    const size_t gBlocks = size_t(kGBlocks) * kGBlocks * kGBlocks;
    m_blockSolid.assign(gBlocks, 0);
    auto markBlock = [&](int x, int y, int z) {
        size_t bi = (size_t(z / kBlockSize) * kGBlocks + size_t(y / kBlockSize)) *
                        kGBlocks +
                    size_t(x / kBlockSize);
        m_blockSolid[bi] = 1;
    };
    for (const VoxelRecord& v : m_records)
        markBlock(v.x, v.y, v.z);
    const std::vector<uint8_t>& objMask = m_field.objectBlockMask();
    for (size_t i = 0; i < gBlocks; ++i)
        m_blockSolid[i] |= objMask[i];

    ctx.blockSolid = m_blockSolid.data();

    for (size_t i = 0; i < m_records.size(); ++i) {
        const VoxelRecord& v = m_records[i];
        size_t ci = (size_t(v.z) / CHUNK_N) * GRID_N * GRID_N +
                    (size_t(v.y) / CHUNK_N) * GRID_N + size_t(v.x) / CHUNK_N;
        ctx.chunkRecords[ci].push_back(uint32_t(i));
    }

    const size_t numChunks = ctx.chunkRecords.size();
    if (m_pools.size() != numChunks) {
        m_pools.clear();
        m_pools.resize(numChunks); // default-constructed (null) unique_ptrs
    }
    if (full)
        for (auto& p : m_pools)
            p.reset();

    std::unordered_set<int> dirtySet(dirty.begin(), dirty.end());

    std::atomic<size_t> next{ 0 };
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < hc; ++t)
        threads.emplace_back([&] {
            for (;;) {
                size_t ci = next.fetch_add(1);
                if (ci >= numChunks)
                    return;
                // reuse the cached pool unless this chunk must be rebuilt
                if (!full && dirtySet.find(int(ci)) == dirtySet.end())
                    continue;
                if (ctx.chunkRecords[ci].empty()) {
                    // no records: drop any previous geometry (the post-process
                    // step may promote fully-below-ground chunks to solid)
                    m_pools[ci].reset();
                    continue;
                }
                int cz = int(ci / (GRID_N * GRID_N));
                int cy = int((ci / GRID_N) % GRID_N);
                int cx = int(ci % GRID_N);
                int x0 = cx * CHUNK_N, y0 = cy * CHUNK_N, z0 = cz * CHUNK_N;

                auto pool = std::make_unique<ChunkPool>();

                std::function<int32_t(int, int, int, int)> build =
                    [&](int bx, int by, int bz, int side) -> int32_t {
                    int cls = classifyBox(ctx, ctx.blockSolid, false, bx, by, bz, side);
                    if (cls == 1)
                        return int32_t(kSolidHandle);
                    if (cls == 0)
                        return -1;
                    if (side == BRICK_N) {
                        uint32_t data[BRICK_WORDS];
                        fillBrick(ctx, data, bx, by, bz);
                        return int32_t(pool->emitBrick(data));
                    }
                    int half = side / 2;
                    uint32_t nodeH = pool->allocNode();
                    uint32_t base = pool->childBase[nodeIndexOf(nodeH)];
                    uint32_t validMask = 0, solidMask = 0;
                    for (int i = 0; i < 8; ++i) {
                        int ox = bx + (i & 1) * half;
                        int oy = by + ((i >> 1) & 1) * half;
                        int oz = bz + ((i >> 2) & 1) * half;
                        int32_t ch = build(ox, oy, oz, half);
                        pool->handles[base + i] = uint32_t(ch);
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
                pool->root = build(x0, y0, z0, CHUNK_N);
                m_pools[ci] = std::move(pool);
            }
        });
    for (auto& th : threads)
        th.join();

    // --- deterministic merge across per-chunk pools (non-mutating) ---
    m_gpu = GpuWorld{};
    m_gpu.chunkGrid.assign(numChunks, -1);
    size_t nodeOff = 0, hOff = 0, bOff = 0;
    for (size_t ci = 0; ci < numChunks; ++ci) {
        ChunkPool* pp = m_pools[ci].get();
        if (!pp || pp->root < 0) {
            m_gpu.chunkGrid[ci] = -1;
            continue;
        }
        // copy + offset-adjust handles into m_gpu; the pool stays pristine so it
        // can be reused in a later incremental rebuild.
        size_t hStart = m_gpu.handles.size();
        m_gpu.handles.insert(m_gpu.handles.end(), pp->handles.begin(), pp->handles.end());
        for (size_t k = hStart; k < m_gpu.handles.size(); ++k) {
            uint32_t& h = m_gpu.handles[k];
            if (handleIsNode(h))
                h += uint32_t(nodeOff << 2);
            else if (handleIsBrick(h))
                h += uint32_t(bOff << 2);
        }
        uint32_t r = uint32_t(pp->root);
        if (handleIsNode(r))
            r += uint32_t(nodeOff << 2);
        else if (handleIsBrick(r))
            r += uint32_t(bOff << 2);
        m_gpu.chunkGrid[ci] = r == kEmptyHandle ? -1 : int32_t(r);
        size_t cStart = m_gpu.childBase.size();
        m_gpu.childBase.insert(m_gpu.childBase.end(), pp->childBase.begin(),
                               pp->childBase.end());
        for (size_t k = cStart; k < m_gpu.childBase.size(); ++k)
            m_gpu.childBase[k] += uint32_t(hOff);
        m_gpu.payload.insert(m_gpu.payload.end(), pp->payload.begin(), pp->payload.end());
        m_gpu.bricks.insert(m_gpu.bricks.end(), pp->bricks.begin(), pp->bricks.end());
        nodeOff += pp->payload.size();
        hOff += pp->handles.size();
        bOff += pp->bricks.size() / BRICK_WORDS;
    }

    // chunks without any record: fully-below-ground becomes a solid terminal,
    // everything else stays an empty root
    for (size_t ci = 0; ci < numChunks; ++ci) {
        if (m_gpu.chunkGrid[ci] != -1 || !ctx.chunkRecords[ci].empty())
            continue;
        int cz = int(ci / (GRID_N * GRID_N));
        int cy = int((ci / GRID_N) % GRID_N);
        int cx = int(ci % GRID_N);
        int cls = classifyBox(ctx, nullptr, false, cx * CHUNK_N, cy * CHUNK_N,
                              cz * CHUNK_N, CHUNK_N);
        m_gpu.chunkGrid[ci] = cls == 1 ? int32_t(kSolidHandle) : -1;
    }

    m_stats.records = m_records.size();
    m_stats.nodes = m_gpu.payload.size();
    m_stats.bricks = m_gpu.bricks.size() / BRICK_WORDS;
    m_stats.activeChunks =
        size_t(std::count_if(m_gpu.chunkGrid.begin(), m_gpu.chunkGrid.end(),
                             [](int32_t v) { return v >= 0; }));
    m_stats.memoryBytes = m_gpu.memoryBytes();
    m_stats.buildSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (getenv("VF_TRACE")) {
        auto vecHash = [](const std::vector<uint32_t>& v) {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t x : v)
                h = (h ^ x) * 1099511628211ull;
            return h;
        };
        std::vector<uint32_t> gridWords(m_gpu.chunkGrid.begin(), m_gpu.chunkGrid.end());
        spdlog::info("layered_world hash: grid={:x} nodes={:x} handles={:x} bricks={:x}",
                     vecHash(gridWords), vecHash(m_gpu.payload), vecHash(m_gpu.handles),
                     vecHash(m_gpu.bricks));
    }

    spdlog::info("layered_world: {} records -> {} nodes, {} bricks, {}/{} chunks"
                 " active, {:.1f} MB in {:.2f}s{}",
                 m_stats.records, m_stats.nodes, m_stats.bricks, m_stats.activeChunks,
                 GRID_N * GRID_N * GRID_N,
                 double(m_stats.memoryBytes) / (1024.0 * 1024.0), m_stats.buildSeconds,
                 full ? "" : " (incremental)");

    // temporary synthesis grids no longer needed once the SVO is built
    // (m_pools is kept resident for incremental rebuilds)
    m_blockSolid.clear();
    m_blockSolid.shrink_to_fit();
    m_objCells.clear();
    m_objCells.shrink_to_fit();
    m_objMats.clear();
    m_objMats.shrink_to_fit();
    return true;
}

LayeredWorld::~LayeredWorld() = default;

} // namespace vf::voxel
