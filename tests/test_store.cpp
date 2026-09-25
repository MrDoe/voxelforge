// Unit tests for the runtime-explicit ChunkStore (M0 foundation):
// canonical chunk indexing, adoption from the synthesized pools, queries vs
// the VoxelField oracle, cell edits and the incremental chunk rebuild.
#include "voxel/chunk_index.hpp"
#include "voxel/chunk_store.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/live_editor.hpp"
#include "voxel/surfelize.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace vf::voxel;

// Defined in test_authoring.cpp: one shared all-layers world for the suite.
LayeredWorld& testLayeredWorld();

namespace {

int latN() { return int(WORLD / VOXEL); }

// --- synthetic pool for fast, isolated edit tests ---------------------------
// One chunk with a single 8^3 brick: solid for ly < 4, air above. Block (0,0,0)
// so the editable surface sits at chunk-local y = 4.
std::vector<std::unique_ptr<ChunkPool>> syntheticPools(int ci, bool object)
{
    std::vector<std::unique_ptr<ChunkPool>> pools(kChunkCount);
    auto p = std::make_unique<ChunkPool>();
    uint32_t data[BRICK_WORDS];
    for (int z = 0; z < BRICK_N; ++z)
        for (int y = 0; y < BRICK_N; ++y)
            for (int x = 0; x < BRICK_N; ++x) {
                const size_t i = (size_t(z) * BRICK_N + y) * BRICK_N + x;
                const bool solid = y < 4;
                const int8_t sdf = solid ? -1 : 2;
                data[i * 2] = 0x40u | (0x80u << 8) | (0x20u << 16) |
                              (uint32_t(uint8_t(sdf)) << 24);
                data[i * 2 + 1] = 255u | (0u << 8) | (200u << 16) |
                                   ((4u | (object ? 0x80u : 0u)) << 24);
            }
    p->emitBrick(data);
    p->root = int32_t(p->bricks.empty() ? -1 : ((p->bricks.size() / BRICK_WORDS - 1) << 2 | 1));
    pools[size_t(ci)] = std::move(p);
    return pools;
}

ChunkStore makeSyntheticStore(int ci, bool object = false)
{
    ChunkStore s;
    const int n = latN();
    std::vector<int16_t> colTop(size_t(n) * n, 400);
    std::vector<uint8_t> colMat(size_t(n) * n, 3);
    s.adopt(syntheticPools(ci, object), colTop, colMat);
    return s;
}

// A single-brick terrain patch with deliberately varied column tops. The
// optional object spike verifies that Smooth treats object-owned columns as
// protected rather than treating their roofs as terrain samples.
ChunkStore makePatternStore(int ci, bool pit, bool objectSpike)
{
    std::vector<std::unique_ptr<ChunkPool>> pools(kChunkCount);
    auto p = std::make_unique<ChunkPool>();
    uint32_t data[BRICK_WORDS];
    const int baseY = (ci / kChunkGridN % kChunkGridN) * CHUNK_N;
    for (int z = 0; z < BRICK_N; ++z) {
        for (int y = 0; y < BRICK_N; ++y) {
            for (int x = 0; x < BRICK_N; ++x) {
                const size_t i = (size_t(z) * BRICK_N + y) * BRICK_N + x;
                int top = 4;
                if (!pit && x == 4 && z == 4)
                    top = 7; // terrain spike
                if (pit && x == 2 && z == 2)
                    top = 1; // terrain pit
                const bool object = objectSpike && x == 5 && z == 5 && y >= 5;
                if (object)
                    top = 7;
                const bool solid = y <= top;
                const int8_t sdf = solid ? -1 : 2;
                data[i * 2] = 0x40u | (0x80u << 8) | (0x20u << 16) |
                              (uint32_t(uint8_t(sdf)) << 24);
                data[i * 2 + 1] = 255u | (0u << 8) | (200u << 16) |
                                   ((4u | (object ? 0x80u : 0u)) << 24);
            }
        }
    }
    p->root = int32_t(p->emitBrick(data));
    pools[size_t(ci)] = std::move(p);

    ChunkStore s;
    const int n = latN();
    std::vector<int16_t> colTop(size_t(n) * n, int16_t(baseY + 4));
    std::vector<uint8_t> colMat(size_t(n) * n, 3);
    s.adopt(pools, colTop, colMat);
    return s;
}

// Structural validation of a rebuilt octree pool: every handle resolves and
// every brick index is in range.
bool validatePool(const ChunkPool& p)
{
    if (p.root < 0)
        return true;
    std::vector<uint32_t> stack { uint32_t(p.root) };
    while (!stack.empty()) {
        const uint32_t h = stack.back();
        stack.pop_back();
        if (h == kEmptyHandle || h == kSolidHandle)
            continue;
        if (handleIsBrick(h)) {
            if (size_t(h >> 2) * BRICK_WORDS + BRICK_WORDS > p.bricks.size())
                return false;
            continue;
        }
        const uint32_t ni = h >> 2;
        if (ni >= p.payload.size() || ni >= p.childBase.size())
            return false;
        const uint32_t base = p.childBase[ni];
        if (size_t(base) + 8 > p.handles.size())
            return false;
        for (int i = 0; i < 8; ++i) {
            const uint32_t pl = p.payload[ni];
            if (((pl & 0xFFu) >> i & 1u) == 0)
                continue; // invalid child
            stack.push_back(p.handles[base + uint32_t(i)]);
        }
    }
    return true;
}

// Walk a chunk pool exactly like common_svo.glsl does (chunk-local handles)
// and return the brick cell's sign at a chunk-local coordinate.
bool poolSolid(const ChunkPool& p, int lx, int ly, int lz)
{
    if (p.root < 0)
        return false;
    uint32_t h = uint32_t(p.root);
    int bx = 0, by = 0, bz = 0, side = CHUNK_N;
    for (int guard = 0; guard < 32; ++guard) {
        if (h == kEmptyHandle)
            return false;
        if (h == kSolidHandle)
            return true;
        if (handleIsBrick(h)) {
            const uint32_t* w = p.bricks.data() + size_t(h >> 2) * BRICK_WORDS;
            const int cx = lx & 7, cy = ly & 7, cz = lz & 7;
            const uint32_t v = w[(uint32_t(cz) * BRICK_N * BRICK_N +
                                  uint32_t(cy) * BRICK_N + uint32_t(cx)) * 2];
            return int8_t((v >> 24) & 0xFFu) <= 0;
        }
        const uint32_t ni = h >> 2;
        const int half = side / 2;
        const int oct = ((lx - bx) >= half ? 1 : 0) | ((ly - by) >= half ? 2 : 0) |
                        ((lz - bz) >= half ? 4 : 0);
        h = p.handles[p.childBase[ni] + uint32_t(oct)];
        bx += (oct & 1) * half;
        by += ((oct >> 1) & 1) * half;
        bz += ((oct >> 2) & 1) * half;
        side = half;
    }
    return false;
}

} // namespace

TEST_CASE("chunk index: z-major round trip and cell mapping")
{
    CHECK(chunkIndexOf(0, 0, 0) == 0);
    CHECK(chunkIndexOf(1, 0, 0) == 1);
    CHECK(chunkIndexOf(0, 1, 0) == kChunkGridN);
    CHECK(chunkIndexOf(0, 0, 1) == kChunkGridN * kChunkGridN);
    for (int i = 0; i < 64; ++i) {
        const int cx = (i * 7) % kChunkGridN, cy = (i * 5) % kChunkGridN,
                  cz = (i * 3) % kChunkGridN;
        int ox, oy, oz;
        chunkCoordsOf(chunkIndexOf(cx, cy, cz), ox, oy, oz);
        CHECK(ox == cx);
        CHECK(oy == cy);
        CHECK(oz == cz);
    }
    CHECK(chunkIndexOfCell(5, 6, 7) == 0);
    CHECK(chunkIndexOfCell(CHUNK_N, 0, 0) == 1);
    CHECK(chunkIndexOfCell(0, 0, CHUNK_N) == kChunkGridN * kChunkGridN);
}

TEST_CASE("chunk store: synthetic adoption, query and pool validity")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore s = makeSyntheticStore(ci);
    CHECK(s.loaded());

    const int gx = 2 * CHUNK_N, gy = 1 * CHUNK_N, gz = 3 * CHUNK_N;
    // below the surface: solid; above: air
    CHECK(s.cellAt(gx + 2, gy + 2, gz + 3).solid);
    CHECK_FALSE(s.cellAt(gx + 2, gy + 5, gz + 3).solid);
    // solid material comes from the brick
    CHECK(s.cellAt(gx + 2, gy + 2, gz + 3).mat == 4);
    CHECK(s.sample(gx + 2, gy + 2, gz + 3).d < 0.0f);
    CHECK(s.sample(gx + 2, gy + 5, gz + 3).d > 0.0f);
    // far away: empty air, still a valid query
    CHECK_FALSE(s.cellAt(0, 0, 0).solid);
    // adopted pool is exposed and structurally valid
    REQUIRE(s.pool(ci) != nullptr);
    CHECK(validatePool(*s.pool(ci)));
    CHECK(s.stats().chunks == 1);
    CHECK(s.stats().bricks == 1);
}

TEST_CASE("chunk store: smooth terrain relaxes a spike and protects objects")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makePatternStore(ci, /*pit=*/false, /*objectSpike=*/true);
    const int gx = 2 * CHUNK_N + 4;
    const int gz = 3 * CHUNK_N + 4;
    const int gy = 1 * CHUNK_N;

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ gx, gy + 7, gz }, VOXEL * 1.5f, 1.0f);
    REQUIRE(!batch.edits.empty());
    bool clearsSpike = false;
    for (const StoreEdit& e : batch.edits) {
        if (e.x == gx && e.z == gz && e.mode == StoreEdit::Mode::Clear &&
            e.y > gy + 4)
            clearsSpike = true;
        // No edit may be an object-tagged Set in this terrain-only batch.
        const bool objectSet =
            e.mode == StoreEdit::Mode::Set && !e.terrain;
        CHECK_FALSE(objectSet);
    }
    CHECK(clearsSpike);

    store.apply(batch.edits);
    store.rebuildDirty();
    CHECK(store.cellAt(gx, gy + 4, gz).solid);
    CHECK_FALSE(store.cellAt(gx, gy + 5, gz).solid);
    CHECK_FALSE(store.cellAt(gx, gy + 5, gz).obj);

    const int objectX = 2 * CHUNK_N + 5;
    const int objectZ = 3 * CHUNK_N + 5;
    CHECK(store.cellAt(objectX, gy + 7, objectZ).solid);
    CHECK(store.cellAt(objectX, gy + 7, objectZ).obj);
}

TEST_CASE("chunk store: smooth relaxes an object surface without touching terrain")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makePatternStore(ci, /*pit=*/false, /*objectSpike=*/true);
    const int gx = 2 * CHUNK_N + 5;
    const int gz = 3 * CHUNK_N + 5;
    const int gy = 1 * CHUNK_N;

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ gx, gy + 7, gz }, VOXEL * 1.5f, 1.0f);
    REQUIRE(!batch.edits.empty());
    CHECK(batch.objectSurface);
    bool clearedObject = false;
    bool createdTerrain = false;
    for (const StoreEdit& e : batch.edits) {
        if (e.x == gx && e.z == gz && e.mode == StoreEdit::Mode::Clear &&
            e.y >= gy + 5)
            clearedObject = true;
        if (e.mode == StoreEdit::Mode::Set && e.terrain)
            createdTerrain = true;
    }
    CHECK(clearedObject);
    CHECK_FALSE(createdTerrain);

    store.apply(batch.edits);
    store.rebuildDirty();
    // The whole grounded run settles onto the terrain in one stamp, so no
    // unsupported floater is left behind (the splat surface is then exactly
    // the ground plane).
    CHECK_FALSE(store.cellAt(gx, gy + 7, gz).solid);
    CHECK_FALSE(store.cellAt(gx, gy + 6, gz).solid);
    CHECK_FALSE(store.cellAt(gx, gy + 5, gz).solid);
    CHECK(store.cellAt(gx, gy + 4, gz).solid);
    CHECK_FALSE(store.cellAt(gx, gy + 4, gz).obj);

    // Convergence: a second stamp at the collapsed spot is a no-op.
    const SmoothTerrainEdits again =
        store.makeSmoothEdits({ gx, gy + 5, gz }, VOXEL * 1.5f, 1.0f);
    CHECK(again.edits.empty());
}

// Build a single object-owned Set, the same shape the brush emits.
static vf::voxel::StoreEdit objectCell(int x, int y, int z)
{
    vf::voxel::StoreEdit e;
    e.x = x;
    e.y = y;
    e.z = z;
    e.mode = vf::voxel::StoreEdit::Mode::Set;
    e.terrain = false;
    e.hasColor = true;
    e.mat = 6;
    e.r = 160;
    e.g = 80;
    e.b = 40;
    e.reflectivity = 40;
    e.roughness = 180;
    return e;
}

TEST_CASE("chunk store: smooth preserves object walls, posts and staircase steps")
{
    const int ci = chunkIndexOf(2, 1, 3);
    const int bx = 2 * CHUNK_N;
    const int by = 1 * CHUNK_N;
    const int bz = 3 * CHUNK_N;
    ChunkStore store = makePatternStore(ci, /*pit=*/false, /*objectSpike=*/false);

    std::vector<StoreEdit> seed;
    // 1-cell-thick wall in the YZ plane at local x=5, 3 cells tall.
    for (int z = 0; z < 8; ++z)
        for (int y = 5; y <= 7; ++y)
            seed.push_back(objectCell(bx + 5, by + y, bz + z));
    // Tall isolated post: a grounded run longer than the bump run, so it is a
    // feature and must not be mistaken for a bump.
    for (int y = 5; y <= 12; ++y)
        seed.push_back(objectCell(bx + 2, by + y, bz + 2));
    // 45-degree staircase: one cell per step in the XZ plane.
    for (int i = 0; i < 6; ++i)
        seed.push_back(objectCell(bx + 1 + i, by + 5 + i, bz + 6));
    store.apply(seed);
    store.rebuildDirty();

    // Flat wall face: every row of the plane has the same surface
    // coordinate, so the relaxation is at its fixed point.
    const SmoothTerrainEdits wall =
        store.makeSmoothEdits({ bx + 5, by + 6, bz + 4 }, VOXEL * 1.5f, 1.0f);
    CHECK(wall.objectSurface);
    CHECK(wall.edits.empty());

    // Tall post tip: bare ring, but the run is longer than the bump run.
    const SmoothTerrainEdits post =
        store.makeSmoothEdits({ bx + 2, by + 12, bz + 2 }, VOXEL * 1.5f, 1.0f);
    CHECK(post.objectSurface);
    CHECK(post.edits.empty());

    // Middle staircase step: the adjacent steps are its neighbourhood, so it
    // averages to its own height (the ground must not drag it down).
    const SmoothTerrainEdits stair = store.makeSmoothEdits(
        { bx + 3, by + 7, bz + 6 }, VOXEL * 1.5f, 1.0f);
    CHECK(stair.objectSurface);
    CHECK(stair.edits.empty());
}

TEST_CASE("chunk store: smooth fills an object notch without creating terrain")
{
    const int ci = chunkIndexOf(2, 1, 3);
    const int bx = 2 * CHUNK_N;
    const int by = 1 * CHUNK_N;
    const int bz = 3 * CHUNK_N;
    ChunkStore store = makePatternStore(ci, /*pit=*/false, /*objectSpike=*/false);

    std::vector<StoreEdit> seed;
    // 2-cell plate resting on the terrain.
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x)
            for (int y = 5; y <= 6; ++y)
                seed.push_back(objectCell(bx + x, by + y, bz + z));
    // One-cell notch in the plate top (local (6,6): clear of the fixture's
    // terrain spike at (4,4)).
    StoreEdit notch = objectCell(bx + 6, by + 6, bz + 6);
    notch.mode = StoreEdit::Mode::Clear;
    seed.push_back(notch);
    store.apply(seed);
    store.rebuildDirty();
    REQUIRE_FALSE(store.cellAt(bx + 6, by + 6, bz + 6).solid);

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ bx + 6, by + 5, bz + 6 }, VOXEL * 1.5f, 1.0f);
    REQUIRE(batch.objectSurface);
    bool filled = false;
    for (const StoreEdit& e : batch.edits) {
        // Object fills only: the height texture/water bed depend on it.
        if (e.mode == StoreEdit::Mode::Set)
            CHECK_FALSE(e.terrain);
        if (e.x == bx + 6 && e.y == by + 6 && e.z == bz + 6 &&
            e.mode == StoreEdit::Mode::Set && !e.terrain)
            filled = true;
    }
    CHECK(filled);

    store.apply(batch.edits);
    store.rebuildDirty();
    CHECK(store.cellAt(bx + 6, by + 6, bz + 6).solid);
    CHECK(store.cellAt(bx + 6, by + 6, bz + 6).obj);
    CHECK_FALSE(store.cellAt(bx + 6, by + 7, bz + 6).solid);
}

TEST_CASE("chunk store: smooth terrain raises a pit with terrain ownership")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makePatternStore(ci, /*pit=*/true, /*objectSpike=*/false);
    const int gx = 2 * CHUNK_N + 2;
    const int gz = 3 * CHUNK_N + 2;
    const int gy = 1 * CHUNK_N;

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ gx, gy + 1, gz }, VOXEL * 1.5f, 1.0f);
    REQUIRE(!batch.edits.empty());
    bool sawTerrainSet = false;
    for (const StoreEdit& e : batch.edits)
        if (e.mode == StoreEdit::Mode::Set && e.terrain)
            sawTerrainSet = true;
    CHECK(sawTerrainSet);

    store.apply(batch.edits);
    store.rebuildDirty();
    const StoreCell top = store.cellAt(gx, gy + 4, gz);
    CHECK(top.solid);
    CHECK_FALSE(top.obj);
    CHECK_FALSE(store.cellAt(gx, gy + 5, gz).solid);
    CHECK(store.makeSmoothEdits(
              { gx, gy + 1, gz }, VOXEL,
              std::numeric_limits<float>::quiet_NaN())
              .edits.empty());
}

TEST_CASE("chunk store: smooth finds a live terrain top beyond the search band")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makePatternStore(ci, /*pit=*/false, /*objectSpike=*/false);
    const int gx = 2 * CHUNK_N + 2;
    const int gz = 3 * CHUNK_N + 2;
    const int gy = 1 * CHUNK_N;

    // The adopted m_colTop is still gy+4, while the live column is raised far
    // beyond the 192-cell fast-search band. The next Smooth pass must see the
    // actual top, not the first Set cell above the baked surface.
    std::vector<StoreEdit> raise;
    for (int y = gy + 5; y <= gy + 220; ++y) {
        StoreEdit e;
        e.x = gx;
        e.y = y;
        e.z = gz;
        e.mode = StoreEdit::Mode::Set;
        e.terrain = true;
        e.mat = 4;
        raise.push_back(e);
    }
    store.apply(raise);

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ gx, gy + 220, gz }, VOXEL * 1.5f, 1.0f);
    REQUIRE(!batch.edits.empty());
    bool clearedBeyondBand = false;
    for (const StoreEdit& e : batch.edits)
        if (e.x == gx && e.z == gz && e.mode == StoreEdit::Mode::Clear &&
            e.y > gy + 192)
            clearedBeyondBand = true;
    CHECK(clearedBeyondBand);
}

TEST_CASE("chunk store: clear edit removes cells and rebuilds locally")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore s = makeSyntheticStore(ci);
    const int gx = 2 * CHUNK_N, gy = 1 * CHUNK_N, gz = 3 * CHUNK_N;

    const uint64_t before = s.chunkHash(ci);
    // clear the top solid layer of the 8^3 block
    std::vector<StoreEdit> edits;
    for (int z = 0; z < BRICK_N; ++z)
        for (int x = 0; x < BRICK_N; ++x) {
            StoreEdit e;
            e.mode = StoreEdit::Mode::Clear;
            e.x = gx + x; e.y = gy + 3; e.z = gz + z;
            edits.push_back(e);
        }
    s.apply(edits);
    CHECK(s.chunkDirty(ci));
    s.rebuildDirty();
    CHECK_FALSE(s.chunkDirty(ci));
    CHECK(s.chunkHash(ci) != before);

    // cleared cells read air, the layer below stays solid
    CHECK_FALSE(s.cellAt(gx + 3, gy + 3, gz + 3).solid);
    CHECK(s.cellAt(gx + 3, gy + 2, gz + 3).solid);
    // the rebuilt pool is valid and the chunk-local edit did not touch others
    REQUIRE(s.pool(ci) != nullptr);
    CHECK(validatePool(*s.pool(ci)));
    CHECK(s.stats().chunks == 1);

    // put the layer back: solid again, same material
    std::vector<StoreEdit> restore;
    for (int z = 0; z < BRICK_N; ++z)
        for (int x = 0; x < BRICK_N; ++x) {
            StoreEdit e;
            e.mode = StoreEdit::Mode::Set;
            e.x = gx + x; e.y = gy + 3; e.z = gz + z;
            e.mat = 4;
            restore.push_back(e);
        }
    s.apply(restore);
    s.rebuildDirty();
    const StoreCell back = s.cellAt(gx + 3, gy + 3, gz + 3);
    CHECK(back.solid);
    CHECK(back.mat == 4);
    REQUIRE(s.pool(ci) != nullptr);
    CHECK(validatePool(*s.pool(ci)));
}

TEST_CASE("chunk store: SDF band is restored around a removed surface")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore s = makeSyntheticStore(ci);
    const int gx = 2 * CHUNK_N, gy = 1 * CHUNK_N, gz = 3 * CHUNK_N;

    // remove a 3x3 column plug down through the surface
    std::vector<StoreEdit> edits;
    for (int y = 0; y < 5; ++y)
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x) {
                StoreEdit e;
                e.mode = StoreEdit::Mode::Clear;
                e.x = gx + 4 + x; e.y = gy + y; e.z = gz + 4 + z;
                edits.push_back(e);
            }
    s.apply(edits);
    s.rebuildDirty();

    // the plug cells are air and read positive distances
    for (int y = 0; y < 5; ++y) {
        const VoxelField::Sample sm = s.sample(gx + 4, gy + y, gz + 4);
        CHECK(sm.d > 0.0f);
    }
    // a cell adjacent to the plug wall (outside it) is solid
    CHECK(s.cellAt(gx + 5 + 1, gy + 2, gz + 4).solid);
    // air above the plug keeps a finite positive distance to the new walls
    const VoxelField::Sample above = s.sample(gx + 4, gy + 6, gz + 4);
    CHECK(above.d > 0.0f);
    CHECK(above.d <= 32.0f * VOXEL);
    REQUIRE(s.pool(ci) != nullptr);
    CHECK(validatePool(*s.pool(ci)));
}

TEST_CASE("chunk store: paint changes material without changing solidity")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore s = makeSyntheticStore(ci);
    const int gx = 2 * CHUNK_N, gy = 1 * CHUNK_N, gz = 3 * CHUNK_N;

    StoreEdit e;
    e.mode = StoreEdit::Mode::Paint;
    e.x = gx + 1; e.y = gy + 1; e.z = gz + 1;
    e.mat = 7;
    s.apply({ e });
    s.rebuildDirty();
    const StoreCell c = s.cellAt(gx + 1, gy + 1, gz + 1);
    CHECK(c.solid);
    CHECK(c.mat == 7);
    // palette colour of mat 7 was applied
    CHECK(c.r == uint8_t(kPalette[7].r * 255.0f));
}

TEST_CASE("chunk store: rebuild from the baked world matches the VoxelField oracle")
{
    LayeredWorld& lw = testLayeredWorld();
    const VoxelField& field = lw.field();
    ChunkStore& store = lw.store();
    REQUIRE(store.loaded());

    // 1) sign agreement at terrain surfaces: walk columns around colTop.
    const auto& colTops = field.colTops();
    const int n = latN();
    size_t solidSeen = 0, signMismatch = 0, matMismatch = 0, flagMismatch = 0;
    int printed = 0;
    for (int z = 7; z < n; z += 29)
        for (int x = 11; x < n; x += 31) {
            const int16_t top = colTops[size_t(z) * n + size_t(x)];
            if (top < 6)
                continue;
            for (int y = top - 5; y <= top + 5; y += 2) {
                const VoxelField::Sample fs = field.sample(x, y, z);
                const StoreCell sc = store.cellAt(x, y, z);
                const bool sStore = store.sample(x, y, z).d < 0.0f;
                const bool sField = fs.d < 0.0f;
                // the bake quantises the SDF to whole voxels, so a cell whose
                // field distance is within one voxel of the surface may land
                // on the other side of the sign test
                if (sStore != sField && std::fabs(fs.d) > 2.0f * VOXEL) {
                    if (printed++ < 10)
                        fprintf(stderr,
                                "sign mismatch (%d,%d,%d) store %d %.3f mat %u | field %d %.3f mat %u\n",
                                x, y, z, int(sStore), store.sample(x, y, z).d,
                                sc.mat, int(sField), fs.d, fs.mat);
                    ++signMismatch;
                } else if (sStore) {
                    ++solidSeen;
                    if (sc.obj != fs.obj)
                        ++flagMismatch;
                    // Solid-state / box cells are pure terrain and must carry
                    // the column material. Brick materials are copied verbatim
                    // from the bake and are not re-derived here.
                    if (sc.sdfRaw == -127 && sc.mat != fs.mat) {
                        if (printed++ < 10)
                            fprintf(stderr,
                                    "mat mismatch (%d,%d,%d) store mat %u | field mat %u\n",
                                    x, y, z, sc.mat, fs.mat);
                        ++matMismatch;
                    }
                }
            }
        }
    fprintf(stderr, "surface: solid %zu signMismatch %zu matMismatch %zu flagMismatch %zu\n",
            solidSeen, signMismatch, matMismatch, flagMismatch);
    CHECK(solidSeen > 1000);
    CHECK(signMismatch == 0);
    CHECK(flagMismatch == 0);
    CHECK(matMismatch == 0);

    // 2) coarse sign agreement over the whole volume (terrain + objects)
    size_t coarseMismatch = 0;
    int cprinted = 0;
    for (int z = 3; z < n; z += 37)
        for (int y = 3; y < n; y += 37)
            for (int x = 5; x < n; x += 41) {
                const bool sStore = store.sample(x, y, z).d < 0.0f;
                const VoxelField::Sample fs = field.sample(x, y, z);
                const bool sField = fs.d < 0.0f;
                if (sStore != sField && std::fabs(fs.d) > 2.0f * VOXEL) {
                    if (cprinted++ < 10)
                        fprintf(stderr,
                                "coarse mismatch (%d,%d,%d) store %d %.3f mat %u | field %d %.3f mat %u\n",
                                x, y, z, int(sStore), store.sample(x, y, z).d,
                                store.cellAt(x, y, z).mat, int(sField), fs.d, fs.mat);
                    ++coarseMismatch;
                }
            }
    fprintf(stderr, "coarse mismatches %zu\n", coarseMismatch);
    CHECK(coarseMismatch == 0);
}

TEST_CASE("chunk store: per-chunk surfels follow store edits")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();

    // find the terrain surface above the middle of chunk column (8, *, 8)
    const int gx = 8 * CHUNK_N + 32, gz = 8 * CHUNK_N + 32;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(8, topY / CHUNK_N, 8);
    const SurfelParams sp;
    auto maxSurfelY = [](const std::vector<Surfel>& v) {
        float m = -1e9f;
        for (const Surfel& s : v)
            m = std::max(m, s.pos_rU.y);
        return m;
    };

    const std::vector<Surfel> before = buildChunkSurfels(store, ci, sp);
    CHECK(!before.empty());
    // deterministic: same store state => identical output
    CHECK(before == buildChunkSurfels(store, ci, sp));
    // a neighbouring chunk's surfels are untouched by edits here
    const int other = chunkIndexOf(11, topY / CHUNK_N, 11);
    const std::vector<Surfel> otherBefore = buildChunkSurfels(store, other, sp);

    // raise a 3x3, 5-cell-tall plateau on the surface
    std::vector<StoreEdit> edits;
    for (int dy = 1; dy <= 5; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                StoreEdit e;
                e.mode = StoreEdit::Mode::Set;
                e.x = gx + dx; e.y = topY + dy; e.z = gz + dz;
                e.mat = 2;
                edits.push_back(e);
            }
    store.apply(edits);
    store.rebuildDirty();

    const std::vector<Surfel> after = buildChunkSurfels(store, ci, sp);
    CHECK(after != before);
    // The plateau may cross into the chunk above (the test column's surface
    // sits near the top of its chunk): the per-chunk rebuild follows the
    // edited chunks, so check the union of both runs for the raised top.
    std::vector<Surfel> raised = after;
    const int ciAbove = chunkIndexOf(8, (topY + 6) / CHUNK_N, 8);
    if (ciAbove != ci)
        for (const Surfel& s : buildChunkSurfels(store, ciAbove, sp))
            raised.push_back(s);
    CHECK(maxSurfelY(raised) > maxSurfelY(before) + 0.2f);
    // a surfel was emitted on top of the new plateau with an up normal
    bool top = false;
    for (const Surfel& s : raised) {
        if (std::fabs(s.pos_rU.y - (-51.2f + (topY + 5 + 1.0f) * VOXEL)) < 0.25f &&
            glm::vec3(s.normal_rV).y > 0.5f)
            top = true;
    }
    CHECK(top);
    // the far chunk is unaffected by the edit
    CHECK(buildChunkSurfels(store, other, sp) == otherBefore);
}

TEST_CASE("chunk store: set then clear round-trips the chunk (undo path)")
{
    // Undo replays the pre-stroke state of every touched cell as Set/Clear
    // edits (App::undoEdit). The store must return to the exact canonical
    // state - hash and surfels included - or an undone stroke lingers.
    const int ci = 0;
    ChunkStore s = makeSyntheticStore(ci);
    const SurfelParams sp;
    auto maxY = [](const std::vector<Surfel>& v) {
        float m = -1e9f;
        for (const Surfel& x : v)
            m = std::max(m, x.pos_rU.y);
        return m;
    };
    const uint64_t h0 = s.chunkHash(ci);
    const std::vector<Surfel> run0 = buildChunkSurfels(s, ci, sp);

    // raise material above the synthetic surface (solid for ly < 4)
    std::vector<StoreEdit> set;
    for (int dz = 1; dz <= 2; ++dz)
        for (int y = 5; y <= 7; ++y) {
            StoreEdit e;
            e.mode = StoreEdit::Mode::Set;
            e.x = 2; e.y = y; e.z = dz;
            e.mat = 6;
            e.hasColor = true;
            e.r = 10; e.g = 200; e.b = 30;
            set.push_back(e);
        }
    s.apply(set);
    s.rebuildDirty();
    for (const StoreEdit& e : set)
        CHECK(s.cellAt(e.x, e.y, e.z).solid);
    CHECK(s.chunkHash(ci) != h0);
    const std::vector<Surfel> run1 = buildChunkSurfels(s, ci, sp);
    CHECK(run1 != run0);
    CHECK(maxY(run1) > maxY(run0) + 0.2f);

    // undo: clear exactly the cells that were air before (the recorded inverse)
    std::vector<StoreEdit> clear;
    for (const StoreEdit& e : set) {
        StoreEdit inv;
        inv.mode = StoreEdit::Mode::Clear;
        inv.x = e.x; inv.y = e.y; inv.z = e.z;
        clear.push_back(inv);
    }
    s.apply(clear);
    s.rebuildDirty();
    for (const StoreEdit& e : clear)
        CHECK(!s.cellAt(e.x, e.y, e.z).solid);
    // The added geometry is gone: nothing sits above the original surface.
    // (Byte-exact equality would be too strict: the edited chunk's SDF band is
    // recomputed with the store's chamfer, so its shading differs from the
    // bake's until a full rebuild - see the live-editor round-trip test.)
    const std::vector<Surfel> after = buildChunkSurfels(s, ci, sp);
    CHECK(maxY(after) <= maxY(run0) + 0.01f);
    CHECK(s.chunkHash(ci) != 0); // still a valid, loaded chunk
}

TEST_CASE("chunk store: live editor set-then-clear round-trips the cached run")
{
    // The app's undo replays the recorded pre-stroke cells through
    // LiveEditor::stamp (region-limited refresh). Added geometry must leave the
    // cached run again, otherwise an undone stroke keeps rendering.
    ChunkStore s = makeSyntheticStore(0);
    LiveEditor le;
    le.attach(&s);
    const SurfelParams sp;
    le.seedFromStore(0, sp);
    const std::vector<Surfel> run0 = le.chunkRun(0, sp);
    REQUIRE(!run0.empty());

    auto cell = [](int y, int z, StoreEdit::Mode mode) {
        StoreEdit e;
        e.mode = mode;
        e.x = 2; e.y = y; e.z = z; e.mat = 6;
        e.hasColor = true; e.r = 10; e.g = 200; e.b = 30;
        return e;
    };
    std::vector<StoreEdit> up;
    for (int y = 5; y <= 7; ++y)
        for (int z = 1; z <= 2; ++z)
            up.push_back(cell(y, z, StoreEdit::Mode::Set));
    CHECK(le.stamp(up, sp) == std::vector<int> { 0 });
    const std::vector<Surfel> run1 = le.chunkRun(0, sp);
    CHECK(run1 != run0);

    std::vector<StoreEdit> down;
    for (const StoreEdit& e : up)
        down.push_back(cell(e.y, e.z, StoreEdit::Mode::Clear));
    CHECK(le.stamp(down, sp) == std::vector<int> { 0 });
    CHECK(!s.cellAt(2, 6, 1).solid);
    // The cached run must lose the added geometry (the undo bug was a
    // zero-surfel chunk patch that the GPU never received); byte-exact
    // equality with run0 is not expected - the region refresh re-derives the
    // edited cells from the store's SDF band, whose shading differs from the
    // initial seed until a full rebuild.
    auto maxY = [](const std::vector<Surfel>& v) {
        float m = -1e9f;
        for (const Surfel& x : v)
            m = std::max(m, x.pos_rU.y);
        return m;
    };
    const std::vector<Surfel>& r2 = le.chunkRun(0, sp);
    CHECK(maxY(r2) <= maxY(run0) + 0.01f);
    CHECK(r2.size() <= run1.size());
}

TEST_CASE("chunk store: live editor keeps hard-edge bridges outside the micro tail")
{
    ChunkStore store = makeSyntheticStore(0, true);
    LiveEditor editor;
    editor.attach(&store);
    SurfelParams params;
    params.edgeShrink = 0.35f;
    params.edgeFill = true;
    params.microDetail = false;
    params.anisotropy = false;

    editor.seedFromStore(0, params);
    const uint32_t parents = uint32_t(editor.chunkSurfels(0, params).size());
    const uint32_t edges = editor.edgeCountOf(0);
    const std::vector<Surfel>& run = editor.chunkRun(0, params);
    REQUIRE(parents > 0);
    CHECK(edges > 0);
    CHECK(run.size() == parents + edges);
    CHECK(editor.microStartOf(0) == parents + edges);
    for (uint32_t i = parents; i < run.size(); ++i) {
        CHECK(run[i].pos_rU.w > 0.0f);
        CHECK(run[i].normal_rV.w > 0.0f);
        CHECK(glm::length(glm::vec3(run[i].tan_aspect)) > 0.99f);
    }

    // A full region refresh removes and rebuilds both segments together; it
    // must not duplicate bridges or fold them into the material micro tail.
    CHECK(editor.refreshRegion(0, glm::ivec3(0), glm::ivec3(CHUNK_N), params));
    CHECK(editor.chunkSurfels(0, params).size() == parents);
    CHECK(editor.edgeCountOf(0) == edges);
    CHECK(editor.chunkRun(0, params).size() == parents + edges);
    CHECK(editor.microStartOf(0) == parents + edges);
}

TEST_CASE("chunk store: rebuild preserves surface density of untouched dirty chunks")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    // find the terrain surface above the middle of chunk column (6, *, 6)
    const int gx = 6 * CHUNK_N + 32, gz = 6 * CHUNK_N + 32;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(6, topY / CHUNK_N, 6);
    const SurfelParams sp;
    const size_t surfaceBefore = buildChunkSurfels(store, ci, sp).size();
    // a small edit in the SAME chunk (the rebuild is per chunk anyway)
    StoreEdit e;
    e.mode = StoreEdit::Mode::Set;
    e.x = gx; e.y = topY + 1; e.z = gz;
    e.mat = 2;
    store.apply({ e });
    store.rebuildDirty();
    const size_t surfaceAfter = buildChunkSurfels(store, ci, sp).size();
    fprintf(stderr, "density chunk %d: before %zu after %zu\n", ci, surfaceBefore,
            surfaceAfter);
    CHECK(surfaceAfter + 100 >= surfaceBefore);
    CHECK(surfaceAfter <= surfaceBefore + 100);
}

TEST_CASE("chunk store: localized rebuild matches a full rebuild")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    const int gx = 6 * CHUNK_N + 20, gz = 6 * CHUNK_N + 20;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(6, topY / CHUNK_N, 6);

    // a small plateau edit
    std::vector<StoreEdit> edits;
    for (int dy = 1; dy <= 4; ++dy)
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                StoreEdit e;
                e.mode = StoreEdit::Mode::Set;
                e.x = gx + dx; e.y = topY + dy; e.z = gz + dz;
                e.mat = 2;
                edits.push_back(e);
            }
    // a carve nearby (both add + remove in the same rebuild)
    for (int dy = 0; dy >= -3; --dy) {
        StoreEdit e;
        e.mode = StoreEdit::Mode::Clear;
        e.x = gx + 8; e.y = topY + dy; e.z = gz + 8;
        edits.push_back(e);
    }
    // normalize the chunk to the rebuild convention first (the adopted bake
    // used an exact-ish Dijkstra; the live rebuild uses a chamfer transform)
    store.rebuildFull(ci);
    store.apply(edits);
    store.rebuildDirty(); // localized

    // snapshot the localized chunk cell by cell
    struct CellRec {
        bool solid;
        uint8_t mat;
        int8_t raw;
    };
    std::vector<CellRec> localized;
    const int cx0 = (ci % kChunkGridN) * CHUNK_N;
    const int cy0 = ((ci / kChunkGridN) % kChunkGridN) * CHUNK_N;
    const int cz0 = (ci / (kChunkGridN * kChunkGridN)) * CHUNK_N;
    for (int z = cz0; z < cz0 + CHUNK_N; ++z)
        for (int y = cy0; y < cy0 + CHUNK_N; ++y)
            for (int x = cx0; x < cx0 + CHUNK_N; ++x) {
                const StoreCell c = store.cellAt(x, y, z);
                localized.push_back({ c.solid, c.mat, c.sdfRaw });
            }

    store.rebuildFull(ci); // force the full-chunk path

    // Geometry AND distances must agree exactly: the localized rebuild only
    // touches the region that the edit can affect, and the full rebuild
    // re-derives the same bytes (verified cell by cell below).
    size_t idx = 0, signDiff = 0, matDiff = 0, distDiff = 0;
    for (int z = cz0; z < cz0 + CHUNK_N; ++z)
        for (int y = cy0; y < cy0 + CHUNK_N; ++y)
            for (int x = cx0; x < cx0 + CHUNK_N; ++x) {
                const StoreCell c = store.cellAt(x, y, z);
                const CellRec& r = localized[idx++];
                if (c.solid != r.solid)
                    ++signDiff;
                else if (c.solid && c.mat != r.mat)
                    ++matDiff;
                else if (c.sdfRaw != r.raw)
                    ++distDiff;
            }
    fprintf(stderr, "localized vs full: signDiff %zu matDiff %zu distDiff %zu\n",
            signDiff, matDiff, distDiff);
    CHECK(signDiff == 0);
    CHECK(matDiff == 0);
    CHECK(distDiff == 0);

    // the edit is still visible: a solid cell on top of the plateau
    CHECK(store.cellAt(gx, topY + 4, gz).solid);
    // and the carve reads air
    CHECK_FALSE(store.cellAt(gx + 8, topY, gz + 8).solid);
}

TEST_CASE("chunk store: a deep clear stays air under the terrain boxes")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    const int gx = 4 * CHUNK_N + 32, gz = 4 * CHUNK_N + 32;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 40);

    // Clear a shaft well below the SDF band. Deep blocks end up entirely air,
    // so the localized rebuild drops their brick - but the chunk's SolidBoxes
    // still cover them, and a box must never resurrect a cell an edit cleared
    // (bricks win for BOTH signs).
    const int ci = chunkIndexOfCell(gx, topY - 20, gz);
    store.rebuildFull(ci); // normalize to the rebuild convention
    std::vector<StoreEdit> edits;
    for (int y = topY; y >= topY - 30; --y) {
        StoreEdit e;
        e.mode = StoreEdit::Mode::Clear;
        e.x = gx; e.y = y; e.z = gz;
        edits.push_back(e);
    }
    store.apply(edits);
    store.rebuildDirty();
    size_t stillSolid = 0;
    for (int y = topY; y >= topY - 30; --y)
        if (store.cellAt(gx, y, gz).solid)
            ++stillSolid;
    fprintf(stderr, "deep clear: %zu of 31 cells still solid\n", stillSolid);
    CHECK(stillSolid == 0);

    // the localized rebuild still matches a full rebuild cell by cell
    struct CellRec {
        bool solid;
        int8_t raw;
    };
    std::vector<CellRec> localized;
    const int cx0 = (ci % kChunkGridN) * CHUNK_N;
    const int cy0 = ((ci / kChunkGridN) % kChunkGridN) * CHUNK_N;
    const int cz0 = (ci / (kChunkGridN * kChunkGridN)) * CHUNK_N;
    for (int z = cz0; z < cz0 + CHUNK_N; ++z)
        for (int y = cy0; y < cy0 + CHUNK_N; ++y)
            for (int x = cx0; x < cx0 + CHUNK_N; ++x) {
                const StoreCell c = store.cellAt(x, y, z);
                localized.push_back({ c.solid, c.sdfRaw });
            }
    store.rebuildFull(ci);
    size_t signDiff = 0, distDiff = 0, idx = 0;
    for (int z = cz0; z < cz0 + CHUNK_N; ++z)
        for (int y = cy0; y < cy0 + CHUNK_N; ++y)
            for (int x = cx0; x < cx0 + CHUNK_N; ++x) {
                const StoreCell c = store.cellAt(x, y, z);
                const CellRec& r = localized[idx++];
                if (c.solid != r.solid)
                    ++signDiff;
                else if (c.sdfRaw != r.raw)
                    ++distDiff;
            }
    fprintf(stderr, "deep clear localized vs full: signDiff %zu distDiff %zu\n",
            signDiff, distDiff);
    CHECK(signDiff == 0);
    CHECK(distDiff == 0);
}

TEST_CASE("chunk store: rebuilt pool exposes edited geometry (SVO path)")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    const int gx = 6 * CHUNK_N + 20, gz = 6 * CHUNK_N + 20;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(6, topY / CHUNK_N, 6);

    // raise a small plateau (same Set edits the live tool uses)
    std::vector<StoreEdit> edits;
    for (int dy = 1; dy <= 5; ++dy)
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                StoreEdit e;
                e.mode = StoreEdit::Mode::Set;
                e.x = gx + dx; e.y = topY + dy; e.z = gz + dz;
                e.mat = 2;
                edits.push_back(e);
            }
    store.apply(edits);
    store.rebuildDirty();

    const auto& pool = store.pool(ci);
    REQUIRE(pool != nullptr);
    const int lx = gx - 6 * CHUNK_N, lz = gz - 6 * CHUNK_N;
    const int cy0 = (topY / CHUNK_N) * CHUNK_N;
    // the plateau is solid in the pool up to its top ...
    CHECK(poolSolid(*pool, lx, (topY + 5) - cy0, lz));
    // ... and air one cell above it (what the SVO raymarch must see)
    CHECK_FALSE(poolSolid(*pool, lx, (topY + 6) - cy0, lz));
    // canonical and pool agree at the sampled plateau cells
    CHECK(store.cellAt(gx, topY + 5, gz).solid);
    CHECK_FALSE(store.cellAt(gx, topY + 6, gz).solid);

    // pool sign == canonical sign across a deterministic sample of the chunk
    size_t mismatches = 0;
    const int cx0 = 6 * CHUNK_N, cz0 = 6 * CHUNK_N;
    for (int dz = 1; dz < CHUNK_N; dz += 9)
        for (int dy = 1; dy < CHUNK_N; dy += 7)
            for (int dx = 1; dx < CHUNK_N; dx += 11) {
                const bool canon = store.cellAt(cx0 + dx, cy0 + dy, cz0 + dz).solid;
                if (poolSolid(*pool, dx, dy, dz) != canon)
                    ++mismatches;
            }
    CHECK(mismatches == 0);
}

TEST_CASE("chunk store: overlay serialize/apply round trip")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    const int gx = 7 * CHUNK_N + 15, gz = 7 * CHUNK_N + 15;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(7, topY / CHUNK_N, 7);

    // edit, rebuild, snapshot
    std::vector<StoreEdit> edits;
    for (int dy = 1; dy <= 4; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                StoreEdit e;
                e.mode = StoreEdit::Mode::Set;
                e.x = gx + dx; e.y = topY + dy; e.z = gz + dz;
                e.mat = 4;
                edits.push_back(e);
            }
    store.apply(edits);
    store.rebuildDirty();
    CHECK(store.hasEditedChunks());
    CHECK(std::find(store.editedChunks().begin(), store.editedChunks().end(), ci) !=
          store.editedChunks().end());
    const uint64_t wanted = store.chunkHash(ci);
    const std::vector<uint8_t> bytes = store.serializeEdited();
    CHECK(!bytes.empty());

    // clobber the chunk differently, then restore from the section payload
    StoreEdit clobber;
    clobber.mode = StoreEdit::Mode::Clear;
    clobber.x = gx; clobber.y = topY + 1; clobber.z = gz;
    store.apply({ clobber });
    store.rebuildDirty();
    CHECK(store.chunkHash(ci) != wanted);
    REQUIRE(store.applyEdited(bytes));
    CHECK(store.chunkHash(ci) == wanted);
    CHECK(store.cellAt(gx, topY + 4, gz).solid);

    // save/load through a real VXW v2 file (atomic writer path)
    const std::string path =
        (std::filesystem::temp_directory_path() / "vf_test_overlay.vxw").string();
    REQUIRE(store.saveOverlay(path));
    store.apply({ clobber });
    store.rebuildDirty();
    CHECK(store.chunkHash(ci) != wanted);
    REQUIRE(store.loadOverlay(path));
    CHECK(store.chunkHash(ci) == wanted);
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

TEST_CASE("chunk store: rebuilding a dirty Solid neighbour is a no-op (no crash)")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    // find a surface column and clear a cell at the BOTTOM of its chunk so the
    // chunk below (a Solid promoted terrain interior) gets marked dirty
    const int gx = 8 * CHUNK_N + 30, gz = 8 * CHUNK_N + 30;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int cy = topY / CHUNK_N;
    const int ci = chunkIndexOf(8, cy, 8);
    const int below = chunkIndexOf(8, cy - 1, 8);

    StoreEdit e;
    e.mode = StoreEdit::Mode::Clear;
    e.x = gx;
    e.y = (cy - 1) * CHUNK_N + CHUNK_N - 1; // bottom cell of the surface chunk
    e.z = gz;
    store.apply({ e });
    const std::vector<int> dirty = store.dirtyChunks();
    // the edit near the face must have flagged the chunk below too
    CHECK(std::find(dirty.begin(), dirty.end(), below) != dirty.end());
    store.rebuildDirty(); // crashed before the Solid-slot guard
    CHECK(store.cellAt(gx, e.y, gz).solid == false);
    CHECK(store.cellAt(gx, e.y - 3, gz).solid); // still solid below
}

TEST_CASE("chunk store: adoption is deterministic")
{
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore a = makeSyntheticStore(ci);
    ChunkStore b = makeSyntheticStore(ci);
    CHECK(a.stats().chunks == b.stats().chunks);
    CHECK(a.stats().bricks == b.stats().bricks);
    for (int c = 0; c < kChunkCount; c += 97)
        CHECK(a.chunkHash(c) == b.chunkHash(c));
}

TEST_CASE("chunk store: micro-detail is deterministic and follows edits")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    const int gx = 7 * CHUNK_N + 24, gz = 7 * CHUNK_N + 24;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(7, topY / CHUNK_N, 7);
    glm::ivec3 lo, hi;
    chunkCoordsOf(ci, lo.x, lo.y, lo.z);
    lo *= CHUNK_N;
    hi = lo + glm::ivec3(CHUNK_N);

    const SurfelParams sp;
    const SurfelRange rg = buildChunkSurfelsRange(store, ci, lo, hi, sp);
    REQUIRE(!rg.surfels.empty());
    CHECK(rg.keys.size() == rg.surfels.size());
    const std::vector<Surfel> micros = buildMicroSurfels(rg.keys, rg.surfels);
    CHECK(!micros.empty());
    // deterministic: the same cell always yields the same micro geometry
    CHECK(micros == buildMicroSurfels(rg.keys, rg.surfels));
    // bounded fan-out (0-3 children per base surfel)
    CHECK(micros.size() < rg.surfels.size() * 3u);
    for (const Surfel& m : micros) {
        CHECK(std::isfinite(m.pos_rU.x));
        CHECK(std::isfinite(m.pos_rU.y));
        CHECK(std::isfinite(m.pos_rU.z));
        CHECK(m.pos_rU.w > 0.0f);
        CHECK(m.normal_rV.w > 0.0f);
        const float nl = glm::length(glm::vec3(m.normal_rV));
        CHECK(nl > 0.99f);
        CHECK(nl < 1.01f);
    }
    // every micro sits on its parent cell (tangent offset + lift), never
    // detached from the base surface
    for (size_t i = 0; i < std::min<size_t>(micros.size(), 64); ++i) {
        float best = 1e9f;
        for (const Surfel& b : rg.surfels)
            best = std::min(best, glm::length(glm::vec3(micros[i].pos_rU) -
                                              glm::vec3(b.pos_rU)));
        CHECK(best < 0.15f);
    }
}

TEST_CASE("chunk store: live editor keeps micro detail across stamps")
{
    LayeredWorld& lw = testLayeredWorld();
    ChunkStore& store = lw.store();
    const int n = latN();
    const int gx = 5 * CHUNK_N + 12, gz = 5 * CHUNK_N + 12;
    int topY = -1;
    for (int y = n - 2; y > 0; --y)
        if (store.cellAt(gx, y, gz).sdfRaw <= 0) {
            topY = y;
            break;
        }
    REQUIRE(topY > 10);
    const int ci = chunkIndexOf(5, topY / CHUNK_N, 5);

    LiveEditor ed;
    ed.attach(&store);
    SurfelParams sp;
    sp.microDetail = true;

    std::vector<StoreEdit> edits;
    for (int dy = 1; dy <= 3; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                StoreEdit e;
                e.mode = StoreEdit::Mode::Set;
                e.x = gx + dx;
                e.y = topY + dy;
                e.z = gz + dz;
                e.mat = 2;
                edits.push_back(e);
            }
    const std::vector<int> changed = ed.stamp(edits, sp);
    REQUIRE(std::find(changed.begin(), changed.end(), ci) != changed.end());

    const uint32_t baseN = uint32_t(ed.chunkSurfels(ci, sp).size());
    const std::vector<Surfel>& run = ed.chunkRun(ci, sp);
    CHECK(ed.microStartOf(ci) == baseN);
    CHECK(run.size() > baseN); // a live-patched chunk keeps its micro tail
    const std::vector<Surfel>* runPtr = &ed.chunkRun(ci, sp);
    CHECK(runPtr == &run); // cached, no spurious rebuild

    // a second stamp re-marks the run dirty and regenerates base + micros
    StoreEdit paint;
    paint.mode = StoreEdit::Mode::Paint;
    paint.x = gx + 1;
    paint.y = topY + 3;
    paint.z = gz + 1;
    paint.hasColor = true;
    paint.r = 200;
    paint.g = 10;
    paint.b = 10;
    const std::vector<int> changed2 = ed.stamp({ paint }, sp);
    CHECK(std::find(changed2.begin(), changed2.end(), ci) != changed2.end());
    const std::vector<Surfel>& run2 = ed.chunkRun(ci, sp);
    CHECK(ed.microStartOf(ci) <= run2.size());
    CHECK(run2.size() > ed.microStartOf(ci));

    // microDetail off: the same cache collapses to the base run
    SurfelParams off = sp;
    off.microDetail = false;
    const std::vector<Surfel>& baseRun = ed.chunkRun(ci, off);
    CHECK(baseRun.size() <= run2.size());
    CHECK(ed.microStartOf(ci) == uint32_t(baseRun.size()));
}
