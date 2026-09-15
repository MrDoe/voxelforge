#pragma once
// LiveEditor: instant-feedback brush editing over a ChunkStore.
//
// Each stamp applies cell edits, rebuilds only the affected store regions
// (ChunkStore::rebuildDirty), and refreshes a per-chunk surfel cache. Because
// the cache remembers the previous run, only the cells inside the brush
// footprint (+1 cell) are re-enumerated and re-shaded — a stamp costs roughly
// the brush area instead of a whole chunk, which is what makes drag-painting
// feel immediate.
//
// The app patches each returned chunk run into the GPU buffers
// (SplatPass::patchChunkSurfels) and the SVO arenas (SvoPass::patchChunk).
#include "voxel/chunk_store.hpp"
#include "voxel/surfelize.hpp"
#include <functional>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace vf::voxel {

class LiveEditor {
public:
    void attach(ChunkStore* store)
    {
        m_store = store;
        m_cache.clear();
    }
    bool attached() const { return m_store != nullptr; }
    void clear() { m_cache.clear(); }

    // Optional source of a chunk's current GPU base surfel run. When set,
    // first-touch seeding splices from what the GPU already renders instead of
    // re-baking the whole chunk (no first-stamp hitch). The source must return
    // base surfels only (no micro tail), key-sorted.
    using SeedFn = std::function<std::vector<Surfel>(int chunk)>;
    void setSeedSource(SeedFn fn) { m_seedFn = std::move(fn); }
    void clearSeedSource() { m_seedFn = nullptr; }

    // Force a full store-based seed (ignores the GPU source) — used when
    // restoring a persisted overlay whose geometry the GPU does not have yet.
    void seedFromStore(int chunk, const SurfelParams& params);

    // Apply the edits and refresh the caches of every dirty chunk. Returns the
    // chunk indices whose surfel run changed (cache is accessible via
    // chunkSurfels()).
    std::vector<int> stamp(const std::vector<StoreEdit>& edits,
                           const SurfelParams& params);

    // Full chunk run (call chunkSurfels(ci) only for chunks returned by stamp()
    // or after seed()). The reference stays valid until the next stamp().
    const std::vector<Surfel>& chunkSurfels(int chunk, const SurfelParams& params);
    bool hasChunk(int chunk) const { return m_cache.count(chunk) != 0; }

    // Region-limited refresh: re-enumerate/shade only [lo, hi) and splice into
    // the cached run (keeps AO/shadow of the rest). Returns false without a
    // cache.
    bool refreshRegion(int chunk, glm::ivec3 lo, glm::ivec3 hi,
                       const SurfelParams& params);
    // Drop a chunk's cache (e.g. after a full GPU reload invalidated runs).
    void invalidate(int chunk) { m_cache.erase(chunk); }

private:
    struct Cache {
        std::vector<uint64_t> keys; // sorted, parallel to surfels
        std::vector<Surfel> surfels;
    };
    void seed(int chunk, const SurfelParams& params);
    void splice(int chunk, const SurfelRange& rg);

    ChunkStore* m_store = nullptr;
    SeedFn m_seedFn;
    std::unordered_map<int, Cache> m_cache;
};

} // namespace vf::voxel
