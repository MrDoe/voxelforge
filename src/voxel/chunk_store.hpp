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
// Edits are cell-level (set/clear/paint, plus terrain-flagged Set batches for
// Smooth). Dirty chunks rebuild incrementally:
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
    // Set normally creates live object geometry. Terrain sculpt operations set
    // this so raised cells keep the terrain flag and are not accidentally
    // tagged as an object surface.
    bool terrain = false;
};

// Result of one relaxation brush stamp (terrain column or object surface).
// `riseCells` is the greatest upward movement in the batch, which lets the
// caller refresh the height texture without scanning from an arbitrary world
// height. `objectSurface` marks a batch produced by the object-surface
// relaxation, so the caller can refresh the splat cache over the full store
// band the edit solved (a Smooth stroke changes nearby normals/AO beyond the
// brush footprint).
struct SmoothTerrainEdits {
    std::vector<StoreEdit> edits;
    int riseCells = 0;
    bool objectSurface = false;
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
    // Non-persistent provenance oracle for live surfels/picking. The brick
    // format intentionally has no layer lane; base object cells borrow their
    // owner from the current records-derived field, while newly added live
    // cells remain world-space/unowned (layer 0).
    void setLayerSource(const VoxelField* field) { m_layerSource = field; }
    void clear();

    bool loaded() const { return m_loaded; }
    int latN() const { return m_latN; }

    // --- queries ------------------------------------------------------------
    StoreCell cellAt(int x, int y, int z) const;
    // Same convention as VoxelField::sample: metres, negative inside solid.
    // Cells outside the explicit band read +kFar (3.2 m) / their Solid box.
    VoxelField::Sample sample(int x, int y, int z) const;
    VoxelField::Sample sampleWorld(glm::vec3 p) const;
    // Live-store mirror of VoxelField::collectEmissive for the dynamic-light
    // trigger: enumerate the store's current bricks + solid-box faces (so a
    // stamp that adds/removes/paints emitters is reflected) through the same
    // shared cluster tail. `budget` caps the output clusters (<= 0 = empty);
    // pass the remaining LightUBO slots after the authored lights.
    // NOTE: on baked content the store is a SUPERSET of the field (pool
    // bricks carry propagated interior mats the record enumerator never
    // sees), so this must not replace the bake enumerator under a capped
    // UBO — use collectEmissiveRegion for stroke-local refreshes instead.
    void collectEmissive(const std::vector<glm::vec3>& matEmission,
                         std::vector<VoxelField::EmissiveCluster>& out,
                         int budget) const;
    // Same, restricted to the listed chunks (stroke-local refresh): the
    // shared tail runs over the region's cells only. Region == all chunks
    // is bit-identical to collectEmissive.
    void collectEmissiveRegion(const std::vector<glm::vec3>& matEmission,
                               std::vector<VoxelField::EmissiveCluster>& out,
                               int budget, const std::vector<int>& chunks) const;

    // --- edits --------------------------------------------------------------
    // Queue a cell mutation. Does not rebuild; call rebuildDirty() once after
    // a batch so neighbouring edits coalesce.
    void apply(const StoreEdit& e);
    void apply(const std::vector<StoreEdit>& edits);
    // --- terrain relaxation tuning (makeSmoothEdits) -----------------------
    // The smoothing kernel is a Gaussian whose sigma scales with the brush
    // radius, but is capped here. A brush that averaged only its immediate 3x3
    // ring was a despeckler, not a smoother: the radius set the footprint and
    // the falloff but never the averaging scale, so a 1 m brush could not move
    // a 6-cell ridge (measured on a synthetic ridge: the crest's 3x3 average
    // was exactly its own top, so the batch contained NO edit at the crest).
    // Uncapped, sigma = radius/2 would turn one 6 m stamp into ~50M multiply-adds
    // (the kernel cost grows as radius^2 * sigma^2); the cap keeps a wide brush
    // wide-footprint-but-local instead, which is also what makes a held drag
    // converge instead of digging a world-sized pit in one stamp.
    static constexpr int kSmoothMaxSigmaCells = 16; // 1.6 m of kernel reach
    // Hard ceiling on one stamp's movement, in cells. This REPLACES the old
    // min/max clamp over the sample neighbourhood: with strength*falloff in
    // [0,1] the relaxed value already lay inside that range, so the clamp was
    // dead code on open terrain - but it would have become the binding
    // constraint the moment the average was widened (a ridge's wide average
    // falls below its 4-ring minimum), silently clipping the kernel back to one
    // cell. A step cap states the real intent: one click cannot collapse a cliff.
    // It must clear the deviation of a BRUSH-SIZED feature, or a spike takes
    // two clicks and a pit takes two: measured on the synthetic spike/pit
    // fixtures the kernel deviation is 2.53 cells, so a cap of 2 silently
    // halved every stamp. 4 keeps a single stamp to 0.4 m.
    static constexpr int kSmoothStepCapCells = 4;
    // A column whose kernel window is mostly NOT valid terrain (a building, a
    // deep void, the world edge) is left alone. Without this the average is
    // taken over the surviving side only and the clamp snaps the column toward
    // that one direction - terrain visibly eroding away from any obstacle.
    static constexpr float kSmoothMinCoverage = 0.6f;
    // Taubin's re-inflation fraction: a plain relaxation is a Laplacian
    // contraction, so a held brush slowly sinks peaks and fills pits. This
    // UNDOES part of what the smoothing step removed.
    // It MUST be applied as a fraction of the smoothing step this column
    // actually received, never as a constant offset. Measured: with a constant
    // mu, gain = s + mu*(1-s) goes NEGATIVE wherever the brush falloff is small
    // (s -> 0 gives gain = mu), and the tool then INFLATES the rim of every bump
    // into a moat - on the synthetic spike, dev -2.514 * gain -0.516 = +1.30,
    // a 1-cell RAISE on the column that should have been cut. Scaling by s
    // bounds the gain to [0, s]: the pass can only ever soften a relaxation,
    // never reverse it.
    // Default is OFF until it is measured end-to-end on a real render: it is a
    // 47% reduction in effective smoothing strength, and shrinking relief is
    // not a defect anyone has complained about - the moat ring was.
    static constexpr float kSmoothTaubinReinflate = 0.53f;
    // Build one non-mutating height-relaxation batch for the circular terrain
    // footprint around `center`. Only terrain columns are changed; object
    // surfaces are treated as protected samples and are never overwritten.
    // `preserveVolume` softens each stamp by kSmoothTaubinReinflate so a held
    // brush shrinks relief less. OFF by default: see the constant's note.
    SmoothTerrainEdits makeSmoothEdits(glm::ivec3 center, float radiusM,
                                       float strength,
                                       bool preserveVolume = false) const;
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
    // Append one chunk's surface emitter cells (bricks + solid-box faces) to
    // `cells`; shared by collectEmissive and collectEmissiveRegion.
    void emitChunkEmitters(int ci, const std::vector<glm::vec3>& matEmission,
                           std::vector<EmissiveCell>& cells) const;
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
    const VoxelField* m_layerSource = nullptr; // borrowed; never serialized
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
