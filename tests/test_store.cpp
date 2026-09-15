// Unit tests for the runtime-explicit ChunkStore (M0 foundation):
// canonical chunk indexing, adoption from the synthesized pools, queries vs
// the VoxelField oracle, cell edits and the incremental chunk rebuild.
#include "voxel/chunk_index.hpp"
#include "voxel/chunk_store.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/surfelize.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
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
std::vector<std::unique_ptr<ChunkPool>> syntheticPools(int ci)
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
                data[i * 2 + 1] = 255u | (0u << 8) | (200u << 16) | (4u << 24);
            }
    p->emitBrick(data);
    p->root = int32_t(p->bricks.empty() ? -1 : ((p->bricks.size() / BRICK_WORDS - 1) << 2 | 1));
    pools[size_t(ci)] = std::move(p);
    return pools;
}

ChunkStore makeSyntheticStore(int ci)
{
    ChunkStore s;
    const int n = latN();
    std::vector<int16_t> colTop(size_t(n) * n, 400);
    std::vector<uint8_t> colMat(size_t(n) * n, 3);
    s.adopt(syntheticPools(ci), colTop, colMat);
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
    CHECK(maxSurfelY(after) > maxSurfelY(before) + 0.2f);
    // a surfel was emitted on top of the new plateau with an up normal
    bool top = false;
    for (const Surfel& s : after) {
        if (std::fabs(s.pos_rU.y - (-51.2f + (topY + 5 + 1.0f) * VOXEL)) < 0.25f &&
            glm::vec3(s.normal_rV).y > 0.5f)
            top = true;
    }
    CHECK(top);
    // the far chunk is unaffected by the edit
    CHECK(buildChunkSurfels(store, other, sp) == otherBefore);
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
