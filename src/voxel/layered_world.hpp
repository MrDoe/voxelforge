#pragma once
// Layered world: the single runtime world source.
//
// Loads the layer family described by assets/world.json (record-only .vxw
// files: landscape.vxw, object layers, scatter, ai_edits.vxw) and synthesizes
// the chunked-SVO GpuWorld directly from those voxels via VoxelField. There is
// no merged cache and no analytic scene(): every renderer path (SVO ray-march,
// probes) consumes the same records-derived state, so layer edits / enable
// toggles / MCP appends are reflected everywhere at once.
//
// Synthesis rules (VoxelField is truth):
//   - terrain: per-column top record from the landscape layer (world Y of its
//     top face + material); everything below reads solid,
//   - objects: connected components of object records flood-filled to solid
//     volumes with a signed distance transform, so shells AND enclosed
//     interiors read solid at every distance - no hollow-voxel holes,
//   - brick appearance is the exact record colour where one exists, otherwise
//     the palette of the field's material at that cell,
//   - air below WATER_LEVEL is marked as water volume (mat id 9, non-hit).
//
// reloadIfChanged() stats the manifest + layer files and rebuilds when any of
// them changed on disk - this is how MCP edits and GUI checkboxes reach a
// running instance without an external repack step.
#include "voxel/world.hpp"
#include "voxel/chunk_store.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/voxel_field.hpp"
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <glm/glm.hpp>

namespace vf::voxel {

// World-space AABB of a layer's records, used to derive dirty chunks.
struct WorldAABB {
    glm::vec3 lo{ 1e9f, 1e9f, 1e9f };
    glm::vec3 hi{ -1e9f, -1e9f, -1e9f };
    bool valid() const { return hi.x >= lo.x; }
};

class LayeredWorld {
public:
    struct Stats {
        size_t records = 0;
        size_t nodes = 0, bricks = 0, activeChunks = 0;
        double buildSeconds = 0.0;
        size_t memoryBytes = 0;
    };

    // Read manifest + enabled layers, merge with priority, synthesize the SVO.
    // Returns false when no readable manifest exists (caller falls back to the
    // procedural World::build path).
    bool load(const std::string& manifestPath);

    // Rebuild if any input file changed since the last load. Returns kSyncDone
    // when the fresh world is already applied (synchronous mode) or kStartedAsync
    // when a background rebuild was kicked (poll consumeRebuild() each frame).
    enum ReloadResult { kNone, kSyncDone, kStartedAsync };
    ReloadResult reloadIfChanged(glm::vec3 cam);

    // Force a (camera-priority) rebuild, e.g. after a GUI layer toggle.
    void requestReload(glm::vec3 cam, bool full = true);

    // Returns true once a background rebuild has finished and the fresh world has
    // been swapped in; the caller should then re-upload GPU buffers.
    bool consumeRebuild();

    bool loaded() const { return m_loaded; }

    const GpuWorld& gpu() const { return m_gpu; }
    const std::vector<VoxelRecord>& records() const { return m_records; }
    const std::vector<worldfile::WorldLayer>& layers() const { return m_layersMeta; }
    // Placed (post-transform) record AABB per layer file, or invalid when the
    // layer is disabled/unloaded. Used to resolve which layer a picked voxel
    // belongs to (the trackball rotate target).
    WorldAABB layerBox(const std::string& file) const
    {
        const auto it = m_prevBox.find(file);
        return it != m_prevBox.end() ? it->second : WorldAABB{};
    }
    // Exact runtime owner used by picking, surfels and the rotation preview.
    // IDs are stable for the loaded layer set; 0 means disabled/unowned.
    uint8_t layerId(const std::string& file) const
    {
        const auto it = m_prevLayerIds.find(file);
        if (it == m_prevLayerIds.end() || !layerBox(file).valid())
            return 0;
        return it->second;
    }
    std::string layerFile(uint8_t id) const
    {
        if (id == 0)
            return {};
        for (const auto& kv : m_prevLayerIds)
            if (kv.second == id && layerBox(kv.first).valid())
                return kv.first;
        return {};
    }
    // Source-record bottom-center plus the manifest translation. This is the
    // exact pivot used by transformRecords; a post-rotation AABB midpoint is
    // not equivalent and produces a visibly wrong live preview.
    bool layerPivot(const std::string& file, glm::vec3& out) const
    {
        const auto it = m_prevPivots.find(file);
        if (it == m_prevPivots.end())
            return false;
        out = it->second;
        return true;
    }
    const Stats& stats() const { return m_stats; }
    const VoxelField& field() const { return m_field; }

    // Runtime-explicit sparse voxel store (M0 foundation). Built lazily from
    // the resident pools on first access; invalidated by every rebuild. This
    // is where runtime edits will be applied (see chunk_store.hpp).
    ChunkStore& store()
    {
        if (m_storeStale) {
            m_store.setLayerSource(&m_field);
            m_store.adopt(m_pools, m_colTop, m_colMat);
            m_storeStale = false;
        }
        return m_store;
    }

    // Drop every runtime store edit: the next store() re-adopts the resident
    // pools. Used by "Clear live edits", which also re-patches the GPU chunks
    // the cleared edits had touched.
    void invalidateStore() { m_storeStale = true; }

    ~LayeredWorld();

private:
    // Build the SVO into the supplied targets. When `full` is false only the
    // chunks in `dirty` are rebuilt; the rest are reused from `prevPools` (left
    // null in `outPools` entries are skipped). Chunks are visited nearest-first
    // relative to `camPos` so a camera-priority rebuild does the visible volume
    // first. Used by both the synchronous initial load and the background reload.
    bool buildInto(bool full, const std::vector<int>& dirty,
                   std::vector<std::unique_ptr<ChunkPool>>* prevPools,
                   glm::vec3 camPos, GpuWorld& outGpu,
                   std::vector<std::unique_ptr<ChunkPool>>& outPools,
                   VoxelField& outField, Stats& outStats);

    // Synchronous rebuild into the live members (initial load / VF_SYNC_RELOAD).
    bool rebuildNow(bool full, const std::vector<int>& dirty, glm::vec3 cam);

    // Parse the manifest + enabled layers into the record buffers and compute the
    // full/incremental dirty set. Shared by load() and reloadIfChanged().
    bool parseAndComputeDirty(bool& outFull, std::vector<int>& outDirty,
                             double& outReadMs, double& outFieldMs);

    // Kick a rebuild: synchronous when VF_SYNC_RELOAD is set, otherwise a
    // background thread builds into a pending world that consumeRebuild() swaps
    // in. Returns the resulting ReloadResult.
    ReloadResult kick(bool full, std::vector<int> dirty, glm::vec3 cam);

    std::string m_manifestPath;
    bool m_loaded = false;
    bool m_hasFullBuild = false;

    GpuWorld m_gpu;
    std::vector<VoxelRecord> m_records;                    // priority-merged union
    std::vector<worldfile::WorldLayer> m_layersMeta;       // enabled manifest entries
    std::vector<int16_t> m_colTop;                         // per-column landscape top (lattice y)
    std::vector<uint8_t> m_colMat;                         // material of each column's top record

    // packed cellKeys of object (non-landscape) records; grouped into
    // connected components and flood-filled to solid interiors by the
    // VoxelField build.
    std::vector<uint32_t> m_objCells;
    std::vector<uint8_t> m_objMats;                        // material per object cell (parallel to m_objCells)
    std::vector<uint8_t> m_objTexs;                        // per-cell texture override (record's reserved byte)
    std::vector<uint8_t> m_objLayerIds;                    // owning .vxw layer (parallel to m_objCells)
    // subtractive "carve" layer cells (role "carve"): cut holes through terrain
    // and objects. Not part of the merged record set; fed to VoxelField::build
    // as a separate field.
    std::vector<uint32_t> m_carveCells;
    std::vector<uint8_t> m_carveMats;
    // additive "raise" layer cells (role "raise"): lift the terrain into a
    // half-sphere bump. Like carve, kept out of the merged record set and fed to
    // VoxelField::build as its own field that only deforms the heightfield.
    std::vector<uint32_t> m_raiseCells;
    std::vector<uint8_t> m_raiseMats;
    std::vector<uint8_t> m_blockSolid;                     // global presence grid (records+interior)
    VoxelField m_field;                                    // records-derived geometry oracle

    // runtime-explicit sparse voxel store; lazily adopted from m_pools
    ChunkStore m_store;
    bool m_storeStale = true;

    // cached per-chunk SVO pools (kept across reloads for incremental updates)
    std::vector<std::unique_ptr<ChunkPool>> m_pools;

    // dirty-tracking state for incremental rebuilds
    std::map<std::string, WorldAABB> m_prevBox;            // layer file -> record AABB
    std::map<std::string, bool> m_prevEnabled;             // layer file -> enabled
    std::map<std::string, unsigned long long> m_prevSig;   // layer file -> content sig (mtime+size)
    std::map<std::string, uint64_t> m_prevPlace;           // layer file -> placement hash (pos/rot)
    std::map<std::string, uint8_t> m_prevLayerIds;         // enabled file -> GPU owner id
    std::map<std::string, glm::vec3> m_prevPivots;         // file -> canonical placed pivot

    struct Signature {
        std::string path;
        unsigned long long mtime = 0;                      // nanoseconds since epoch
        unsigned long long size = 0;
    };
    std::vector<Signature> m_signatures;

    // parsed-layer cache: file -> (content sig, records). Reloads re-parse only
    // layers whose signature changed (usually just ai_edits.vxw / world.json).
    struct LayerCacheEntry {
        unsigned long long mtime = 0;
        unsigned long long size = 0;
        bool valid = false;                                // false = parse failed before
        std::vector<VoxelRecord> voxels;
    };
    std::map<std::string, LayerCacheEntry> m_layerCache;

    Stats m_stats;

    // --- background rebuild state (camera-priority, non-blocking) ----------
    struct PendingBuild {
        GpuWorld gpu;
        std::vector<std::unique_ptr<ChunkPool>> pools;
        VoxelField field;
        Stats stats;
    };
    std::unique_ptr<PendingBuild> m_pending;
    std::mutex m_pendingMtx;
    std::atomic<bool> m_rebuildRunning{ false };
    std::atomic<bool> m_rebuildDone{ false };
    std::thread m_rebuildThread;
};

} // namespace vf::voxel
