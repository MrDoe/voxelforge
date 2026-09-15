#include "voxel/live_editor.hpp"
#include <algorithm>
#include <utility>

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
}

void LiveEditor::seed(int chunk, const SurfelParams& params)
{
    if (m_seedFn) {
        std::vector<Surfel> run = m_seedFn(chunk);
        if (!run.empty()) {
            Cache& c = m_cache[chunk];
            c.surfels = std::move(run);
            c.keys.resize(c.surfels.size());
            for (size_t i = 0; i < c.surfels.size(); ++i)
                c.keys[i] = keyFromSurfel(c.surfels[i]);
            if (!std::is_sorted(c.keys.begin(), c.keys.end())) {
                std::vector<std::pair<uint64_t, Surfel>> pairs(c.keys.size());
                for (size_t i = 0; i < c.keys.size(); ++i)
                    pairs[i] = { c.keys[i], c.surfels[i] };
                std::sort(pairs.begin(), pairs.end(),
                          [](const std::pair<uint64_t, Surfel>& a,
                             const std::pair<uint64_t, Surfel>& b) {
                              return a.first < b.first;
                          });
                for (size_t i = 0; i < pairs.size(); ++i) {
                    c.keys[i] = pairs[i].first;
                    c.surfels[i] = pairs[i].second;
                }
            }
            return;
        }
    }
    seedFromStore(chunk, params);
}

void LiveEditor::splice(int chunk, const SurfelRange& rg)
{
    Cache& c = m_cache[chunk];
    std::vector<std::pair<uint64_t, Surfel>> merged;
    merged.reserve(c.keys.size() + rg.keys.size());
    for (size_t i = 0; i < c.keys.size(); ++i)
        merged.push_back({ c.keys[i], c.surfels[i] });
    for (size_t i = 0; i < rg.keys.size(); ++i)
        merged.push_back({ rg.keys[i], rg.surfels[i] });
    std::sort(merged.begin(), merged.end(),
              [](const std::pair<uint64_t, Surfel>& a,
                 const std::pair<uint64_t, Surfel>& b) { return a.first < b.first; });
    c.keys.resize(merged.size());
    c.surfels.resize(merged.size());
    for (size_t i = 0; i < merged.size(); ++i) {
        c.keys[i] = merged[i].first;
        c.surfels[i] = merged[i].second;
    }
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
    // drop cached cells inside the region (keys are sorted by x,y,z but an
    // AABB is not a key range, so filter by unpacking)
    size_t w = 0;
    for (size_t i = 0; i < c.keys.size(); ++i) {
        if (keyInside(c.keys[i], lo, hi))
            continue;
        c.keys[w] = c.keys[i];
        c.surfels[w] = c.surfels[i];
        ++w;
    }
    c.keys.resize(w);
    c.surfels.resize(w);
    splice(chunk, buildChunkSurfelsRange(*m_store, chunk, lo, hi, params));
    return true;
}

std::vector<int> LiveEditor::stamp(const std::vector<StoreEdit>& edits,
                                   const SurfelParams& params)
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
    // Surface membership changes within 1 cell of the brush; AO/shadow change
    // within the store band. Refresh +-3 cells per stamp for immediacy — the
    // next full rebuild refreshes the wider band.
    const glm::ivec3 slo(lox - 3, loy - 3, loz - 3);
    const glm::ivec3 shi(hix + 4, hiy + 4, hiz + 4);
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

} // namespace vf::voxel
