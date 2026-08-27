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

// Per-chunk SVO octree pool. Kept resident across reloads so that only the
// chunks touched by a changed layer need to be rebuilt (incremental update).
struct ChunkPool {
    std::vector<uint32_t> childBase, payload, handles, bricks;
    int32_t root = -1; // chunk root handle (-1 = empty)

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
    const Stats& stats() const { return m_stats; }
    const VoxelField& field() const { return m_field; }

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

    // cached per-chunk SVO pools (kept across reloads for incremental updates)
    std::vector<std::unique_ptr<ChunkPool>> m_pools;

    // dirty-tracking state for incremental rebuilds
    std::map<std::string, WorldAABB> m_prevBox;            // layer file -> record AABB
    std::map<std::string, bool> m_prevEnabled;             // layer file -> enabled
    std::map<std::string, unsigned long long> m_prevSig;   // layer file -> content sig (mtime+size)

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
