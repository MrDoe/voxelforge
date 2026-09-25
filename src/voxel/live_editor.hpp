#pragma once
// LiveEditor: instant-feedback brush editing over a ChunkStore.
//
// Each stamp applies cell edits, rebuilds only the affected store regions
// (ChunkStore::rebuildDirty), and refreshes a per-chunk surfel cache. Because
// the cache remembers the previous run, only the cells inside the brush
// footprint + the caller's refresh margin are re-enumerated and re-shaded — a
// stamp costs roughly the brush area instead of a whole chunk, which is what
// makes drag-painting feel immediate.
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

    // Optional source of a chunk's current GPU base+edge run. When set,
    // first-touch seeding splices from what the GPU already renders instead of
    // re-baking the whole chunk (no first-stamp hitch). The source returns the
    // parent and derived hard-edge segments separately; micro detail is
    // regenerated locally and is never seeded.
    using SeedFn = std::function<SurfelRange(int chunk)>;
    void setSeedSource(SeedFn fn) { m_seedFn = std::move(fn); }
    void clearSeedSource() { m_seedFn = nullptr; }

    // Force a full store-based seed (ignores the GPU source) — used when
    // restoring a persisted overlay whose geometry the GPU does not have yet.
    void seedFromStore(int chunk, const SurfelParams& params);

    // Re-enumeration margin around the edited AABB. The fast path (+-3 cells)
    // covers surface membership changes and their immediate shading.
    // kExactStampMargin matches ChunkStore's live SDF band (kLiveBand), so the
    // refreshed run covers every cell whose SDF/normal/AO the store just
    // re-solved: a live patch is then identical to what a full reload of that
    // region would build. Smooth strokes use the exact margin.
    static constexpr int kStampMargin = 3;
    static constexpr int kExactStampMargin = 12;

    // Apply the edits and refresh the caches of every dirty chunk. Returns the
    // chunk indices whose surfel run changed (cache is accessible via
    // chunkSurfels()).
    std::vector<int> stamp(const std::vector<StoreEdit>& edits,
                           const SurfelParams& params,
                           int margin = kStampMargin);

    // Full chunk run (call chunkSurfels(ci) only for chunks returned by stamp()
    // or after seed()). The reference stays valid until the next stamp().
    const std::vector<Surfel>& chunkSurfels(int chunk, const SurfelParams& params);

    // Combined base + micro-detail run (what the GPU should draw). The micro
    // tail is regenerated from the cached base run with the shared bake hash
    // (buildMicroSurfels) whenever the base changed and params.microDetail is
    // on, so a live-patched chunk keeps its texture geometry. microStartOf()
    // returns the index where the micro tail begins (== run size when there is
    // no split). References stay valid until the next stamp()/seed().
    const std::vector<Surfel>& chunkRun(int chunk, const SurfelParams& params);
    uint32_t microStartOf(int chunk) const;
    uint32_t edgeCountOf(int chunk) const;
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
        std::vector<uint64_t> keys; // one parent key per base surfel, sorted
        std::vector<Surfel> surfels;
        // Derived hard-edge bridges, kept separate so region refreshes remove
        // them with their parent and material micros remain parent-only.
        std::vector<uint64_t> edgeKeys; // repeated when a corner has >1 pair
        std::vector<Surfel> edgeSurfels;
        std::vector<Surfel> micros;     // deterministic material micro tail
        std::vector<Surfel> run;        // base + edge + micros, rebuilt lazily
        uint32_t microStart = 0;        // index of the micro tail inside `run`
        bool runDirty = true;
        bool runMicroDetail = false;    // params.microDetail used for `run`
    };
    void seed(int chunk, const SurfelParams& params);
    void splice(int chunk, const SurfelRange& rg);
    // Regenerate the cached chunk's micro tail (buildMicroSurfels) and
    // re-assemble `run`. Called lazily by chunkRun().
    void rebuildRun(int chunk, const SurfelParams& params);

    ChunkStore* m_store = nullptr;
    SeedFn m_seedFn;
    std::unordered_map<int, Cache> m_cache;
};

} // namespace vf::voxel
