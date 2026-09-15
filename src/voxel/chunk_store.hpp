#pragma once
// ChunkStore: the mutable runtime truth for world geometry.
//
// Terrain and objects are the same thing here: per-chunk voxel cells with
// packed appearance (r/g/b), surface response (reflectivity/roughness),
// material id and (later) semantic tags. The bake's derived tiers
// (heightfield columns, component SDFs) remain load-time/bootstrap only.
//
// Per chunk (64^3 cells, canonical z-major index from chunk_index.hpp):
//   Empty    : nothing
//   Solid    : uniform underground fill (terrain interior). Material is
//              resolved from the base column grid when materialised, so a
//              carve exposes the correct per-column material.
//   Explicit : sparse 8^3 bricks plus solid boxes. A brick is the exact GPU
//              layout (world.hpp): word0 = r|g<<8|b<<16|sdfByte<<24,
//              word1 = a|refl<<8|rough<<16|(mat | objFlag<<7)<<24.
//
// Edits are cell-level (set/clear/paint). Dirty chunks rebuild incrementally:
// the local SDF band is recomputed from a dense materialisation of the
// chunk's cells and the octree pool is regenerated from the sparse data.
// M0 wires queries, edits and tests; GPU chunk patching builds on this.
#include "voxel/chunk_index.hpp"
#include "voxel/common.hpp"
#include "voxel/voxel_field.hpp"
#include "voxel/world.hpp"
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vf::voxel {

// One decoded voxel cell.
struct StoreCell {
    bool solid = false;
    bool obj = false;   // object-surface flag (SVO gradient normals/material)
    int8_t sdfRaw = 32; // voxel units (negative inside solid); +32 = far air
    uint8_t r = 0, g = 0, b = 0, a = 255;
    uint8_t reflectivity = 0, roughness = 0;
    uint8_t mat = 0;
    uint8_t tags = 0; // semantic tag bits (reserved; 0 for baked cells)
};

// One cell mutation.
struct StoreEdit {
    enum class Mode : uint8_t { Set, Clear, Paint };
    int x = 0, y = 0, z = 0;
    Mode mode = Mode::Set;
    uint8_t mat = 2;
    uint8_t tags = 0;
    uint8_t r = 0, g = 0, b = 0, reflectivity = 0, roughness = 0;
    bool hasColor = false; // false -> palette colour/response of `mat`
};

class ChunkStore {
public:
    struct Stats {
        size_t chunks = 0;     // non-empty chunks
        size_t bricks = 0;     // explicit bricks across all chunks
        size_t solidBoxes = 0; // compressed solid regions
    };

    // Adopt the synthesized per-chunk pools as the initial explicit world.
    // `colTop`/`colMat` are the landscape column tops and materials used to
    // resolve Solid-chunk material when cells are later materialised.
    void adopt(const std::vector<std::unique_ptr<ChunkPool>>& pools,
               const std::vector<int16_t>& colTop,
               const std::vector<uint8_t>& colMat);
    void clear();

    bool loaded() const { return m_loaded; }
    int latN() const { return m_latN; }

    // --- queries ------------------------------------------------------------
    StoreCell cellAt(int x, int y, int z) const;
    // Same convention as VoxelField::sample: metres, negative inside solid.
    // Cells outside the explicit band read +kFar (3.2 m) / their Solid box.
    VoxelField::Sample sample(int x, int y, int z) const;
    VoxelField::Sample sampleWorld(glm::vec3 p) const;

    // --- edits --------------------------------------------------------------
    // Queue a cell mutation. Does not rebuild; call rebuildDirty() once after
    // a batch so neighbouring edits coalesce.
    void apply(const StoreEdit& e);
    void apply(const std::vector<StoreEdit>& edits);
    // Rebuild every dirty chunk (SDF band + octree pool).
    void rebuildDirty();
    // Force the full-chunk rebuild path for one chunk (ignores the recorded
    // edit bounds). Used by the localized-rebuild equivalence test.
    void rebuildFull(int ci);

    const std::vector<int>& dirtyChunks() const { return m_dirty; }
    bool chunkDirty(int ci) const;
    void clearDirty() { m_dirty.clear(); }

    // Derived octree pool for a chunk. Built lazily from the canonical sparse
    // data on first access; rebuilt by rebuildDirty().
    const std::unique_ptr<ChunkPool>& pool(int ci);
    Stats stats() const;
    // Deterministic content hash of one chunk's canonical data (tests).
    uint64_t chunkHash(int ci) const;

    // --- persistence (VXW v2 store-overlay section) --------------------------
    // Only chunks touched by apply() are serialized. The section payload is
    // opaque to worldfile; ChunkStore owns its schema.
    bool hasEditedChunks() const;
    std::vector<int> editedChunks() const;
    std::vector<uint8_t> serializeEdited() const;
    // Replace the listed chunks wholesale (used on startup to restore a saved
    // overlay). Pools are invalidated and rebuilt lazily on next access.
    bool applyEdited(const std::vector<uint8_t>& data);
    // Write/read assets/<name>.vxw as a VXW v2 file with one store section
    // (atomic temp+rename). Missing files are not an error on load.
    bool saveOverlay(const std::string& path) const;
    bool loadOverlay(const std::string& path);
    // Last edit AABB (global lattice coords) of one chunk, false when none.
    bool chunkEditBounds(int ci, int lo[3], int hi[3]) const;

    static constexpr int kBrickPerAxis = CHUNK_N / BRICK_N;               // 8
    static constexpr int kBricksPerChunk = kBrickPerAxis * kBrickPerAxis * kBrickPerAxis;

private:
    struct SolidBox {
        int16_t x = 0, y = 0, z = 0, side = 0; // chunk-local
        uint8_t mat = 2;
        bool terrain = true; // material resolved from colMat when expanded
    };
    struct Chunk {
        enum class State : uint8_t { Empty, Solid, Explicit };
        State state = State::Empty;
        std::vector<uint32_t> bricks;      // slot * BRICK_WORDS
        std::vector<uint32_t> slotOf;      // kBricksPerChunk entries; kNoBrick
        std::vector<SolidBox> boxes;
        std::unique_ptr<ChunkPool> pool;   // derived octree (rebuilt lazily)
        bool poolValid = false;
        bool dirty = false;
        bool edited = false; // touched by apply(), serialized to the overlay
        // lattice bounds touched since the last rebuild (for the SDF band)
        int lo[3] = { 1 << 30, 1 << 30, 1 << 30 };
        int hi[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
    };

    static constexpr uint32_t kNoBrick = 0xFFFFFFFFu;

    void ensureExplicit(int ci);
    uint32_t ensureBrick(int ci, Chunk& c, uint32_t blockKey);
    void decodeCell(const Chunk& c, uint32_t slot, int lx, int ly, int lz, StoreCell& out) const;
    void encodeCell(uint32_t* words, const StoreCell& cell);
    void markDirty(int ci, int x, int y, int z);
    void rebuildChunk(int ci);
    void buildPoolOnly(int ci);
    bool blockCoveredByBox(const Chunk& c, int lx, int ly, int lz) const;

    int m_latN = int(WORLD / VOXEL);
    bool m_loaded = false;
    std::vector<Chunk> m_chunks;
    std::vector<int16_t> m_colTop;
    std::vector<uint8_t> m_colMat;
    std::vector<int> m_dirty; // chunk indices, stable order
};

// Debounced background writer for the store overlay file. queue() serializes
// the edited chunks on the calling thread (the store is single-threaded),
// then a worker writes atomically (temp + rename); a newer queue() supersedes
// an unstarted write so rapid edits coalesce. flush() in the destructor.
class OverlayWriter {
public:
    OverlayWriter() = default;
    ~OverlayWriter();
    OverlayWriter(const OverlayWriter&) = delete;
    OverlayWriter& operator=(const OverlayWriter&) = delete;

    void queue(const ChunkStore& store, const std::string& path);
    void flush();

private:
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::thread m_th;
    std::string m_path;
    std::vector<uint8_t> m_bytes;
    bool m_pending = false;
    bool m_running = false;
    bool m_stop = false;
};

} // namespace vf::voxel
