// The surfel stream: re-bake it from the live field, bucket the world-wide
// water plane per chunk, and patch the height texture after a live edit.
#include "app/app.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <numeric>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Identify the splats that make up one lattice cell, for the "floating splat"
// investigation. Pure: it builds a throwaway 3x3x3 range around the cell (so
// the support test can see the neighbours that live in adjacent cells) and
// touches no live state.
//
// The support test is the one the bake audit uses: a splat is FLOATING when no
// other splel's disk comes within reach, and its own cell is not counted
// twice. `loneVoxel` is the user's exception - a genuinely single solid voxel
// in space is meant to be visible, so it is reported but not called a defect.
// List every splal in the given chunks that no other splat reaches, with the
// stable id. This is the way to NAME a floating splat: a detached disk sits in
// mid-air, so the Ctrl+LMB pick ray passes through it and hits the solid behind
// it, meaning you cannot click one directly.
//
// Support is evaluated over a 3-cell ring in EVERY direction including the
// chunk boundary: the neighbours of a boundary disk live in the ADJACENT
// chunk's run, so a chunk-local test reports every boundary disk as floating.
// Neighbour chunks are seeded on demand from the store (a pure cache fill).
void App::listFloatingSurfels()
{
    auto& store = m_layers.store();
    const int CH = vf::voxel::CHUNK_N;
    const float half = 0.5f * vf::voxel::WORLD;
    const float vs = vf::voxel::VOXEL;
    auto cellKey = [](int cx, int cy, int cz) {
        return (uint64_t(std::uint32_t(cx)) << 20) | (uint32_t(cy) << 10) |
               uint32_t(cz);
    };
    const std::vector<int> edited = store.editedChunks();
    spdlog::info("list-floating: scanning {} edited chunk(s) with a 1-chunk ring",
                 edited.size());
    if (edited.empty()) {
        spdlog::info("list-floating: no edited chunks - pass the overlay that "
                     "reproduces the artifact");
        return;
    }
    // Collect the chunk set: the edited ones plus their 26 neighbours.
    std::vector<int> want(edited);
    for (int c : edited) {
        int cx, cy, cz;
        vf::voxel::chunkCoordsOf(c, cx, cy, cz);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= 16 || ny >= 16 ||
                        nz >= 16)
                        continue;
                    want.push_back(vf::voxel::chunkIndexOf(nx, ny, nz));
                }
    }
    std::sort(want.begin(), want.end());
    want.erase(std::unique(want.begin(), want.end()), want.end());

    // One flat, world-bucketed list of the run of every chunk in the set.
    struct E { vf::voxel::Surfel s; uint64_t key; uint32_t seg, sub; int chunk; };
    std::vector<E> all;
    std::unordered_map<int, std::vector<int>> byChunk;
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.edgeBridgeSize = m_edgeBridgeSize;
    sp.cornerFill = m_cornerFill;
    sp.sunDir = glm::vec3(m_sunDir);
    sp.anisotropy = true;
    for (int c : want) {
        int cx, cy, cz;
        vf::voxel::chunkCoordsOf(c, cx, cy, cz);
        const glm::ivec3 lo(cx * CH, cy * CH, cz * CH);
        const glm::ivec3 hi(lo.x + CH, lo.y + CH, lo.z + CH);
        vf::voxel::SurfelRange rg =
            vf::voxel::buildChunkSurfelsRange(store, c, lo, hi, sp);
        const int n0 = int(all.size());
        for (size_t i = 0; i < rg.surfels.size(); ++i)
            all.push_back({rg.surfels[i], rg.keys[i], 0u, uint32_t(i), c});
        for (size_t i = 0; i < rg.edgeSurfels.size(); ++i)
            all.push_back({rg.edgeSurfels[i],
                           i < rg.edgeKeys.size() ? rg.edgeKeys[i] : uint64_t(0),
                           1u, uint32_t(i), c});
        byChunk[c].push_back(n0);
        byChunk[c].push_back(int(all.size()));   // [first, last)
    }

    // Buckets over every splat in the set, so a disk sees its neighbours even
    // when they belong to a different chunk's run.
    std::unordered_map<uint64_t, std::vector<int>> bucket;
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& s = all[i].s;
        bucket[cellKey(int((s.pos_rU.x + half) / vs), int((s.pos_rU.y + half) / vs),
                       int((s.pos_rU.z + half) / vs))]
            .push_back(int(i));
    }
    static const char* segName[2] = { "base", "edge" };
    static const int kProbe[27][3] = {
        {-1,-1,-1},{0,-1,-1},{1,-1,-1},{-1,0,-1},{1,0,-1},{-1,1,-1},{0,1,-1},{1,1,-1},
        {-1,-1, 0},{1,-1, 0},{-1, 1, 0},{1, 1, 0},
        {-1,-1, 1},{0,-1, 1},{1,-1, 1},{-1,0, 1},{1,0, 1},{-1,1, 1},{0,1, 1},{1,1, 1},
        {-1, 0, 0},{1, 0, 0},{0,-1, 0},{0, 1, 0},{0, 0,-1},{0, 0, 1},{0, 0, 0}};

    size_t floating = 0, reported = 0;
    const size_t kMaxReport = 400;
    for (int c : edited) {
        auto it = byChunk.find(c);
        if (it == byChunk.end())
            continue;
        for (int ai = it->second[0]; ai < it->second[1]; ++ai) {
            const E& e = all[size_t(ai)];
            if (e.key == 0)
                continue;
            int ex, ey, ez;
            vf::voxel::surfelUnpackKey(e.key, ex, ey, ez);
            const float ra = std::max(e.s.pos_rU.w, e.s.normal_rV.w);
            bool touch = false;
            float margin = -1e30f, best = 0.0f, bestR = 0.0f;
            for (int d = 0; d < 27; ++d) {
                auto bt = bucket.find(cellKey(ex + kProbe[d][0], ey + kProbe[d][1],
                                               ez + kProbe[d][2]));
                if (bt == bucket.end())
                    continue;
                for (int bi : bt->second) {
                    if (bi == ai)
                        continue;
                    const E& o = all[size_t(bi)];
                    const float ro = std::max(o.s.pos_rU.w, o.s.normal_rV.w);
                    const float d2 = glm::distance(e.s.pos_rU, o.s.pos_rU);
                    const float m = (ra + ro) - d2;
                    if (m > margin) { margin = m; best = d2; bestR = ro; }
                    if (m > 0.0f)
                        touch = true;
                }
            }
            if (touch)
                continue;
            ++floating;
            if (reported < kMaxReport) {
                ++reported;
                spdlog::info("  FLOATING id={} chunk={} seg={} sub={} cell=({},{},{}) "
                             "mat={} rU={:.4f} rV={:.4f} pos=({:.3f},{:.3f},{:.3f}) "
                             "owner={} nearest={:.4f}(r {:.4f})",
                             vf::voxel::surfelId(e.key, e.seg, e.sub), c,
                             segName[e.seg], e.sub, ex, ey, ez,
                             int(e.s.mat_ao.x + 0.5f), e.s.pos_rU.w,
                             e.s.normal_rV.w, e.s.pos_rU.x, e.s.pos_rU.y,
                             e.s.pos_rU.z,
                             vf::voxel::surfelLayerId(e.s.mat_ao.w), best, bestR);
            }
        }
    }
    spdlog::info("list-floating: {} unconnected splat(s) in {} edited chunk(s) "
                 "(searched {} splats in {} chunks, reported {})", floating,
                 edited.size(), all.size(), want.size(), reported);
}

void App::describeSurfelsAt(const glm::ivec3& cell)
{
    auto& store = m_layers.store();
    const int CH = vf::voxel::CHUNK_N;
    const int cx = cell.x / CH, cy = cell.y / CH, cz = cell.z / CH;
    const int chunk = vf::voxel::chunkIndexOf(cx, cy, cz);

    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.edgeBridgeSize = m_edgeBridgeSize;
    sp.cornerFill = m_cornerFill;
    sp.sunDir = glm::vec3(m_sunDir);
    sp.anisotropy = true;

    // 3x3x3 around the cell: the cell itself plus the ring the support test
    // needs. Clamped to the chunk the range builder already clamps to.
    const glm::ivec3 lo(cell.x - 1, cell.y - 1, cell.z - 1);
    const glm::ivec3 hi(cell.x + 2, cell.y + 2, cell.z + 2);
    const int latN = store.latN();
    spdlog::info("  [ctx] chunk={} cell=({},{},{}) bounds {}x{}x{} latN={} "
                 "centreSolid={}", chunk, cell.x, cell.y, cell.z,
                 hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, latN,
                 store.cellAt(cell.x, cell.y, cell.z).solid ? 1 : 0);
    vf::voxel::SurfelRange rg =
        vf::voxel::buildChunkSurfelsRange(store, chunk, lo, hi, sp);
    spdlog::info("  [ctx] range -> {} parents, {} edges, {} keys", rg.surfels.size(),
                 rg.edgeSurfels.size(), rg.keys.size());
    // One flat list with the segment + sub-index each splat needs for its ID.
    struct Entry { vf::voxel::Surfel s; uint64_t key; uint32_t seg, sub; };
    std::vector<Entry> all;
    all.reserve(rg.surfels.size() + rg.edgeSurfels.size());
    for (size_t i = 0; i < rg.surfels.size(); ++i)
        all.push_back({rg.surfels[i], rg.keys[i], 0u, uint32_t(i)});
    for (size_t i = 0; i < rg.edgeSurfels.size(); ++i)
        all.push_back({rg.edgeSurfels[i],
                       i < rg.edgeKeys.size() ? rg.edgeKeys[i] : uint64_t(0), 1u,
                       uint32_t(i)});

    static const char* segName[2] = { "base", "edge" };
    int floating = 0, atCell = 0;
    // Per-cell tally across the whole 3x3x3, so an interior or off-surface
    // click still shows the neighbourhood instead of an empty report.
    std::map<std::array<int, 3>, std::array<int, 2>> tally;  // {count, floating}
    for (const Entry& e : all) {
        int ex, ey, ez;
        vf::voxel::surfelUnpackKey(e.key, ex, ey, ez);
        if (e.key == 0)
            continue;   // unattributed splat: no cell to report it under
        const vf::voxel::Surfel& a = e.s;
        const float ra = std::max(a.pos_rU.w, a.normal_rV.w);
        // Support = SOME other disk reaches this one. Test EVERY neighbour and
        // keep the best contact margin (sum of radii minus distance): taking
        // the single nearest-by-DISTANCE and testing only that one lets a
        // close-but-tiny splat hide the fat parent that actually overlaps,
        // which reported ordinary sand grains as floating.
        float margin = -1e30f, best = 0.0f, bestR = 0.0f;
        bool touch = false;
        for (const Entry& o : all) {
            if (&o == &e)
                continue;
            const float d = glm::distance(a.pos_rU, o.s.pos_rU);
            const float ro = std::max(o.s.pos_rU.w, o.s.normal_rV.w);
            const float m = (ra + ro) - d;
            if (m > margin) {
                margin = m;
                best = d;
                bestR = ro;
            }
            if (m > 0.0f)
                touch = true;
        }
        auto& t = tally[{ex, ey, ez}];
        t[0] += 1;
        if (!touch)
            t[1] += 1;
        const bool here = (ex == cell.x && ey == cell.y && ez == cell.z);
        if (here)
            ++atCell;
        if (!touch)
            ++floating;
        // Detail for the clicked cell, and for every FLOATING splat anywhere in
        // the box - those are the ones being hunted.
        if (here || !touch)
            spdlog::info("  surfel id={} seg={} sub={} cell=({},{},{}){} mat={} "
                         "rU={:.4f} rV={:.4f} pos=({:.3f},{:.3f},{:.3f}) "
                         "n=({:.2f},{:.2f},{:.2f}) owner={} nearest={:.4f}(r {:.4f}) {}",
                         vf::voxel::surfelId(e.key, e.seg, e.sub), segName[e.seg],
                         e.sub, ex, ey, ez, here ? " <PICKED>" : "", 
                         int(a.mat_ao.x + 0.5f), a.pos_rU.w,
                         a.normal_rV.w, a.pos_rU.x, a.pos_rU.y, a.pos_rU.z,
                         a.normal_rV.x, a.normal_rV.y, a.normal_rV.z,
                         vf::voxel::surfelLayerId(a.mat_ao.w), best, bestR,
                         touch ? "supported" : "FLOATING");
    }
    for (const auto& [k, t] : tally) {
        spdlog::info("  cell ({},{},{}) -> {} splats, {} floating{}", k[0], k[1],
                     k[2], t[0], t[1],
                     (k[0] == cell.x && k[1] == cell.y && k[2] == cell.z)
                         ? " <PICKED>" : "");
    }
    // The user's exception: is this cell a genuinely single solid voxel?
    bool lone = store.cellAt(cell.x, cell.y, cell.z).solid;
    if (lone) {
        static const int sd[6][3] = {{1,0,0},{-1,0,0},{0,1,0},
                                     {0,-1,0},{0,0,1},{0,0,-1}};
        const int latN = store.latN();
        for (int d = 0; d < 6 && lone; ++d) {
            const int nx = cell.x + sd[d][0], ny = cell.y + sd[d][1],
                      nz = cell.z + sd[d][2];
            if (nx < 0 || nx >= latN || ny < 0 || ny >= latN || nz < 0 ||
                nz >= latN || store.cellAt(nx, ny, nz).solid)
                lone = false;
        }
    }
    spdlog::info("surfel report: cell ({},{},{}) -> {} splats in cell, {} "
                 "FLOATING, loneVoxel={} ({} splats searched)",
                 cell.x, cell.y, cell.z, atCell, floating, lone, all.size());
}

void App::rebuildSurfels()
{
    if (!m_layers.loaded())
        return;
    vkDeviceWaitIdle(m_ctx.device());
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.edgeBridgeSize = m_edgeBridgeSize;
    sp.cornerFill = m_cornerFill;
    sp.sunDir = glm::vec3(m_sunDir);
    // LOD rings: baked 2x2x2 / 4x4x4 merged-terrain surfel runs per chunk;
    // the renderer picks a ring per chunk by distance (VF_LOD1/VF_LOD2).
    sp.lodRings = true;
    if (const char* e = getenv("VF_LOD"))
        sp.lodRings = atoi(e) != 0;
    // material-split LOD merging (VF_LOD_SPLIT=0: single majority disk)
    sp.lodMaterialSplit = true;
    if (const char* e = getenv("VF_LOD_SPLIT"))
        sp.lodMaterialSplit = atoi(e) != 0;
    // anisotropic footprints: disks stretch along local creases (VF_ANISO=0)
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    // debug/experiment overrides for the surfel bake (default = tuned values)
    if (const char* e = getenv("VF_FLOAT_DROP"))
        sp.dropFloating = atoi(e) != 0;
    if (const char* e = getenv("VF_SURFEL_SMOOTH"))
        sp.smoothNormals = atoi(e) != 0;
    if (const char* e = getenv("VF_SURFEL_HFBLEND"))
        sp.terrainHeightfieldNormals = atoi(e) != 0;
    vf::voxel::SurfelSet set = vf::voxel::buildSurfels(m_layers.field(), sp);
    // The water is ONE fixed-level plane subdivided into a world-wide 0.2 m
    // grid of coplanar surfels (coverage only - the shader intersects the
    // analytic plane per fragment). The depth test against the opaque prepass
    // hides the cells standing on dry land and objects, so a live dig below
    // the level is water with no per-column bookkeeping and every water
    // fragment shades identically. Bucket the run per chunk (deterministic
    // order) so the draw code can frustum-cull.
    m_waterSurfels = vf::voxel::buildWaterSurfels();
    const uint32_t waterStart = uint32_t(set.surfels.size());
    m_waterRel = updateWaterBuckets();
    std::vector<uint32_t> waterRange(m_waterRel.size(), waterStart);
    for (size_t i = 0; i < m_waterRel.size(); ++i)
        waterRange[i] = waterStart + m_waterRel[i];
    set.surfels.resize(set.surfels.size() + m_waterSurfels.size());
    std::copy(m_waterSurfels.begin(), m_waterSurfels.end(),
              set.surfels.begin() + waterStart);
    // chunkRange only covers opaque surfels; waterRange buckets the trailing
    // water run per chunk for frustum-culled water draws
    m_splatPass.setSurfels(set.surfels.data(),
                           set.surfels.size() * sizeof(vf::voxel::Surfel),
                           set.surfels.size(), set.chunkRange, waterStart, waterRange,
                           set.edgeStart, set.lod1Range, set.lod2Range);
    spdlog::info("splat backend: {} surfels ({} water, {} edge bridges on {} parents), "
                 "{} chunks, lod1 {} lod2 {}",
                 set.surfels.size(), m_waterSurfels.size(), set.edgeBridgeCount,
                 set.edgeParentCount,
                 set.chunkRange.empty() ? 0 : set.chunkRange.size() - 1,
                 set.lod1Count, set.lod2Count);
}

std::vector<uint32_t> App::updateWaterBuckets()
{
    constexpr uint32_t kChunks = 16 * 16 * 16;
    m_waterRel.assign(kChunks + 1, 0);
    auto chunkOf = [](const vf::voxel::Surfel& s) {
        const float px = s.pos_rU.x, py = s.pos_rU.y, pz = s.pos_rU.z;
        const auto ax = std::clamp(int(std::floor((px + 51.2f) / 6.4f)), 0, 15);
        const auto ay = std::clamp(int(std::floor((py + 51.2f) / 6.4f)), 0, 15);
        const auto az = std::clamp(int(std::floor((pz + 51.2f) / 6.4f)), 0, 15);
        return uint32_t(vf::voxel::chunkIndexOf(ax, ay, az));
    };
    if (m_waterSurfels.empty())
        return m_waterRel;
    std::vector<uint32_t> order(m_waterSurfels.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return chunkOf(m_waterSurfels[a]) < chunkOf(m_waterSurfels[b]);
    });
    std::vector<vf::voxel::Surfel> sorted;
    sorted.reserve(m_waterSurfels.size());
    uint32_t open = chunkOf(m_waterSurfels[order[0]]);
    m_waterRel[open] = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const uint32_t c = chunkOf(m_waterSurfels[order[k]]);
        if (c != open) {
            for (uint32_t f = open + 1; f <= c; ++f)
                m_waterRel[f] = uint32_t(k);
            open = c;
        }
        sorted.push_back(m_waterSurfels[order[k]]);
    }
    for (uint32_t f = open + 1; f < m_waterRel.size(); ++f)
        m_waterRel[f] = uint32_t(order.size());
    m_waterSurfels.swap(sorted);
    return m_waterRel;
}

void App::patchHeightTexture(int x0, int z0, int x1, int z1, int riseCells)
{
    auto& store = m_layers.store();
    const int latN = store.latN();
    if (latN <= 0 || m_heightCpu.size() != size_t(latN) * latN)
        return;
    x0 = std::clamp(x0, 0, latN - 1); x1 = std::clamp(x1, 0, latN - 1);
    z0 = std::clamp(z0, 0, latN - 1); z1 = std::clamp(z1, 0, latN - 1);
    if (x1 < x0 || z1 < z0)
        return;
    const float half = 0.5f * vf::voxel::WORLD;
    int topMost = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            // current (kept in sync) top -> lattice, then scan past the rise
            const float oldTop = m_heightCpu[size_t(z) * latN + size_t(x)].x;
            int y = (oldTop > -1e29f)
                        ? int(std::floor((oldTop + half) / vf::voxel::VOXEL)) - 1
                        : latN - 1;
            y = std::min(latN - 1, y + std::max(riseCells, 0));
            float topY = -1e30f;
            uint8_t mat = 0;
            for (; y >= 0; --y) {
                const vf::voxel::StoreCell c = store.cellAt(x, y, z);
                if (c.solid && !c.obj) {
                    topY = -half + (float(y) + 1.0f) * vf::voxel::VOXEL;
                    mat = c.mat;
                    break;
                }
            }
            m_heightCpu[size_t(z) * latN + size_t(x)] =
                glm::vec2(topY, float(mat) / 255.0f);
            topMost = std::max(topMost, y);
        }
    if (m_heightImg.img == VK_NULL_HANDLE)
        return;
    const uint32_t w = uint32_t(x1 - x0 + 1), h = uint32_t(z1 - z0 + 1);
    std::vector<glm::vec2> rect(size_t(w) * h);
    for (uint32_t z = 0; z < h; ++z)
        std::memcpy(rect.data() + size_t(z) * w,
                    m_heightCpu.data() + size_t(z0 + z) * latN + size_t(x0),
                    size_t(w) * sizeof(glm::vec2));
    if (!vf::uploadSubImage3D(m_ctx, m_heightImg, rect.data(), uint32_t(x0),
                              uint32_t(z0), w, h, sizeof(glm::vec2)))
        spdlog::warn("live edit: height-texture patch {}x{} at ({},{}) failed", w, h,
                     x0, z0);
}

} // namespace app
} // namespace vf
