// Unit tests for the runtime-explicit ChunkStore (M0 foundation):
// canonical chunk indexing, adoption from the synthesized pools, queries vs
// the VoxelField oracle, cell edits and the incremental chunk rebuild.
#include "voxel/chunk_index.hpp"
#include "voxel/chunk_store.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/live_editor.hpp"
#include "voxel/surfelize.hpp"
#include <doctest/doctest.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
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

// Minimal octree builder for the terrain fixtures: leaves are BRICK_N^3 cells,
// an internal node of side S has 8 children of side S/2, and a node is emitted
// only when it contains at least one non-empty brick. `topAt` returns the
// column height in CHUNK-LOCAL cells, or -1 for a void column. Mirrors
// ChunkStore::buildPoolOnly's handle encoding (child index bits: x, y, z).
int32_t buildTestOctree(ChunkPool& p, int side, int ox0, int oy0, int oz0,
                        const std::function<int(int, int)>& topAt)
{
    if (side == BRICK_N) {
        uint32_t data[BRICK_WORDS];
        bool any = false;
        for (int z = 0; z < BRICK_N; ++z)
            for (int y = 0; y < BRICK_N; ++y)
                for (int x = 0; x < BRICK_N; ++x) {
                    const size_t i = (size_t(z) * BRICK_N + y) * BRICK_N + x;
                    // Both the height lookup and the test must be in the SAME
                    // (chunk-local) space. Comparing a chunk-local top against
                    // the leaf-LOCAL y (0..7) silently fills every brick in a
                    // column whose top is >= 7, which is how a "ramp" fixture
                    // came out solid to the chunk ceiling. makeRidgeStore used
                    // to mask it with a `y > 7` guard inside its lambda; taking
                    // the origin as a parameter removes the whole class of bug.
                    const int top = topAt(ox0 + x, oz0 + z);
                    const bool solid = top >= 0 && (oy0 + y) <= top;
                    any |= solid;
                    const int8_t sdf = solid ? -1 : 2;
                    data[i * 2] = 0x40u | (0x80u << 8) | (0x20u << 16) |
                                  (uint32_t(uint8_t(sdf)) << 24);
                    data[i * 2 + 1] = 255u | (0u << 8) | (200u << 16) |
                                       (4u << 24);
                }
        return any ? int32_t(p.emitBrick(data)) : -1;
    }
    const int half = side / 2;
    const uint32_t nodeH = p.allocNode();
    const uint32_t base = p.childBase[nodeIndexOf(nodeH)];
    uint32_t validMask = 0;
    for (int i = 0; i < 8; ++i) {
        const int ox = (i & 1) * half, oy = ((i >> 1) & 1) * half,
                  oz = ((i >> 2) & 1) * half;
        const int32_t child = buildTestOctree(p, half, ox0 + ox, oy0 + oy,
                                              oz0 + oz, topAt);
        p.handles[base + i] = uint32_t(child);
        if (child >= 0)
            validMask |= 1u << i;
    }
    if (validMask == 0)
        return -1;
    // validMask only: the children are BRICKS, not solid terminals, so setting
    // solidMask would make adopt() promote them to -2 (fully solid).
    p.payload[nodeIndexOf(nodeH)] = validMask;
    return int32_t(nodeH);
}

// A square terrain patch centred on the chunk, 2*halfCells cells across and 8
// cells tall, with a RIDGE of half-width `ridgeHalf` running along z through
// the chunk's mid-x at height `ridgeTop`; the rest is flat at `baseTop`.
// Deliberately wider than any brush kernel support: a patch narrower than the
// kernel makes every outer sample a void, the coverage guard then skips the
// whole footprint, and the test would pass for the wrong reason.
ChunkStore makeRidgeStore(int ci, int halfCells, int ridgeHalf,
                          int baseTop = 4, int ridgeTop = 7)
{
    const int baseY = (ci / kChunkGridN % kChunkGridN) * CHUNK_N;
    int ccx, ccy, ccz;
    chunkCoordsOf(ci, ccx, ccy, ccz);
    const int gx0 = ccx * CHUNK_N, gz0 = ccz * CHUNK_N;
    auto p = std::make_unique<ChunkPool>();
    // Clamped to the chunk: a patch cannot extend outside it, and the negative
    // index would be a wild colTop write.
    const int lo = std::max(0, CHUNK_N / 2 - halfCells);
    const int hi = std::min(CHUNK_N, CHUNK_N / 2 + halfCells);
    const int mid = CHUNK_N / 2;
    auto topAt = [&](int x, int z) {
        if (x < lo || x >= hi || z < lo || z >= hi)
            return -1; // outside the patch: void, not terrain
        return (x >= mid - ridgeHalf && x <= mid + ridgeHalf) ? ridgeTop
                                                              : baseTop;
    };
    p->root = buildTestOctree(*p, CHUNK_N, 0, 0, 0, topAt);

    std::vector<std::unique_ptr<ChunkPool>> pools(kChunkCount);
    pools[size_t(ci)] = std::move(p);

    ChunkStore s;
    const int n = latN();
    // colTop is the landscape truth; -1 marks "no terrain record", which is
    // what keeps the void outside the patch from reading as editable.
    std::vector<int16_t> colTop(size_t(n) * size_t(n), int16_t(-1));
    std::vector<uint8_t> colMat(size_t(n) * size_t(n), 3);
    for (int z = lo; z < hi; ++z)
        for (int x = lo; x < hi; ++x) {
            const int top = (x >= mid - ridgeHalf && x <= mid + ridgeHalf)
                                ? ridgeTop
                                : baseTop;
            // m_colTop is indexed by GLOBAL lattice coords, not chunk-local:
            // ChunkStore::findColumn looks the adopted top up at the sample's
            // own (x,z). Writing chunk-local indices here leaves every real
            // column at -1 ("no terrain record") and the brush finds nothing.
            colTop[size_t(gz0 + z) * size_t(n) + size_t(gx0 + x)] =
                int16_t(baseY + top);
        }
    s.adopt(pools, colTop, colMat);
    return s;
}

// A staircase RAMP: the top steps down one cell every `run` columns along x,
// over a square patch. This is the shape a voxel slope always has, and the one
// a surfel normal has to describe. Taller than makeRidgeStore's 8 cells so the
// ramp has room to run.
ChunkStore makeRampStore(int ci, int halfCells, int run, int topBase = 20)
{
    int ccx, ccy, ccz;
    chunkCoordsOf(ci, ccx, ccy, ccz);
    const int baseY = ccy * CHUNK_N;
    const int gx0 = ccx * CHUNK_N, gz0 = ccz * CHUNK_N;
    const int lo = std::max(0, CHUNK_N / 2 - halfCells);
    const int hi = std::min(CHUNK_N, CHUNK_N / 2 + halfCells);
    auto p = std::make_unique<ChunkPool>();
    auto topAt = [&](int x, int z) {
        if (x < lo || x >= hi || z < lo || z >= hi)
            return -1;
        return std::max(1, topBase - (x - lo) / std::max(1, run));
    };
    p->root = buildTestOctree(*p, CHUNK_N, 0, 0, 0, topAt);

    std::vector<std::unique_ptr<ChunkPool>> pools(kChunkCount);
    pools[size_t(ci)] = std::move(p);

    ChunkStore s;
    const int n = latN();
    std::vector<int16_t> colTop(size_t(n) * size_t(n), int16_t(-1));
    std::vector<uint8_t> colMat(size_t(n) * size_t(n), 3);
    for (int z = lo; z < hi; ++z)
        for (int x = lo; x < hi; ++x)
            colTop[size_t(gz0 + z) * size_t(n) + size_t(gx0 + x)] =
                int16_t(baseY + topAt(x, z));
    s.adopt(pools, colTop, colMat);
    return s;
}

TEST_CASE("surfelize: a staircase ramp's normals need the store heightfield blend")
{
    // The BAKE blends every terrain-top normal 0.55 toward a two-scale gradient
    // of a bilinearly interpolated FLOAT top field. The store path had no such
    // blend, so a live edit shaded terrain from exposed faces alone - which is
    // axis-quantised. This measures the gap on the shape that exposes it: a
    // 1-in-4 staircase ramp, where the ideal normal is 14.0 deg off vertical.
    //
    // A/B on ONE fixture, with the blend off and on, so the number is the
    // blend's doing and nothing else.
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makeRampStore(ci, /*halfCells=*/12, /*run=*/4);
    const int gx0 = 2 * CHUNK_N, gy0 = 1 * CHUNK_N, gz0 = 3 * CHUNK_N;

    // The ramp descends along +x, so the true surface normal is (0.25, 1, 0)
    // normalised: one cell down every four across.
    const glm::vec3 ideal = glm::normalize(glm::vec3(0.25f, 1.0f, 0.0f));

    // The fixture must really step: a solid-to-the-ceiling bug in the octree
    // builder once made this a flat plane, and the normal measurement then
    // silently measured the wrong thing.
    auto topOf = [&](int lx) {
        for (int y = CHUNK_N - 1; y >= 0; --y)
            if (store.cellAt(gx0 + lx, gy0 + y, gz0 + 20).solid)
                return y;
        return -1;
    };
    CHECK(topOf(20) == 20);
    CHECK(topOf(24) == 19);
    CHECK(topOf(40) == 15);

    auto meanError = [&](bool heightfieldNormals, float blend = 0.55f) {
        SurfelParams sp;
        sp.terrainHeightfieldNormals = heightfieldNormals;
        sp.heightfieldBlend = blend;
        const SurfelRange rg = buildChunkSurfelsRange(
            store, ci, glm::ivec3(gx0, gy0, gz0),
            glm::ivec3(gx0 + CHUNK_N, gy0 + CHUNK_N, gz0 + CHUNK_N), sp);
        double sum = 0.0;
        double worst = 0.0;
        int n = 0;
        for (const Surfel& f : rg.surfels) {
            const glm::vec3 nrm = glm::vec3(f.normal_rV);
            if (glm::dot(nrm, nrm) < 1e-6f)
                continue;
            // Only terrain tops: the ramp's exposed +Y faces.
            if (nrm.y < 0.5f)
                continue;
            const float ang = glm::degrees(
                std::acos(std::clamp(glm::dot(glm::normalize(nrm), ideal), -1.0f, 1.0f)));
            sum += ang;
            worst = std::max(worst, double(ang));
            ++n;
        }
        return std::tuple<double, double, int>(n > 0 ? sum / n : -1.0, worst, n);
    };

    // Fixture sanity: print the actual column tops the ramp built, so a wrong
    // fixture cannot be mistaken for a surfel-normal result.
    const auto [meanOff, worstOff, nOff] = meanError(false);
    const auto [meanOn, worstOn, nOn] = meanError(true);
    // Weight sweep: how much of the normal SHOULD come from the heightfield?
    // 0.55 is the bake's look-tuning; 1.0 trusts the gradient outright. This
    // measures whether the stage is merely mistuned or structurally diluted,
    // because the neighbour pass rebuilds n from its own blended value plus the
    // neighbours' UNBLENDED rawN (parity with the bake, which smooths
    // rawNormals[]), so a 0.55 blend survives at roughly 1 term in 5.
    const auto [meanFull, worstFull, nFull] = meanError(true, 1.0f);
    MESSAGE("ramp normals (ideal " << glm::degrees(std::acos(glm::dot(ideal, glm::vec3(0,1,0))))
            << " deg off vertical): blend OFF mean " << meanOff << ", worst " << worstOff
            << " (" << nOff << "); 0.55 mean " << meanOn << ", worst " << worstOn
            << " (" << nOn << "); 1.00 mean " << meanFull << ", worst " << worstFull
            << " (" << nFull << ")");

    REQUIRE(nOff > 0);
    REQUIRE(nOn > 0);
    // The stage must measurably improve agreement with the true slope, and it
    // must not invent surfels or drop any.
    CHECK(meanOn < meanOff);
    CHECK(nOn == nOff);
    // And the sweep must be monotone: the heightfield normal is a better
    // description of the slope than the face normal, at any weight > 0.
    CHECK(meanFull < meanOn);
}

TEST_CASE("chunk store: a WIDE smooth brush collapses a WIDE ridge")
{
    // Regression for the brush that did not scale: the smoothing average was a
    // fixed 3x3 box, so the brush radius only set the footprint mask and the
    // falloff - a 1 m brush averaged exactly what a 1-voxel brush averaged and
    // could not move a ridge wider than its own neighbourhood. Measured on this
    // fixture before the fix: "top=71 avg=71.000" - the crest's whole 3x3 ring
    // sits at its own height, lround returns the current cell, and the batch
    // contains NO edit at the crest. Every other smooth test in this file used
    // VOXEL*1.5f (a 1.5-voxel radius), which is why it never showed up.
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makeRidgeStore(ci, /*halfCells=*/16, /*ridgeHalf=*/3);
    const int gx = 2 * CHUNK_N + CHUNK_N / 2; // chunk (2,1,3), patch centre
    const int gz = 3 * CHUNK_N + CHUNK_N / 2;
    const int gy = 1 * CHUNK_N;

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ gx, gy + 7, gz }, 1.0f, 1.0f);
    REQUIRE(!batch.edits.empty());

    // The crest must be CUT, not merely feathered at the flanks.
    int crestClears = 0;
    for (const StoreEdit& e : batch.edits)
        if (e.x >= gx - 3 && e.x <= gx + 3 && e.z == gz &&
            e.mode == StoreEdit::Mode::Clear)
            ++crestClears;
    CHECK(crestClears > 0);

    store.apply(batch.edits);
    store.rebuildDirty();
    CHECK_FALSE(store.cellAt(gx, gy + 7, gz).solid);
    // ...and the surrounding flat base must not sink into the void with it.
    CHECK(store.cellAt(gx - 12, gy + 4, gz).solid);
}

TEST_CASE("chunk store: the smooth kernel stops growing past the sigma cap")
{
    // The cap is the contract that keeps a wide brush interactive: past it the
    // kernel is pinned, so a 3.2 m brush and a 6 m brush relax the CENTRE
    // column identically and differ only in footprint. Without the cap the
    // kernel cost grows as radius^2 * sigma^2 and one 6 m stamp becomes tens
    // of millions of multiply-adds.
    const int ci = chunkIndexOf(2, 1, 3);
    const int gx = 2 * CHUNK_N + CHUNK_N / 2; // chunk (2,1,3), patch centre
    const int gz = 3 * CHUNK_N + CHUNK_N / 2;
    const int gy = 1 * CHUNK_N;

    auto centreClears = [&](float radiusM) {
        // halfCells 48 -> a 96-cell patch, wide enough for a 3.2 m brush
        // (65-cell kernel support) to keep full coverage.
        ChunkStore store =
            makeRidgeStore(ci, /*halfCells=*/48, /*ridgeHalf=*/3);
        const SmoothTerrainEdits batch =
            store.makeSmoothEdits({ gx, gy + 7, gz }, radiusM, 1.0f);
        int cleared = 0;
        for (const StoreEdit& e : batch.edits)
            if (e.x == gx && e.z == gz && e.mode == StoreEdit::Mode::Clear)
                ++cleared;
        return cleared;
    };

    const int atCap = centreClears(3.2f);
    const int pastCap = centreClears(6.0f);
    CHECK(atCap > 0);
    CHECK(pastCap == atCap);
}

TEST_CASE("chunk store: smooth leaves a column whose kernel window is mostly void")
{
    // The obstacle guard. With the OLD renormalisation the average was taken
    // over whatever samples survived and divided by THEIR weight, so a column
    // beside a hole was averaged over its one surviving side at full strength
    // and snapped toward it - terrain visibly eroding away from any void or
    // building. The fix divides by the kernel's CONSTANT weight (an obstacle
    // acts as a mirror, contributing nothing) and skips a column whose window
    // is not mostly terrain at all.
    // A 6-cell strip under a 1 m brush (21-cell kernel support) covers ~8% of
    // the window, far below kSmoothMinCoverage, so nothing may move. The old
    // code moved every column in it.
    const int ci = chunkIndexOf(2, 1, 3);
    ChunkStore store = makeRidgeStore(ci, /*halfCells=*/3, /*ridgeHalf=*/0);
    const int gx = 2 * CHUNK_N + CHUNK_N / 2;
    const int gz = 3 * CHUNK_N + CHUNK_N / 2;
    const int gy = 1 * CHUNK_N;

    const SmoothTerrainEdits batch =
        store.makeSmoothEdits({ gx, gy + 7, gz }, 1.0f, 1.0f);
    CHECK(batch.edits.empty());

    // And the geometry is genuinely untouched.
    CHECK(store.cellAt(gx, gy + 7, gz).solid);
    CHECK(store.cellAt(gx - 2, gy + 4, gz).solid);
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
    // The crest is cut. How MANY cells it loses in one stamp is deliberately
    // NOT pinned: this fixture's neighbour set contains an object column, which
    // is an invalid sample, so coverage is 0.940 and the constant-normaliser
    // rule pulls the target back toward the current height by design
    // (measured: dev -2.335 -> relaxed 4.665, which rounds UP). The old 3x3
    // kernel gave 4.46 and rounded down. That 0.2-cell difference straddling a
    // .5 boundary is an accident of the fixture, not a contract.
    CHECK_FALSE(store.cellAt(gx, gy + 7, gz).solid);
    CHECK(store.cellAt(gx, gy + 4, gz).solid);
    // The invariant that does matter: the spike converges away rather than
    // stalling one cell short of flat.
    store.apply(
        store.makeSmoothEdits({ gx, gy + 7, gz }, VOXEL * 1.5f, 1.0f).edits);
    store.rebuildDirty();
    CHECK_FALSE(store.cellAt(gx, gy + 5, gz).solid);
    CHECK_FALSE(store.cellAt(gx, gy + 5, gz).obj);
    CHECK(store.cellAt(gx, gy + 4, gz).solid);

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

TEST_CASE("chunk store: live editor keeps hard-edge bridges after the base run")
{
    ChunkStore store = makeSyntheticStore(0, true);
    LiveEditor editor;
    editor.attach(&store);
    SurfelParams params;
    params.edgeShrink = 0.35f;
    params.edgeFill = true;
    params.anisotropy = false;

    editor.seedFromStore(0, params);
    const uint32_t parents = uint32_t(editor.chunkSurfels(0, params).size());
    const uint32_t edges = editor.edgeCountOf(0);
    const std::vector<Surfel>& run = editor.chunkRun(0, params);
    REQUIRE(parents > 0);
    CHECK(edges > 0);
    CHECK(run.size() == parents + edges);
    for (uint32_t i = parents; i < run.size(); ++i) {
        CHECK(run[i].pos_rU.w > 0.0f);
        CHECK(run[i].normal_rV.w > 0.0f);
        CHECK(glm::length(glm::vec3(run[i].tan_aspect)) > 0.99f);
    }

    // A full region refresh removes and rebuilds both segments together; it
    // must not duplicate bridges.
    CHECK(editor.refreshRegion(0, glm::ivec3(0), glm::ivec3(CHUNK_N), params));
    CHECK(editor.chunkSurfels(0, params).size() == parents);
    CHECK(editor.edgeCountOf(0) == edges);
    CHECK(editor.chunkRun(0, params).size() == parents + edges);
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

TEST_CASE("chunk store: range bake is deterministic and well-formed")
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
    // deterministic: the same range always bakes the same run
    const SurfelRange rg2 = buildChunkSurfelsRange(store, ci, lo, hi, sp);
    CHECK(rg2.surfels == rg.surfels);
    for (const Surfel& s : rg.surfels) {
        CHECK(std::isfinite(s.pos_rU.x));
        CHECK(std::isfinite(s.pos_rU.y));
        CHECK(std::isfinite(s.pos_rU.z));
        CHECK(s.pos_rU.w > 0.0f);
        CHECK(s.normal_rV.w > 0.0f);
        const float nl = glm::length(glm::vec3(s.normal_rV));
        CHECK(nl > 0.99f);
        CHECK(nl < 1.01f);
    }
    // every baked surfel sits near a parent cell, never detached
    for (size_t i = 0; i < std::min<size_t>(rg.surfels.size(), 64); ++i) {
        float best = 1e9f;
        for (size_t j = 0; j < rg.keys.size(); ++j) {
            int gx2, gy2, gz2;
            surfelUnpackKey(rg.keys[j], gx2, gy2, gz2);
            const glm::vec3 cc(-51.2f + (gx2 + 0.5f) * VOXEL,
                               -51.2f + (gy2 + 0.5f) * VOXEL,
                               -51.2f + (gz2 + 0.5f) * VOXEL);
            best = std::min(best, glm::length(glm::vec3(rg.surfels[i].pos_rU) - cc));
        }
        CHECK(best < 0.5f);
    }
}

TEST_CASE("chunk store: live editor patched run carries the stamp to the chunk")
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

    // the pre-stamp run: chunkRun seeds from the store on first touch
    const size_t preN = ed.chunkRun(ci, sp).size();
    REQUIRE(preN > 0);

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

    // the stamp reached the GPU-side chunk run: non-empty, changed by the
    // stamp, and sealed (base parents + edge bridges regenerate together)
    const std::vector<Surfel>& run = ed.chunkRun(ci, sp);
    CHECK(!run.empty());
    CHECK(run.size() != preN);
    CHECK(run.size() ==
          ed.chunkSurfels(ci, sp).size() + ed.edgeCountOf(ci));
    const std::vector<Surfel>* runPtr = &ed.chunkRun(ci, sp);
    CHECK(runPtr == &run); // cached, no spurious rebuild

    // a second stamp re-marks the run dirty and rebuilds a sealed run
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
    CHECK(!run2.empty());
    CHECK(run2.size() ==
          ed.chunkSurfels(ci, sp).size() + ed.edgeCountOf(ci));
}

// --- re-derivation parity: a live edit must not re-aim untouched splats -----
namespace {

const int kFaceDir[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                             { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

// Lattice cell of a surfel, recovered the way LiveEditor::keyFromSurfel does.
uint64_t surfelCell(const Surfel& s)
{
    glm::vec3 n = glm::vec3(s.normal_rV);
    const float l2 = glm::dot(n, n);
    n = (l2 > 1e-12f) ? n / std::sqrt(l2) : glm::vec3(0.f, 1.f, 0.f);
    const glm::vec3 c = glm::vec3(s.pos_rU) - n * (0.5f * VOXEL);
    return (uint64_t(uint32_t(std::lround((c.x + 0.5f * WORLD) / VOXEL - 0.5f)))
            << 20) |
           (uint64_t(uint32_t(std::lround((c.y + 0.5f * WORLD) / VOXEL - 0.5f)))
            << 10) |
           uint64_t(uint32_t(std::lround((c.z + 0.5f * WORLD) / VOXEL - 0.5f)));
}

glm::vec3 surfelUnitNormal(const Surfel& s)
{
    const glm::vec3 n = glm::vec3(s.normal_rV);
    const float l2 = glm::dot(n, n);
    return l2 > 1e-12f ? n / std::sqrt(l2) : glm::vec3(0.f);
}

bool nearlyUp(const glm::vec3& n) { return n.y > 0.9999f; }

// Bits of exposed lattice faces in the store: air is `sdfRaw > 0`, and
// out-of-lattice counts as air, exactly as the surfelizer's own rule does.
unsigned storeExposedFaces(const ChunkStore& s, int x, int y, int z)
{
    const int n = s.latN();
    unsigned m = 0;
    for (int d = 0; d < 6; ++d) {
        const int nx = x + kFaceDir[d][0], ny = y + kFaceDir[d][1],
                  nz = z + kFaceDir[d][2];
        if (nx < 0 || nx >= n || ny < 0 || ny >= n || nz < 0 || nz >= n ||
            s.cellAt(nx, ny, nz).sdfRaw > 0)
            m |= 1u << d;
    }
    return m;
}

int exposedCount(unsigned m)
{
    int c = 0;
    for (int d = 0; d < 6; ++d)
        if (m & (1u << d))
            ++c;
    return c;
}

// True when no cell in the 3x3x3 neighbourhood carries the byte value 0.
//
// The brick SDF is int8 voxel units written by truncation, and `raw == 0`
// counts as SOLID because the SVO DDA tests `sdf <= 0`. So a cell whose true
// distance is a small positive number is stored as 0 and reads as solid here
// while VoxelField calls it air. That is a one-sided quantisation floor of the
// storage format (measured: 0.77 % of cabin cells), it predates live editing,
// and it is not what this guard is about - so the angular comparison below
// skips those cells and measures the normal RULE on its own.
bool storeNeighbourhoodIsClean(const ChunkStore& s, int x, int y, int z)
{
    const int n = s.latN();
    for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy, nz = z + dz;
                if (nx < 0 || nx >= n || ny < 0 || ny >= n || nz < 0 || nz >= n)
                    continue;
                if (s.cellAt(nx, ny, nz).sdfRaw == 0)
                    return false;
            }
    return true;
}

// An object cell on a flat vertical wall face: exactly one exposed face, and
// that face points sideways. This is the class the report is about - a wall
// disk that used to come out of a live refresh pointing straight up.
bool findFlatVerticalWallCell(const ChunkStore& s, int cx, int cy, int cz,
                              glm::ivec3& out)
{
    for (int lz = 0; lz < CHUNK_N; ++lz)
        for (int ly = 0; ly < CHUNK_N; ++ly)
            for (int lx = 0; lx < CHUNK_N; ++lx) {
                const int x = cx * CHUNK_N + lx, y = cy * CHUNK_N + ly,
                          z = cz * CHUNK_N + lz;
                const StoreCell c = s.cellAt(x, y, z);
                if (c.sdfRaw > 0 || !c.obj)
                    continue;
                const unsigned m = storeExposedFaces(s, x, y, z);
                if (m == 0 || (m & (m - 1)) != 0)
                    continue;
                if ((m & 0x30u) == 0)
                    continue; // +-X or +-Z, not a floor / ceiling face
                out = glm::ivec3(x, y, z);
                return true;
            }
    return false;
}

} // namespace

TEST_CASE("chunk store: live surfel normals follow the bake, never a fabricated up")
{
    LayeredWorld& lw = testLayeredWorld();
    // The suite shares one world and earlier cases stamp into it. Re-adopt the
    // baked pools so this case starts from the pristine world, and drop the
    // store again on the way out so the next case is not handed this case's
    // edits.
    lw.invalidateStore();
    const VoxelField& field = lw.field();
    ChunkStore& store = lw.store();

    SurfelParams sp;
    sp.edgeShrink = 0.35f;
    sp.edgeFill = true;
    sp.anisotropy = true;
    sp.lodRings = false;

    // An OBJECT chunk. A terrain-only chunk cannot guard this: the divergence
    // needs a thick object body, whose byte-quantised store SDF cancels to
    // exactly zero deep inside - which is what used to hand those cells a
    // fabricated straight-up normal while a wall's own disks were re-aimed.
    int wallChunk = -1;
    glm::ivec3 wall;
    for (int cz = 0; cz < kChunkGridN && wallChunk < 0; ++cz)
        for (int cy = 0; cy < kChunkGridN && wallChunk < 0; ++cy)
            for (int cx = 0; cx < kChunkGridN; ++cx) {
                if (!findFlatVerticalWallCell(store, cx, cy, cz, wall))
                    continue;
                wallChunk = chunkIndexOf(cx, cy, cz);
                break;
            }
    REQUIRE(wallChunk >= 0);
    REQUIRE(store.cellAt(wall.x, wall.y, wall.z).obj);
    // Pin the class: the guard cell really is a flat vertical wall face.
    REQUIRE(exposedCount(storeExposedFaces(store, wall.x, wall.y, wall.z)) == 1);
    REQUIRE((storeExposedFaces(store, wall.x, wall.y, wall.z) & 0x30u) != 0);

    const int cx = wallChunk % kChunkGridN;
    const int cy = (wallChunk / kChunkGridN) % kChunkGridN;
    const int cz = wallChunk / (kChunkGridN * kChunkGridN);
    const glm::ivec3 clo(cx * CHUNK_N, cy * CHUNK_N, cz * CHUNK_N);

    // 1) Parity. A live stamp re-derives the edit AABB plus a margin, so any
    //    rule difference between the bake and the store path is applied to
    //    cells the user never touched. Same cell, same direction - or the edit
    //    moves splats across the world.
    const SurfelSet baked = buildSurfels(field, sp);
    std::map<uint64_t, int> bakeCount;
    std::map<uint64_t, glm::vec3> bakeNormal;
    for (const Surfel& sf : baked.surfels) {
        if (!surfelIsObject(sf.mat_ao.w))
            continue;
        const uint64_t k = surfelCell(sf);
        ++bakeCount[k];
        bakeNormal.emplace(k, surfelUnitNormal(sf));
    }
    const SurfelRange full = buildChunkSurfelsRange(
        store, wallChunk, clo, clo + glm::ivec3(CHUNK_N), sp);
    REQUIRE(!full.surfels.empty());

    int compared = 0, grossOff = 0, zeroNormal = 0, fabricatedUp = 0;
    double angleSum = 0;
    for (const Surfel& sf : full.surfels) {
        if (!surfelIsObject(sf.mat_ao.w))
            continue;
        const uint64_t k = surfelCell(sf);
        const int x = int((k >> 20) & 0x3FFu);
        const int y = int((k >> 10) & 0x3FFu);
        const int z = int(k & 0x3FFu);
        REQUIRE(exposedCount(storeExposedFaces(store, x, y, z)) > 0);
        const glm::vec3 n = surfelUnitNormal(sf);
        if (glm::dot(n, n) < 0.5f) {
            ++zeroNormal; // an unnormalisable normal would reach the GPU
            continue;
        }
        const auto bit = bakeNormal.find(k);
        if (bit == bakeNormal.end())
            continue;
        // The reported symptom, as a number: a disk that came out of the live
        // path pointing straight up where the bake has it elsewhere. That is
        // what a cancelled SDF gradient used to fabricate.
        if (nearlyUp(n) && !nearlyUp(bit->second))
            ++fabricatedUp;
        // Only cells the bake emits ONCE enter the angular comparison: a thin
        // plate is legitimately several axis-aligned entries on both sides.
        // Cells on the SDF byte's 0 boundary are skipped for the reason above.
        if (bakeCount[k] != 1 || !storeNeighbourhoodIsClean(store, x, y, z))
            continue;
        ++compared;
        const float ang = std::acos(
                              std::clamp(glm::dot(n, bit->second), -1.f, 1.f)) *
                          57.2958f;
        angleSum += ang;
        if (ang > 30.f)
            ++grossOff;
    }
    CHECK(zeroNormal == 0);
    CHECK(compared > 0);
    CHECK(fabricatedUp == 0);
    CHECK(grossOff == 0);
    CHECK(angleSum / double(compared) < 5.0);

    // 2) The live half. The stamp goes 2 cells out along the wall's exposed
    //    face: far enough that the wall cell keeps its own exposure (a cell
    //    placed right against it would legitimately remove that face), but
    //    still inside the +-3 refresh margin, so the wall IS re-derived.
    const unsigned wallMask = storeExposedFaces(store, wall.x, wall.y, wall.z);
    int face = 0;
    while (!(wallMask & (1u << face)))
        ++face;
    const glm::ivec3 edit(wall.x + 2 * kFaceDir[face][0],
                          wall.y + 2 * kFaceDir[face][1],
                          wall.z + 2 * kFaceDir[face][2]);
    REQUIRE(store.cellAt(edit.x, edit.y, edit.z).sdfRaw > 0); // really air

    const uint64_t wallKey = (uint64_t(uint32_t(wall.x)) << 20) |
                             (uint64_t(uint32_t(wall.y)) << 10) |
                             uint64_t(uint32_t(wall.z));
    const uint64_t editKey = (uint64_t(uint32_t(edit.x)) << 20) |
                             (uint64_t(uint32_t(edit.y)) << 10) |
                             uint64_t(uint32_t(edit.z));

    LiveEditor ed;
    ed.attach(&store);
    ed.seedFromStore(wallChunk, sp);
    glm::vec3 wallBefore(0.f);
    bool editSurfelledBefore = false;
    for (const Surfel& sf : ed.chunkSurfels(wallChunk, sp)) {
        const uint64_t k = surfelCell(sf);
        if (k == wallKey)
            wallBefore = surfelUnitNormal(sf);
        if (k == editKey)
            editSurfelledBefore = true;
    }
    REQUIRE(glm::dot(wallBefore, wallBefore) > 0.5f);
    REQUIRE(!editSurfelledBefore);

    StoreEdit add;
    add.mode = StoreEdit::Mode::Set;
    add.x = edit.x;
    add.y = edit.y;
    add.z = edit.z;
    add.mat = 6;
    const std::vector<int> changed = ed.stamp({ add }, sp);
    // Assert the instrument before the outcome: the stamp must have landed in
    // the wall's chunk, or "nothing moved" would prove nothing.
    REQUIRE(std::find(changed.begin(), changed.end(), wallChunk) !=
            changed.end());
    REQUIRE(store.cellAt(edit.x, edit.y, edit.z).solid);

    // The region really was re-derived: the new cell is now part of the run.
    bool editSurfelledAfter = false;
    glm::vec3 wallAfter(0.f);
    for (const Surfel& sf : ed.chunkSurfels(wallChunk, sp)) {
        const uint64_t k = surfelCell(sf);
        if (k == wallKey)
            wallAfter = surfelUnitNormal(sf);
        if (k == editKey)
            editSurfelledAfter = true;
    }
    CHECK(editSurfelledAfter);
    REQUIRE(glm::dot(wallAfter, wallAfter) > 0.5f);
    // Same cell, same direction, bit for bit. The wall sits inside the refresh
    // margin, so this is a re-derivation that has to be a no-op - not a cache
    // that skipped the cell. The reported bug: an edit near a wall re-aimed
    // the wall's disks and turned them all upwards.
    CHECK(glm::dot(wallAfter, wallBefore) > 0.9999f);

    lw.invalidateStore(); // leave the shared world as we found it
}

// UN-SKIPPED at the UBO-256 flip: bake and trigger both enumerate the
// store now, so the flip contract is production-prefix coverage - every
// field light (the old baked set) still has a store light within the thin
// distance at the same intensity inside the production prefix (budget -
// authored). The store's extras are submerged lava the pools already render
// as lava (provenance: bricks-only recount also yields 169, exonerating the
// box-face enumeration). The knee MESSAGE is the early warning: content
// drift that pushes it past the production room re-fires this test instead
// of silently darkening hearths.
TEST_CASE("chunk store: collectEmissive covers the field variant")
{
    LayeredWorld& lw = testLayeredWorld();
    REQUIRE(lw.loaded());
    // palette emission table, no texture overrides in this content set
    std::vector<glm::vec3> emission(kPaletteN, glm::vec3(0.0f));
    for (int m = 0; m < kPaletteN; ++m)
        emission[size_t(m)] = kEmissive[size_t(m)];
    std::vector<VoxelField::EmissiveCluster> fromField, fromStore;
    lw.field().collectEmissive(emission, fromField);
    lw.store().collectEmissive(emission, fromStore,
                               std::numeric_limits<int>::max());
    MESSAGE("emissive clusters: field ", fromField.size(), " store ",
            fromStore.size());
    // The store reads pool bricks/boxes, which carry the bake's propagated
    // interior mats; the field reads records + column tops only. So the
    // store is a SUPERSET on baked content (measured: 19 field vs 169
    // store on the hamlet). The flip contract is PREFIX coverage: every
    // field light has a store light within the thin distance at the same
    // intensity inside the production prefix (budget - authored), so the
    // budget-truncated flip set loses no baked light.
    REQUIRE(fromStore.size() >= fromField.size());
    // Known field ghosts (probe-verified 2026-10-08, all in empty air):
    //   (20.08,5.23,19.46) d=+4.25, (21.56,5.17,18.05) d=+4.05,
    //   (20.05,4.48,19.62) d=+3.45 (mat=1, empty).
    // Buried lava pockets whose bucket centroid lifted 3-4 m through solid
    // to the surface: lights with no visible emitter. The store clusters
    // the buried cells in place (occluded, correctly dark) instead, so the
    // flip REMOVES these three - a fix, not a regression. Excluded from
    // required coverage by exact position; any NEW field-only cluster still
    // fails loudly below. Follow-up (needs shared-tail owner): cap
    // liftToAir so buried buckets drop instead of surfacing.
    const std::vector<glm::vec3> kKnownGhosts{
        glm::vec3(20.0753f, 5.2251f, 19.4558f),
        glm::vec3(21.5581f, 5.17027f, 18.0518f),
        glm::vec3(20.047f, 4.47537f, 19.6172f),
    };
    auto isKnownGhost = [&](const glm::vec3& p) {
        for (const glm::vec3& g : kKnownGhosts)
            if (glm::distance(p, g) < 0.05f)
                return true;
        return false;
    };
    // Intensity match is relative (10%), not exact: a cluster's intensity
    // is its bucket's mean max-component, and the field/store buckets hold
    // slightly different cell sets, so exact equality evicts identical
    // lights over sub-percent means (measured: 3.192 vs 3.200 at the same
    // hearth). 10% is visually nil; the genuine divergences below differ
    // by 25%+ at 4 m and still fire.
    auto sameI = [](float a, float b) {
        return std::abs(a - b) <= 0.1f * std::max({ a, b, 1e-6f });
    };
    auto coveredAt = [&](size_t prefix, std::vector<size_t>* skipped) {
        for (size_t i = 0; i < fromField.size(); ++i) {
            if (isKnownGhost(fromField[i].pos)) {
                if (skipped)
                    skipped->push_back(i);
                continue;
            }
            bool matched = false;
            for (size_t j = 0; j < std::min(prefix, fromStore.size());
                 ++j) {
                if (glm::distance(fromStore[j].pos, fromField[i].pos) <
                        kEmissiveThinDist &&
                    sameI(fromStore[j].intensity, fromField[i].intensity)) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        return true;
    };
    // Production room, derived from the manifests, never hardcoded: budget
    // minus the world.json authored count (the courtyard lamps eat slots).
    std::vector<worldfile::LightSource> authoredProd;
    worldfile::loadLightManifest(std::string(VOXELFORGE_ASSET_DIR) +
                                     "/world.json",
                                 authoredProd);
    const size_t prodRoom =
        size_t(worldfile::kLightBudgetDefault) -
        std::min<size_t>(authoredProd.size(),
                         size_t(worldfile::kLightBudgetDefault));
    MESSAGE("authored ", authoredProd.size(), " production room ", prodRoom);
    // Eviction knee: sweep up from the field count so the log carries the
    // minimal survivable room, not just a pass/fail at one budget. A zero
    // knee means UNCOVERED even at the full list (never misread the
    // fallback size as a passing knee). Known ghosts are skipped in the
    // sweep but audited right after: any NEW field-only cluster fails.
    std::vector<size_t> skipped;
    size_t minRoom = 0;
    for (size_t room = fromField.size(); room <= fromStore.size(); ++room) {
        if (coveredAt(room, nullptr)) {
            minRoom = room;
            break;
        }
    }
    if (minRoom > 0)
        MESSAGE("hearths survive at room >= ", minRoom);
    else
        MESSAGE("hearths UNCOVERED even at full ", fromStore.size());
    CHECK(minRoom > 0);
    CHECK(minRoom <= prodRoom);
    CHECK(coveredAt(prodRoom, &skipped));
    for (size_t i : skipped)
        MESSAGE("ghost skip field #", i, " pos (", fromField[i].pos.x, ",",
                fromField[i].pos.y, ",", fromField[i].pos.z, ")");
    // The skip set must equal the known ghosts exactly: a new field-only
    // cluster is a regression hiding as a ghost, not drift to absorb.
    CHECK(skipped.size() == kKnownGhosts.size());
    // Diagnosis on RED: for every field cluster missed at the production
    // room, report the nearest store cluster (index, distance, intensity
    // pair) so the eviction reads as position-loss vs intensity-strictness.
    if (!coveredAt(prodRoom, nullptr)) {
        for (size_t i = 0; i < fromField.size(); ++i) {
            bool inProd = false;
            for (size_t j = 0; j < std::min(prodRoom, fromStore.size());
                 ++j) {
                if (glm::distance(fromStore[j].pos, fromField[i].pos) <
                        kEmissiveThinDist &&
                    sameI(fromStore[j].intensity,
                          fromField[i].intensity)) {
                    inProd = true;
                    break;
                }
            }
            if (inProd)
                continue;
            if (isKnownGhost(fromField[i].pos))
                continue; // audited above, not an eviction
            size_t best = 0;
            float bestD = 1e30f;
            for (size_t j = 0; j < fromStore.size(); ++j) {
                const float d =
                    glm::distance(fromStore[j].pos, fromField[i].pos);
                if (d < bestD) {
                    bestD = d;
                    best = j;
                }
            }
            MESSAGE("evicted field #", i, " pos (", fromField[i].pos.x,
                    ",", fromField[i].pos.y, ",", fromField[i].pos.z,
                    ") I=", fromField[i].intensity, " nearest store #",
                    best, " d=", bestD, " I=", fromStore[best].intensity);
        }
    }
}

TEST_CASE("chunk store: collectEmissive reflects live emissive stamps")
{
    // isolated synthetic store: stamping here cannot pollute the shared
    // testLayeredWorld other cases read
    ChunkStore store = makeSyntheticStore(0, true);
    std::vector<glm::vec3> emission(kPaletteN, glm::vec3(0.0f));
    for (int m = 0; m < kPaletteN; ++m)
        emission[size_t(m)] = kEmissive[size_t(m)];
    std::vector<VoxelField::EmissiveCluster> before;
    store.collectEmissive(emission, before, std::numeric_limits<int>::max());

    // find a solid surface cell in chunk 0 and lay a 2x2 ember bed on it
    int sx = -1, sy = -1, sz = -1;
    for (int y = 1; y < CHUNK_N - 1 && sx < 0; ++y)
        for (int z = 0; z < CHUNK_N && sx < 0; ++z)
            for (int x = 0; x < CHUNK_N - 1; ++x)
                if (store.cellAt(x, y, z).solid && !store.cellAt(x, y + 1, z).solid &&
                    !store.cellAt(x + 1, y, z).solid) {
                    sx = x;
                    sy = y + 1;
                    sz = z;
                    break;
                }
    REQUIRE(sx >= 0);
    std::vector<StoreEdit> edits;
    for (int dz = 0; dz < 2; ++dz)
        for (int dx = 0; dx < 2; ++dx) {
            StoreEdit e;
            e.mode = StoreEdit::Mode::Set;
            e.x = sx + dx;
            e.y = sy;
            e.z = sz + dz;
            e.mat = 9; // lava: kEmissive max component 3.0
            edits.push_back(e);
        }
    store.apply(edits);
    store.rebuildDirty();

    std::vector<VoxelField::EmissiveCluster> after;
    store.collectEmissive(emission, after, std::numeric_limits<int>::max());
    // the synthetic content has no emitters, so the bed is exactly one new
    // cluster (all four cells fall in one 1 m bucket, far from anything else)
    REQUIRE(before.empty());
    REQUIRE(after.size() == 1);
    // bed-cell centres averaged: (sx+1, sy+0.5, sz+1) in grid units, then
    // to world (the cluster lifts ~0.1 m off the bed into air on top)
    const glm::vec3 bedW(-0.5f * WORLD + (float(sx) + 1.0f) * VOXEL,
                         -0.5f * WORLD + (float(sy) + 0.5f) * VOXEL,
                         -0.5f * WORLD + (float(sz) + 1.0f) * VOXEL);
    CHECK(glm::distance(after[0].pos, bedW) < 1.5f);
    CHECK(after[0].intensity == doctest::Approx(3.0f));
    CHECK(after[0].radius >= 1.0f);
    // radius cap raised 4.0 -> 8.0 for Victor's base+intensity formula
    // (white-hot hits ~4.6); the 2x2 bed itself is ~1.1 either way.
    CHECK(after[0].radius <= 8.0f);

    // budget: capped output, and zero budget means empty
    std::vector<VoxelField::EmissiveCluster> capped, none;
    store.collectEmissive(emission, capped, 1);
    CHECK(capped.size() == 1);
    store.collectEmissive(emission, none, 0);
    CHECK(none.empty());

    // region == all chunks is bit-identical to the full enumeration (same
    // cells in the same order through the same shared tail)
    std::vector<int> all;
    for (int ci = 0; ci < kChunkCount; ++ci)
        all.push_back(ci);
    std::vector<VoxelField::EmissiveCluster> full, regional;
    store.collectEmissive(emission, full, std::numeric_limits<int>::max());
    store.collectEmissiveRegion(emission, regional,
                                std::numeric_limits<int>::max(), all);
    REQUIRE(full.size() == regional.size());
    for (size_t i = 0; i < full.size(); ++i) {
        CHECK(regional[i].pos.x == doctest::Approx(full[i].pos.x));
        CHECK(regional[i].pos.y == doctest::Approx(full[i].pos.y));
        CHECK(regional[i].pos.z == doctest::Approx(full[i].pos.z));
        CHECK(regional[i].intensity == doctest::Approx(full[i].intensity));
    }
    // region subset: only the stamped chunk's clusters come back
    std::vector<VoxelField::EmissiveCluster> sub;
    store.collectEmissiveRegion(emission, sub, std::numeric_limits<int>::max(),
                                { 0 });
    CHECK(sub.size() == full.size());
}
