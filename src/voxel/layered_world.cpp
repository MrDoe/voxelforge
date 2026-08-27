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
    const glm::vec3& c = kPalette[std::min(int(mat), 16)];
    return uint32_t(c.r * 255.f) | (uint32_t(c.g * 255.f) << 8) |
           (uint32_t(c.b * 255.f) << 16);
}
inline uint32_t paletteWordHi(uint8_t mat, bool isObj = false)
{
    const glm::vec2& rr = kMaterialReflection[std::min(int(mat), 16)];
    return 255u | (uint32_t(rr.x) << 8) | (uint32_t(rr.y) << 16) |
           (uint32_t(mat) | (isObj ? 0x80u : 0u)) << 24;
}

inline void wordsFromPacked(uint64_t p, int sdfRaw, bool isObj, uint32_t w[2])
{
    w[0] = uint32_t(p & 0xFFFFFFu) | (uint32_t(sdfRaw & 0xFF) << 24);
    w[1] = 255u | (uint32_t((p >> 24) & 0xFFu) << 8) |
           (uint32_t((p >> 32) & 0xFFu) << 16) |
           ((uint32_t((p >> 40) & 0xFFu) | (isObj ? 0x80u : 0u)) << 24);
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
                        wordsFromPacked(p, int(h.d / VOXEL), h.obj, w);
                    else {
                        w[0] = paletteWordLo(h.mat) |
                               (uint32_t(int32_t(int(h.d / VOXEL)) & 0xFF) << 24);
                        w[1] = paletteWordHi(h.mat, h.obj);
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
    // b is in world meters; a chunk spans CHUNK_N cells = CHUNK_N * VOXEL m
    const float chunkW = float(CHUNK_N) * VOXEL;
    auto toChunk = [&](float x) {
        int c = int(std::floor((x + 0.5f * WORLD) / chunkW));
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
    bool full = true;
    std::vector<int> dirty;
    double readMs = 0, fieldMs = 0;
    if (!parseAndComputeDirty(full, dirty, readMs, fieldMs)) {
        m_loaded = false;
        return false;
    }
    auto tSvo = std::chrono::steady_clock::now();
    if (!rebuildNow(full, dirty, glm::vec3(0))) {
        m_loaded = false;
        return false;
    }
    double svoMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - tSvo)
                       .count();
    spdlog::info("layered_world: load {:.0f} ms (read {:.0f}, field {:.0f}, svo {:.0f})",
                 readMs + fieldMs + svoMs, readMs, fieldMs, svoMs);
    m_hasFullBuild = true;
    m_loaded = true;
    return true;
}

bool LayeredWorld::parseAndComputeDirty(bool& outFull, std::vector<int>& outDirty,
                                        double& outReadMs, double& outFieldMs)
{
    auto tLoad = std::chrono::steady_clock::now();
    std::vector<worldfile::WorldLayer> manifest;
    if (!worldfile::loadManifest(m_manifestPath, manifest))
        return false;

    std::string dir = m_manifestPath;
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
    m_carveCells.clear();
    m_carveMats.clear();
    m_blockSolid.clear();

    std::unordered_set<uint32_t> claimed;
    claimed.reserve(1u << 22);

    auto addSig = [&](const std::string& file) -> unsigned long long {
        std::filesystem::path p = std::filesystem::path(dir) / file;
        std::error_code ec;
        unsigned long long sz = static_cast<unsigned long long>(
            ec ? 0ull : std::filesystem::file_size(p, ec));
        unsigned long long sig = sigOf(p) ^ (sz * 0x9E3779B97F4A7C15ull);
        m_signatures.push_back({ p.string(), sigOf(p), sz });
        return sig;
    };

    addSig(m_manifestPath);

    // per-layer record AABB + enabled-state + content signature, used to
    // derive the dirty chunk set for incremental rebuilds.
    std::map<std::string, WorldAABB> curBox;
    std::map<std::string, bool> curEnabled;
    std::map<std::string, unsigned long long> curSig;
    std::string landscapeFile;
    for (const auto& l : manifest) {
        curEnabled[l.file] = l.enabled;
        if (l.name == "landscape" || l.file == "landscape.vxw")
            landscapeFile = l.file;
    }

    for (const worldfile::WorldLayer& l : manifest) {
        // remember every non-packed layer (enabled or not) so GUI persistence
        // can never silently delete a temporarily disabled layer; dedupe by
        // file (first entry wins) so corrupt manifests cannot multiply rows
        if (l.role == "packed")
            continue;
        bool known = false;
        for (const auto& k : m_layersMeta)
            known |= (k.file == l.file);
        if (!known)
            m_layersMeta.push_back(l);
        if (!l.enabled)
            continue;
        // parse-cache: only re-read layers whose mtime+size changed
        const std::string path = dir + l.file;
        std::error_code ec;
        unsigned long long sz =
            static_cast<unsigned long long>(std::filesystem::file_size(path, ec));
        unsigned long long mt = sigOf(path);
        LayerCacheEntry& ce = m_layerCache[l.file];
        const std::vector<VoxelRecord>* vox = nullptr;
        if (ce.valid && ce.mtime == mt && ce.size == sz) {
            vox = &ce.voxels;
        } else {
            WorldFileData data;
            if (!worldfile::read(path, data)) {
                spdlog::error("layered_world: cannot read layer '{}'", l.file);
                ce.valid = false;
                continue;
            }
            if (data.meta.worldSize != expected.worldSize ||
                data.meta.voxelSize != expected.voxelSize ||
                data.meta.gridN != expected.gridN) {
                spdlog::error("layered_world: layer '{}' meta mismatch", l.file);
                ce.valid = false;
                continue;
            }
            ce.mtime = mt;
            ce.size = sz;
            ce.voxels = std::move(data.voxels);
            ce.valid = true;
            vox = &ce.voxels;
        }
        curSig[l.file] = addSig(l.file);
        const bool isLandscape = (l.name == "landscape" || l.file == "landscape.vxw");
        const bool isCarve = (l.role == "carve");
        WorldAABB& box = curBox[l.file];
        for (const VoxelRecord& v : *vox) {
            uint32_t key = cellKey(v.x, v.y, v.z);
            glm::vec3 wp = v.position(expected);
            box.lo = glm::min(box.lo, wp);
            box.hi = glm::max(box.hi, wp);
            if (isCarve) {
                // subtractive field: carve cells are fed to VoxelField::build as a
                // separate (carved) volume and never enter the merged record set.
                m_carveCells.push_back(key);
                m_carveMats.push_back(v.materialId);
                continue;
            }
            if (claimed.insert(key).second)
                m_records.push_back(v);
            if (isLandscape) {
                int16_t& top = m_colTop[size_t(v.z) * kLatN + size_t(v.x)];
                if (v.y > top) {
                    top = int16_t(v.y);
                    m_colMat[size_t(v.z) * kLatN + size_t(v.x)] = v.materialId;
                }
            } else {
                m_objCells.push_back(key);
                m_objMats.push_back(v.materialId);
            }
        }
    }

    // --- decide full vs incremental rebuild -------------------------------
    // Toggles stay incremental: only layers whose enabled-state, record AABB
    // or file content changed mark their chunk range dirty.
    const double readMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tLoad)
                              .count();
    auto tField = std::chrono::steady_clock::now();
    bool full = !m_hasFullBuild;
    WorldAABB dirty;
    bool dirtyValid = false;
    for (const auto& kv : curEnabled) {
        bool changed = true;
        auto pit = m_prevEnabled.find(kv.first);
        if (pit != m_prevEnabled.end() && pit->second == kv.second) {
            auto pbox = m_prevBox.find(kv.first);
            auto cbox = curBox.find(kv.first);
            auto psig = m_prevSig.find(kv.first);
            auto csig = curSig.find(kv.first);
            bool sigSame =
                (psig == m_prevSig.end()) == (csig == curSig.end()) &&
                (psig == m_prevSig.end() || psig->second == csig->second);
            if (sigSame && pbox != m_prevBox.end() && cbox != curBox.end() &&
                pbox->second.lo == cbox->second.lo && pbox->second.hi == cbox->second.hi)
                changed = false;
        }
        if (changed) {
            if (kv.first == landscapeFile)
                full = true;
            // dirty whatever box we know: the current one (enabled / content
            // change) or the previous one (layer just disabled - its chunks
            // must be rebuilt or the old geometry would linger)
            const WorldAABB* b = nullptr;
            auto cbox = curBox.find(kv.first);
            if (cbox != curBox.end())
                b = &cbox->second;
            else {
                auto pbox = m_prevBox.find(kv.first);
                if (pbox != m_prevBox.end())
                    b = &pbox->second;
            }
            if (b) {
                dirty.lo = glm::min(dirty.lo, b->lo);
                dirty.hi = glm::max(dirty.hi, b->hi);
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

    const double fieldMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - tField)
                               .count();

    outFull = full;
    outDirty = std::move(dirtyChunks);
    outReadMs = readMs;
    outFieldMs = fieldMs;

    m_prevBox = std::move(curBox);
    m_prevEnabled = std::move(curEnabled);
    m_prevSig = std::move(curSig);
    return true;
}

LayeredWorld::ReloadResult LayeredWorld::reloadIfChanged(glm::vec3 cam)
{
    if (!m_loaded)
        return kNone;
    for (const Signature& s : m_signatures) {
        std::error_code ec;
        unsigned long long sz = static_cast<unsigned long long>(
            ec ? 0ull : std::filesystem::file_size(s.path, ec));
        if (ec || sigOf(s.path) != s.mtime || sz != s.size) {
            spdlog::info("layered_world: '{}' changed on disk, rebuilding", s.path);
            bool full = true;
            std::vector<int> dirty;
            double r = 0, f = 0;
            if (!parseAndComputeDirty(full, dirty, r, f))
                return kNone;
            return kick(full, std::move(dirty), cam);
        }
    }
    return kNone;
}

bool LayeredWorld::rebuildNow(bool full, const std::vector<int>& dirty, glm::vec3 cam)
{
    std::vector<std::unique_ptr<ChunkPool>> newPools;
    Stats s;
    if (!buildInto(full, dirty, &m_pools, cam, m_gpu, newPools, m_field, s))
        return false;
    m_pools = std::move(newPools);
    m_stats = s;
    return true;
}

LayeredWorld::ReloadResult LayeredWorld::kick(bool full, std::vector<int> dirty, glm::vec3 cam)
{
    if (m_rebuildRunning)
        return kNone;
    if (m_rebuildThread.joinable())
        m_rebuildThread.join();
    if (getenv("VF_SYNC_RELOAD")) {
        if (!rebuildNow(full, dirty, cam))
            return kNone;
        return kSyncDone;
    }
    m_rebuildRunning = true;
    m_rebuildDone = false;
    m_rebuildThread = std::thread([this, full, dirty, cam]() {
        std::vector<std::unique_ptr<ChunkPool>> newPools;
        GpuWorld g;
        VoxelField f;
        Stats s;
        buildInto(full, dirty, &m_pools, cam, g, newPools, f, s);
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            m_pending = std::make_unique<PendingBuild>();
            m_pending->gpu = std::move(g);
            m_pending->pools = std::move(newPools);
            m_pending->field = std::move(f);
            m_pending->stats = s;
        }
        m_rebuildDone = true;
        m_rebuildRunning = false;
    });
    return kStartedAsync;
}

void LayeredWorld::requestReload(glm::vec3 cam, bool full)
{
    if (m_rebuildRunning)
        return;
    std::vector<int> dirty; // empty => full
    bool f = full;
    double r = 0, ff = 0;
    if (parseAndComputeDirty(f, dirty, r, ff))
        kick(full, std::move(dirty), cam);
}

bool LayeredWorld::consumeRebuild()
{
    if (!m_rebuildDone.load(std::memory_order_acquire))
        return false;
    std::unique_lock<std::mutex> lk(m_pendingMtx);
    if (!m_pending) {
        m_rebuildDone = false;
        return false;
    }
    m_gpu = std::move(m_pending->gpu);
    m_pools = std::move(m_pending->pools);
    m_field = std::move(m_pending->field);
    m_stats = m_pending->stats;
    m_pending.reset();
    lk.unlock();
    m_rebuildDone = false;
    return true;
}

bool LayeredWorld::buildInto(bool full, const std::vector<int>& dirty,
                              std::vector<std::unique_ptr<ChunkPool>>* prevPools,
                              glm::vec3 camPos, GpuWorld& outGpu,
                              std::vector<std::unique_ptr<ChunkPool>>& outPools,
                              VoxelField& outField, Stats& outStats)
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
    ctx.field = &outField;
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
    outField.build(m_records, m_colTop, m_colMat, m_objCells, m_objMats, m_carveCells, m_carveMats);

    // global presence grid (record cells + object interiors) for the SVO builder
    const size_t gBlocks = size_t(kGBlocks) * kGBlocks * kGBlocks;
    std::vector<uint8_t> blockSolid(gBlocks, 0);
    auto markBlock = [&](int x, int y, int z) {
        size_t bi = (size_t(z / kBlockSize) * kGBlocks + size_t(y / kBlockSize)) *
                        kGBlocks +
                    size_t(x / kBlockSize);
        blockSolid[bi] = 1;
    };
    for (const VoxelRecord& v : m_records)
        markBlock(v.x, v.y, v.z);
    const std::vector<uint8_t>& objMask = outField.objectBlockMask();
    for (size_t i = 0; i < gBlocks; ++i)
        blockSolid[i] |= objMask[i];

    ctx.blockSolid = blockSolid.data();

    for (size_t i = 0; i < m_records.size(); ++i) {
        const VoxelRecord& v = m_records[i];
        size_t ci = (size_t(v.z) / CHUNK_N) * GRID_N * GRID_N +
                    (size_t(v.y) / CHUNK_N) * GRID_N + size_t(v.x) / CHUNK_N;
        ctx.chunkRecords[ci].push_back(uint32_t(i));
    }

    const size_t numChunks = ctx.chunkRecords.size();
    outPools.clear();
    outPools.resize(numChunks);

    std::unordered_set<int> dirtySet(dirty.begin(), dirty.end());

    // camera-distance-priority visit order: nearest chunks are built first so a
    // rebuild (especially an incremental one) makes the visible volume correct
    // before the far field.
    std::vector<size_t> order(numChunks);
    for (size_t ci = 0; ci < numChunks; ++ci)
        order[ci] = ci;
    {
        const float chunkM = CHUNK_N * VOXEL;
        const float half = WORLD * 0.5f;
        std::vector<float> dist(numChunks);
        for (size_t ci = 0; ci < numChunks; ++ci) {
            int cz = int(ci / (GRID_N * GRID_N));
            int cy = int((ci / GRID_N) % GRID_N);
            int cx = int(ci % GRID_N);
            glm::vec3 c = (glm::vec3(float(cx), float(cy), float(cz)) + 0.5f) * chunkM -
                          glm::vec3(half);
            glm::vec3 d = c - camPos;
            dist[ci] = glm::dot(d, d);
        }
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return dist[a] < dist[b]; });
    }

    std::atomic<size_t> next{ 0 };
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < hc; ++t)
        threads.emplace_back([&] {
            for (;;) {
                size_t oi = next.fetch_add(1);
                if (oi >= numChunks)
                    return;
                size_t ci = order[oi];
                // reuse the cached pool unless this chunk must be rebuilt
                if (!full && dirtySet.find(int(ci)) == dirtySet.end()) {
                    if (prevPools && (*prevPools)[ci])
                        outPools[ci] = std::move((*prevPools)[ci]);
                    continue;
                }
                if (ctx.chunkRecords[ci].empty()) {
                    // no records: drop any previous geometry (the post-process
                    // step may promote fully-below-ground chunks to solid)
                    outPools[ci].reset();
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
                outPools[ci] = std::move(pool);
            }
        });
    for (auto& th : threads)
        th.join();

    // --- deterministic merge across per-chunk pools (non-mutating) ---
    outGpu = GpuWorld{};
    outGpu.chunkGrid.assign(numChunks, -1);
    size_t nodeOff = 0, hOff = 0, bOff = 0;
    for (size_t ci = 0; ci < numChunks; ++ci) {
        ChunkPool* pp = outPools[ci].get();
        if (!pp || pp->root < 0) {
            outGpu.chunkGrid[ci] = -1;
            continue;
        }
        // copy + offset-adjust handles into outGpu; the pool stays pristine so it
        // can be reused in a later incremental rebuild.
        size_t hStart = outGpu.handles.size();
        outGpu.handles.insert(outGpu.handles.end(), pp->handles.begin(), pp->handles.end());
        for (size_t k = hStart; k < outGpu.handles.size(); ++k) {
            uint32_t& h = outGpu.handles[k];
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
        outGpu.chunkGrid[ci] = r == kEmptyHandle ? -1 : int32_t(r);
        size_t cStart = outGpu.childBase.size();
        outGpu.childBase.insert(outGpu.childBase.end(), pp->childBase.begin(),
                                pp->childBase.end());
        for (size_t k = cStart; k < outGpu.childBase.size(); ++k)
            outGpu.childBase[k] += uint32_t(hOff);
        outGpu.payload.insert(outGpu.payload.end(), pp->payload.begin(), pp->payload.end());
        outGpu.bricks.insert(outGpu.bricks.end(), pp->bricks.begin(), pp->bricks.end());
        nodeOff += pp->payload.size();
        hOff += pp->handles.size();
        bOff += pp->bricks.size() / BRICK_WORDS;
    }

    // chunks without any record: fully-below-ground becomes a solid terminal,
    // everything else stays an empty root
    for (size_t ci = 0; ci < numChunks; ++ci) {
        if (outGpu.chunkGrid[ci] != -1 || !ctx.chunkRecords[ci].empty())
            continue;
        int cz = int(ci / (GRID_N * GRID_N));
        int cy = int((ci / GRID_N) % GRID_N);
        int cx = int(ci % GRID_N);
        int cls = classifyBox(ctx, nullptr, false, cx * CHUNK_N, cy * CHUNK_N,
                              cz * CHUNK_N, CHUNK_N);
        outGpu.chunkGrid[ci] = cls == 1 ? int32_t(kSolidHandle) : -1;
    }

    outStats.records = m_records.size();
    outStats.nodes = outGpu.payload.size();
    outStats.bricks = outGpu.bricks.size() / BRICK_WORDS;
    outStats.activeChunks =
        size_t(std::count_if(outGpu.chunkGrid.begin(), outGpu.chunkGrid.end(),
                             [](int32_t v) { return v >= 0; }));
    outStats.memoryBytes = outGpu.memoryBytes();
    outStats.buildSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (getenv("VF_TRACE")) {
        auto vecHash = [](const std::vector<uint32_t>& v) {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t x : v)
                h = (h ^ x) * 1099511628211ull;
            return h;
        };
        std::vector<uint32_t> gridWords(outGpu.chunkGrid.begin(), outGpu.chunkGrid.end());
        spdlog::info("layered_world hash: grid={:x} nodes={:x} handles={:x} bricks={:x}",
                     vecHash(gridWords), vecHash(outGpu.payload),
                     vecHash(outGpu.handles), vecHash(outGpu.bricks));
    }

    spdlog::info("layered_world: {} records -> {} nodes, {} bricks, {}/{} chunks"
                 " active, {:.1f} MB in {:.2f}s{}",
                 outStats.records, outStats.nodes, outStats.bricks, outStats.activeChunks,
                 GRID_N * GRID_N * GRID_N,
                 double(outStats.memoryBytes) / (1024.0 * 1024.0), outStats.buildSeconds,
                 full ? "" : " (incremental)");
    return true;
}

LayeredWorld::~LayeredWorld()
{
    if (m_rebuildThread.joinable())
        m_rebuildThread.join();
}

} // namespace vf::voxel
