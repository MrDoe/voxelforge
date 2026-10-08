#include "voxel/live_editor.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>
#include <utility>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace vf::voxel {

namespace {

void chunkBoundsOf(int chunk, glm::ivec3& lo, glm::ivec3& hi)
{
    int cx, cy, cz;
    chunkCoordsOf(chunk, cx, cy, cz);
    lo = glm::ivec3(cx * CHUNK_N, cy * CHUNK_N, cz * CHUNK_N);
    hi = lo + glm::ivec3(CHUNK_N);
}

bool keyInside(uint64_t k, glm::ivec3 lo, glm::ivec3 hi)
{
    const int x = int((k >> 20) & 0x3FFu), y = int((k >> 10) & 0x3FFu),
              z = int(k & 0x3FFu);
    return x >= lo.x && x < hi.x && y >= lo.y && y < hi.y && z >= lo.z && z < hi.z;
}

// Recover the lattice cell of a baked surfel: the emitter writes
// pos = cellCentre + normal * 0.5 * VOXEL, so stepping back along the normal
// and rounding to the lattice recovers the cell exactly.
uint64_t keyFromSurfel(const Surfel& s)
{
    glm::vec3 n = glm::vec3(s.normal_rV);
    const float l2 = glm::dot(n, n);
    n = (l2 > 1e-12f) ? n / std::sqrt(l2) : glm::vec3(0.f, 1.f, 0.f);
    const glm::vec3 c = glm::vec3(s.pos_rU) - n * (0.5f * VOXEL);
    const int x = int(std::lround((c.x + 0.5f * WORLD) / VOXEL - 0.5f));
    const int y = int(std::lround((c.y + 0.5f * WORLD) / VOXEL - 0.5f));
    const int z = int(std::lround((c.z + 0.5f * WORLD) / VOXEL - 0.5f));
    return (uint64_t(uint32_t(x)) << 20) | (uint32_t(y) << 10) | uint32_t(z);
}

} // namespace

void LiveEditor::seedFromStore(int chunk, const SurfelParams& params)
{
    glm::ivec3 lo, hi;
    chunkBoundsOf(chunk, lo, hi);
    SurfelRange rg = buildChunkSurfelsRange(*m_store, chunk, lo, hi, params);
    Cache& c = m_cache[chunk];
    c.keys = std::move(rg.keys);
    c.surfels = std::move(rg.surfels);
    c.edgeKeys = std::move(rg.edgeKeys);
    c.edgeSurfels = std::move(rg.edgeSurfels);
    c.runDirty = true;
}

void LiveEditor::seed(int chunk, const SurfelParams& params)
{
    if (m_seedFn) {
        SurfelRange rg = m_seedFn(chunk);
        if (!rg.surfels.empty()) {
            Cache& c = m_cache[chunk];
            c.keys.resize(rg.surfels.size());
            for (size_t i = 0; i < rg.surfels.size(); ++i)
                c.keys[i] = keyFromSurfel(rg.surfels[i]);
            c.surfels = std::move(rg.surfels);

            c.edgeKeys.resize(rg.edgeSurfels.size());
            for (size_t i = 0; i < rg.edgeSurfels.size(); ++i)
                c.edgeKeys[i] = keyFromSurfel(rg.edgeSurfels[i]);
            c.edgeSurfels = std::move(rg.edgeSurfels);

            auto sortSegment = [](std::vector<uint64_t>& keys,
                                  std::vector<Surfel>& values) {
                std::vector<size_t> order(keys.size());
                std::iota(order.begin(), order.end(), 0u);
                std::stable_sort(order.begin(), order.end(),
                                 [&](size_t a, size_t b) {
                                     return keys[a] < keys[b];
                                 });
                std::vector<uint64_t> sortedKeys(keys.size());
                std::vector<Surfel> sortedValues(values.size());
                for (size_t i = 0; i < order.size(); ++i) {
                    sortedKeys[i] = keys[order[i]];
                    sortedValues[i] = values[order[i]];
                }
                keys.swap(sortedKeys);
                values.swap(sortedValues);
            };
            sortSegment(c.keys, c.surfels);
            sortSegment(c.edgeKeys, c.edgeSurfels);
            c.runDirty = true;
            return;
        }
    }
    seedFromStore(chunk, params);
}

void LiveEditor::splice(int chunk, const SurfelRange& rg)
{
    Cache& c = m_cache[chunk];
    auto merge = [](std::vector<uint64_t>& keys, std::vector<Surfel>& values,
                    const std::vector<uint64_t>& addKeys,
                    const std::vector<Surfel>& addValues) {
        std::vector<std::pair<uint64_t, Surfel>> merged;
        merged.reserve(keys.size() + addKeys.size());
        for (size_t i = 0; i < keys.size(); ++i)
            merged.push_back({ keys[i], values[i] });
        for (size_t i = 0; i < addKeys.size(); ++i)
            merged.push_back({ addKeys[i], addValues[i] });
        std::stable_sort(merged.begin(), merged.end(),
                         [](const auto& a, const auto& b) {
                             return a.first < b.first;
                         });
        keys.resize(merged.size());
        values.resize(merged.size());
        for (size_t i = 0; i < merged.size(); ++i) {
            keys[i] = merged[i].first;
            values[i] = merged[i].second;
        }
    };
    merge(c.keys, c.surfels, rg.keys, rg.surfels);
    merge(c.edgeKeys, c.edgeSurfels, rg.edgeKeys, rg.edgeSurfels);
    c.runDirty = true;
}

bool LiveEditor::refreshRegion(int chunk, glm::ivec3 lo, glm::ivec3 hi,
                               const SurfelParams& params)
{
    if (!m_store)
        return false;
    auto it = m_cache.find(chunk);
    if (it == m_cache.end())
        return false;
    Cache& c = it->second;
    // Drop parents and their derived edge bridges together. Keys are sorted
    // by x,y,z but an AABB is not a key range, so filter by unpacking.
    auto retainOutside = [&](std::vector<uint64_t>& keys,
                             std::vector<Surfel>& values) {
        size_t w = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keyInside(keys[i], lo, hi))
                continue;
            keys[w] = keys[i];
            values[w] = values[i];
            ++w;
        }
        keys.resize(w);
        values.resize(w);
    };
    retainOutside(c.keys, c.surfels);
    retainOutside(c.edgeKeys, c.edgeSurfels);
    {
        SurfelRange rg = buildChunkSurfelsRange(*m_store, chunk, lo, hi, params);
        if (getenv("VF_TRACE"))
            spdlog::info("live refresh chunk {} box {}x{}x{}: {} parents, {} "
                         "edge bridges -> run now {} parents + {} edges",
                         chunk, hi.x - lo.x, hi.y - lo.y, hi.z - lo.z,
                         rg.surfels.size(), rg.edgeSurfels.size(),
                         c.surfels.size(), c.edgeSurfels.size());
        splice(chunk, std::move(rg));
    }
    return true;
}

std::vector<int> LiveEditor::stamp(const std::vector<StoreEdit>& edits,
                                   const SurfelParams& params,
                                   int margin)
{
    std::vector<int> changed;
    if (!m_store || edits.empty())
        return changed;
    int lox = 1 << 30, loy = 1 << 30, loz = 1 << 30;
    int hix = -(1 << 30), hiy = -(1 << 30), hiz = -(1 << 30);
    for (const StoreEdit& e : edits) {
        lox = std::min(lox, e.x); loy = std::min(loy, e.y); loz = std::min(loz, e.z);
        hix = std::max(hix, e.x); hiy = std::max(hiy, e.y); hiz = std::max(hiz, e.z);
    }
    m_store->apply(edits);
    std::vector<int> dirty = m_store->dirtyChunks();
    m_store->rebuildDirty();
    // Surface membership changes within 1 cell of the brush; the SDF-gradient
    // normals and the baked AO/shadow of nearby surfels change within the
    // store band the rebuild just solved. `margin` is 3 for the fast path and
    // kExactStampMargin for Smooth, which then matches a full reload.
    const glm::ivec3 slo(lox - margin, loy - margin, loz - margin);
    const glm::ivec3 shi(hix + margin + 1, hiy + margin + 1, hiz + margin + 1);
    for (int ci : dirty) {
        // A freshly seeded chunk's cache is the GPU's CURRENT run, which is
        // pre-edit geometry (the GPU has not been patched yet): refresh the
        // edited region on top, otherwise the first stamp in a chunk patches
        // the old surface back and looks like a no-op in the splat backend.
        if (!hasChunk(ci))
            seed(ci, params);
        refreshRegion(ci, slo, shi, params);
        changed.push_back(ci);
    }
    return changed;
}

const std::vector<Surfel>& LiveEditor::chunkSurfels(int chunk, const SurfelParams& params)
{
    if (!hasChunk(chunk) && m_store)
        seed(chunk, params);
    static const std::vector<Surfel> empty;
    auto it = m_cache.find(chunk);
    return it == m_cache.end() ? empty : it->second.surfels;
}

void LiveEditor::rebuildRun(int chunk)
{
    auto it = m_cache.find(chunk);
    if (it == m_cache.end())
        return;
    Cache& c = it->second;
    c.run.clear();
    c.run.reserve(c.surfels.size() + c.edgeSurfels.size());
    c.run.insert(c.run.end(), c.surfels.begin(), c.surfels.end());
    c.run.insert(c.run.end(), c.edgeSurfels.begin(), c.edgeSurfels.end());
    c.runDirty = false;

    // ---- TRACE audit: geometric support over the LIVE run ----------------
    // The bake audit (buildSurfels) reports ~0.02% unconnected disks, all at
    // the world rim. This is the same question for the store/live path, which
    // is a SEPARATE enumerator (collectChunkCandidates) and therefore the one
    // place a floating disk can appear without the bake gate noticing. The run
    // layout here is [base | edge bridges], the same one the
    // GPU draws, so this measures exactly what is visible.
    if (getenv("VF_TRACE")) {
        const auto& S = c.run;
        const float half = 0.5f * vf::voxel::WORLD;
        const float vs = vf::voxel::VOXEL;
        // Same 10-10-10 packing the surfelizer uses internally (it is not
        // exported); duplicated here only for this diagnostic bucket.
        auto cellKey = [&](int cx, int cy, int cz) {
            return (uint64_t(std::uint32_t(cx)) << 20) | (uint32_t(cy) << 10) |
                   uint32_t(cz);
        };
        auto cellOf = [&](const vf::voxel::Surfel& s) {
            return cellKey(int((s.pos_rU.x + half) / vs),
                           int((s.pos_rU.y + half) / vs),
                           int((s.pos_rU.z + half) / vs));
        };
        std::unordered_map<uint64_t, std::vector<int>> bucket;
        bucket.reserve(S.size());
        for (size_t i = 0; i < S.size(); ++i)
            bucket[cellOf(S[i])].push_back(int(i));
        static const int kProbe[27][3] = {
            {-1,-1,-1},{0,-1,-1},{1,-1,-1},{-1,0,-1},{1,0,-1},{-1,1,-1},{0,1,-1},{1,1,-1},
            {-1,-1, 0},{1,-1, 0},{-1, 1, 0},{1, 1, 0},
            {-1,-1, 1},{0,-1, 1},{1,-1, 1},{-1,0, 1},{1,0, 1},{-1,1, 1},{0,1, 1},{1,1, 1},
            {-1, 0, 0},{1, 0, 0},{0,-1, 0},{0, 1, 0},{0, 0,-1},{0, 0, 1},{0, 0, 0}};
        size_t floating = 0, probed = 0;
        int shown = 0;
        for (size_t i = 0; i < S.size(); i += 11) {
            const auto& a = S[i];
            const uint64_t ck = cellOf(a);
            const int x = int((ck >> 20) & 0x3FFu);
            const int y = int((ck >> 10) & 0x3FFu);
            const int z = int(ck & 0x3FFu);
            const float ra = std::max(a.pos_rU.w, a.normal_rV.w);
            ++probed;
            bool touch = false;
            for (int d = 0; d < 27 && !touch; ++d) {
                auto it = bucket.find(cellKey(x + kProbe[d][0], y + kProbe[d][1],
                                              z + kProbe[d][2]));
                if (it == bucket.end())
                    continue;
                for (int j : it->second) {
                    if (size_t(j) == i)
                        continue;
                    const auto& b = S[j];
                    if (glm::distance(a.pos_rU, b.pos_rU) <
                        ra + std::max(b.pos_rU.w, b.normal_rV.w)) {
                        touch = true;
                        break;
                    }
                }
            }
            if (touch)
                continue;
            ++floating;
            if (shown < 8) {
                ++shown;
                spdlog::info("  LIVE-FLOATING #{} chunk {} cell ({},{},{}) mat {} "
                             "r {:.3f} pos ({:.2f},{:.2f},{:.2f}) run {}", shown,
                             chunk, x, y, z, int(a.mat_ao.x + 0.5f), ra,
                             a.pos_rU.x, a.pos_rU.y, a.pos_rU.z, S.size());
            }
        }
        if (probed)
            spdlog::info("live-support audit: chunk {} run {} -> {} unconnected of "
                         "{} probed ({:.4f}%)", chunk, S.size(), floating, probed,
                         100.0 * floating / probed);
    }
}

const std::vector<Surfel>& LiveEditor::chunkRun(int chunk, const SurfelParams& params)
{
    static const std::vector<Surfel> empty;
    if (!hasChunk(chunk)) {
        if (!m_store)
            return empty;
        seed(chunk, params);
    }
    auto it = m_cache.find(chunk);
    if (it == m_cache.end())
        return empty;
    if (it->second.runDirty)
        rebuildRun(chunk);
    return it->second.run;
}

uint32_t LiveEditor::edgeCountOf(int chunk) const
{
    auto it = m_cache.find(chunk);
    return it == m_cache.end() ? 0u : uint32_t(it->second.edgeSurfels.size());
}

} // namespace vf::voxel
