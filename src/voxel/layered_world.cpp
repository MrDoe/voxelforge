#include "voxel/layered_world.hpp"
#include "voxel/common.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
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
    const int16_t* colTop = nullptr;                  // kLatN*kLatN landscape tops
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
            int16_t t = c.colTop[size_t(z) * kLatN + size_t(x)];
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
    // Bake the analytic signed distance field scene() into the brick. scene()
    // is negative inside every solid - terrain interior and object shells and
    // their enclosed interiors - so objects read solid at every distance and
    // there is no hollow-voxel / flood-leak hole. Appearance is the exact
    // record colour where one exists, otherwise the palette of scene()'s
    // material. The SDF is sphere-traced exactly like the dense raymarch path.
    auto worldOf = [](int vx, int vy, int vz) {
        return glm::vec3(-0.5f * WORLD + (float(vx) + 0.5f) * VOXEL,
                         -0.5f * WORLD + (float(vy) + 0.5f) * VOXEL,
                         -0.5f * WORLD + (float(vz) + 0.5f) * VOXEL);
    };
    auto worldY = [](int cy) { return -0.5f * WORLD + (float(cy) + 0.5f) * VOXEL; };

    for (int bz = 0; bz < BRICK_N; ++bz)
        for (int by = 0; by < BRICK_N; ++by)
            for (int bx = 0; bx < BRICK_N; ++bx) {
                int cx = bx0 + bx, cy = by0 + by, cz = bz0 + bz;
                SceneSample h = scene(worldOf(cx, cy, cz));
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
                } else if (worldY(cy) < WATER_LEVEL) {
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
        const bool isLandscape = l.role == "landscape";
        for (VoxelRecord& v : data.voxels) {
            uint32_t key = cellKey(v.x, v.y, v.z);
            if (claimed.insert(key).second)
                m_records.push_back(v);
            if (isLandscape) {
                int16_t& top = m_colTop[size_t(v.z) * kLatN + size_t(v.x)];
                if (v.y > top)
                    top = int16_t(v.y);
            } else {
                m_objCells.push_back(cellKey(v.x, v.y, v.z));
                m_objMats.push_back(v.materialId);
            }
        }
    }

    if (!synthesize()) {
        m_loaded = false;
        return false;
    }
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

bool LayeredWorld::synthesize()
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
    ctx.colTop = m_colTop.data();
    ctx.chunkRecords.resize(size_t(GRID_N) * GRID_N * GRID_N);
    ctx.ugLo = paletteWordLo(2);
    ctx.ugHi = paletteWordHi(2);
    {
        glm::vec3 wc(0.06f, 0.22f, 0.28f); // matches analytic water tint
        ctx.waterLo = uint32_t(wc.r * 255.f) | (uint32_t(wc.g * 255.f) << 8) |
                      (uint32_t(wc.b * 255.f) << 16);
        ctx.waterHi = 255u | (130u << 8) | (25u << 16) | (9u << 24); // shiny, mat 9
    }

    // --- group object records into connected components (26-neighbourhood) ---
    // The component bounding boxes seed the SDF presence test below (which marks
    // enclosed interiors solid via scene()), so we only touch object regions.
    {
        const size_t nObj = m_objCells.size();
        size_t cap = 1024;
        while (cap < nObj * 2)
            cap <<= 1;
        std::vector<uint32_t> okey(cap, 0), oval(cap, 0); // key+1 -> record index
        const size_t omask = cap - 1;
        auto oplace = [&](uint32_t k, uint32_t v) {
            size_t i = (size_t(k) * 2654435761u) & omask;
            while (okey[i]) {
                if (okey[i] - 1u == k)
                    return;
                i = (i + 1) & omask;
            }
            okey[i] = k + 1;
            oval[i] = v;
        };
        auto ofind = [&](uint32_t k) -> int {
            size_t i = (size_t(k) * 2654435761u) & omask;
            while (okey[i]) {
                if (okey[i] - 1u == k)
                    return int(oval[i]);
                i = (i + 1) & omask;
            }
            return -1;
        };
        for (size_t i = 0; i < nObj; ++i)
            oplace(m_objCells[i], uint32_t(i));

        // label connected components over object cells (26-neighbourhood)
        // and track whether the component contains foliage (mat 8)
        std::vector<uint32_t> comp(nObj, 0xFFFFFFFFu);
        std::vector<uint32_t> queue;
        struct Box { int lo[3], hi[3]; };
        std::vector<Box> boxes;
        std::vector<char> compHasLeaf;
        auto cellXYZ = [](uint32_t k, int& x, int& y, int& z) {
            x = int(k >> 20);
            y = int((k >> 10) & 0x3FFu);
            z = int(k & 0x3FFu);
        };
        for (size_t s = 0; s < nObj; ++s) {
            if (comp[s] != 0xFFFFFFFFu)
                continue;
            uint32_t id = uint32_t(boxes.size());
            int x, y, z;
            cellXYZ(m_objCells[s], x, y, z);
            Box b{};
            b.lo[0] = b.hi[0] = x;
            b.lo[1] = b.hi[1] = y;
            b.lo[2] = b.hi[2] = z;
            comp[s] = id;
            bool hasLeaf = (m_objMats[s] == 8);
            queue.clear();
            queue.push_back(uint32_t(s));
            for (size_t qi = 0; qi < queue.size(); ++qi) {
                int cx, cy, cz;
                cellXYZ(m_objCells[queue[qi]], cx, cy, cz);
                if (cx < b.lo[0]) b.lo[0] = cx;
                if (cy < b.lo[1]) b.lo[1] = cy;
                if (cz < b.lo[2]) b.lo[2] = cz;
                if (cx > b.hi[0]) b.hi[0] = cx;
                if (cy > b.hi[1]) b.hi[1] = cy;
                if (cz > b.hi[2]) b.hi[2] = cz;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy && !dz)
                                continue;
                            int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= kLatN || ny >= kLatN || nz >= kLatN)
                                continue;
                            int ni = ofind(cellKey(uint32_t(nx), uint32_t(ny), uint32_t(nz)));
                            if (ni < 0 || comp[size_t(ni)] != 0xFFFFFFFFu)
                                continue;
                            comp[size_t(ni)] = id;
                            if (m_objMats[size_t(ni)] == 8)
                                hasLeaf = true;
                            queue.push_back(uint32_t(ni));
                        }
            }
            boxes.push_back(b);
            compHasLeaf.push_back(hasLeaf ? 1 : 0);
        }

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

    // Extend octree presence into enclosed interiors by sampling the analytic
    // scene() SDF. scene() is negative inside every solid, so we mark any block
    // whose centre or a corner lies inside (or within VOXEL of) the surface.
    // This reaches the house interior and other object volumes without the old
    // flood-fill that leaked through carved openings (door/windows) and left
    // hollow holes. Seeded by the component boxes so we only touch object regions.
    auto worldOf = [](int vx, int vy, int vz) {
        return glm::vec3(-0.5f * WORLD + (float(vx) + 0.5f) * VOXEL,
                         -0.5f * WORLD + (float(vy) + 0.5f) * VOXEL,
                         -0.5f * WORLD + (float(vz) + 0.5f) * VOXEL);
    };
    const float kMark = VOXEL; // block intersects solid if min sample d < this
    for (uint32_t id = 0; id < boxes.size(); ++id) {
        const Box& b = boxes[id];
        int x0 = std::max(0, b.lo[0] - 1), x1 = std::min(kLatN - 1, b.hi[0] + 1);
        int y0 = std::max(0, b.lo[1] - 1), y1 = std::min(kLatN - 1, b.hi[1] + 1);
        int z0 = std::max(0, b.lo[2] - 1), z1 = std::min(kLatN - 1, b.hi[2] + 1);
        int bx0 = x0 / kBlockSize, bx1 = x1 / kBlockSize;
        int by0 = y0 / kBlockSize, by1 = y1 / kBlockSize;
        int bz0 = z0 / kBlockSize, bz1 = z1 / kBlockSize;
        for (int bz = bz0; bz <= bz1; ++bz)
            for (int by = by0; by <= by1; ++by)
                for (int bx = bx0; bx <= bx1; ++bx) {
                    int vbx = bx * kBlockSize, vby = by * kBlockSize,
                        vbz = bz * kBlockSize;
                    bool hit = false;
                    for (int cz = 0; cz <= 1 && !hit; ++cz)
                        for (int cy = 0; cy <= 1 && !hit; ++cy)
                            for (int cx = 0; cx <= 1 && !hit; ++cx) {
                                int vx = vbx + cx * (kBlockSize - 1);
                                int vy = vby + cy * (kBlockSize - 1);
                                int vz = vbz + cz * (kBlockSize - 1);
                                if (scene(worldOf(vx, vy, vz)).d < kMark)
                                    hit = true;
                            }
                    if (!hit) {
                        // block centre catches a thin wall passing through the middle
                        int vc = kBlockSize / 2;
                        if (scene(worldOf(vbx + vc, vby + vc, vbz + vc)).d < kMark)
                            hit = true;
                    }
                    if (hit)
                        markBlock(vbx, vby, vbz);
                }
    }
    } // close component-grouping block

    ctx.blockSolid = m_blockSolid.data();

    for (size_t i = 0; i < m_records.size(); ++i) {
        const VoxelRecord& v = m_records[i];
        size_t ci = (size_t(v.z) / CHUNK_N) * GRID_N * GRID_N +
                    (size_t(v.y) / CHUNK_N) * GRID_N + size_t(v.x) / CHUNK_N;
        ctx.chunkRecords[ci].push_back(uint32_t(i));
    }

    const size_t numChunks = ctx.chunkRecords.size();

    struct Pool {
        std::vector<uint32_t> childBase, payload, handles, bricks;
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
    std::vector<Pool*> pools(numChunks, nullptr);
    std::vector<int32_t> chunkGrid(numChunks, -1);

    std::atomic<size_t> next{ 0 };
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < hc; ++t)
        threads.emplace_back([&] {
            for (;;) {
                size_t ci = next.fetch_add(1);
                if (ci >= numChunks)
                    return;
                if (ctx.chunkRecords[ci].empty())
                    continue; // resolved after the merge (solid-or-empty)

                int cz = int(ci / (GRID_N * GRID_N));
                int cy = int((ci / GRID_N) % GRID_N);
                int cx = int(ci % GRID_N);
                int x0 = cx * CHUNK_N, y0 = cy * CHUNK_N, z0 = cz * CHUNK_N;

                Pool* pool = new Pool();
                pools[ci] = pool;

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
                chunkGrid[ci] = build(x0, y0, z0, CHUNK_N);
            }
        });
    for (auto& th : threads)
        th.join();

    // deterministic merge across per-chunk pools
    m_gpu = GpuWorld{};
    m_gpu.chunkGrid = std::move(chunkGrid);
    size_t nodeOff = 0, hOff = 0, bOff = 0;
    for (size_t ci = 0; ci < numChunks; ++ci) {
        Pool* pp = pools[ci];
        if (!pp)
            continue;
        Pool& p = *pp;
        for (uint32_t& h : p.handles) {
            if (handleIsNode(h))
                h += uint32_t(nodeOff << 2);
            else if (handleIsBrick(h))
                h += uint32_t(bOff << 2);
        }
        uint32_t r = uint32_t(m_gpu.chunkGrid[ci]);
        if (handleIsNode(r))
            r += uint32_t(nodeOff << 2);
        else if (handleIsBrick(r))
            r += uint32_t(bOff << 2);
        m_gpu.chunkGrid[ci] = r == kEmptyHandle ? -1 : int32_t(r);
        for (uint32_t& cb : p.childBase)
            cb += uint32_t(hOff);
        m_gpu.handles.insert(m_gpu.handles.end(), p.handles.begin(), p.handles.end());
        m_gpu.childBase.insert(m_gpu.childBase.end(), p.childBase.begin(),
                               p.childBase.end());
        m_gpu.payload.insert(m_gpu.payload.end(), p.payload.begin(), p.payload.end());
        m_gpu.bricks.insert(m_gpu.bricks.end(), p.bricks.begin(), p.bricks.end());
        nodeOff += p.payload.size();
        hOff += p.handles.size();
        bOff += p.bricks.size() / BRICK_WORDS;
        delete pp;
    }
    pools.clear();

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
                 " active, {:.1f} MB in {:.2f}s",
                 m_stats.records, m_stats.nodes, m_stats.bricks, m_stats.activeChunks,
                 GRID_N * GRID_N * GRID_N,
                 double(m_stats.memoryBytes) / (1024.0 * 1024.0), m_stats.buildSeconds);

    // temporary synthesis grids no longer needed once the SVO is built
    m_blockSolid.clear();
    m_blockSolid.shrink_to_fit();
    m_objCells.clear();
    m_objCells.shrink_to_fit();
    m_objMats.clear();
    m_objMats.shrink_to_fit();
    return true;
}

} // namespace vf::voxel
