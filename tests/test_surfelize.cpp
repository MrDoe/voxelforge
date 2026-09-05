// Surfelize tests: extraction correctness, determinism, chunk bucketing.
#include <doctest/doctest.h>
#include "voxel/surfelize.hpp"
#include "voxel/layered_world.hpp"
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

using namespace vf::voxel;

namespace {
// Content tests use the checked-in all-enabled manifest (world_all.json),
// same world the visual_check shots render.
LayeredWorld& allLayersWorld()
{
    static LayeredWorld lw;
    static bool ok = [] {
        return lw.load(std::string(VOXELFORGE_ASSET_DIR) + "/world_all.json");
    }();
    (void)ok;
    return lw;
}

constexpr int kLatN = 1024;

// A VoxelField holding a single solid 2x2x2 object block at lattice
// (512,512,512), no terrain. All 8 cells are surface cells.
VoxelField cubeField()
{
    std::vector<uint32_t> cells;
    std::vector<uint8_t> mats;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const uint32_t x = 512 + dx, y = 512 + dy, z = 512 + dz;
                cells.push_back((x << 20) | (y << 10) | z);
                mats.push_back(6);
            }
    VoxelField f;
    std::vector<VoxelRecord> recs;
    std::vector<int16_t> colTop(kLatN * kLatN, -1);
    std::vector<uint8_t> colMat(kLatN * kLatN, 0);
    f.build(recs, colTop, colMat, cells, mats);
    return f;
}

// A flat 5x5 terrain plateau at lattice height 600, no objects.
VoxelField plateauField()
{
    std::vector<int16_t> colTop(kLatN * kLatN, -1);
    std::vector<uint8_t> colMat(kLatN * kLatN, 0);
    for (int dz = 0; dz < 5; ++dz)
        for (int dx = 0; dx < 5; ++dx) {
            colTop[size_t(510 + dz) * kLatN + (510 + dx)] = 600;
            colMat[size_t(510 + dz) * kLatN + (510 + dx)] = 0;
        }
    VoxelField f;
    std::vector<VoxelRecord> recs;
    f.build(recs, colTop, colMat, {}, {});
    return f;
}

// A terrain step: 5x5 patch at height 600 abutting a 5x5 patch at 596.
VoxelField stepField()
{
    std::vector<int16_t> colTop(kLatN * kLatN, -1);
    std::vector<uint8_t> colMat(kLatN * kLatN, 0);
    for (int dz = 0; dz < 5; ++dz)
        for (int dx = 0; dx < 5; ++dx) {
            colTop[size_t(510 + dz) * kLatN + (510 + dx)] = 600;
            colMat[size_t(510 + dz) * kLatN + (510 + dx)] = 0;
            colTop[size_t(510 + dz) * kLatN + (515 + dx)] = 596;
            colMat[size_t(510 + dz) * kLatN + (515 + dx)] = 0;
        }
    VoxelField f;
    std::vector<VoxelRecord> recs;
    f.build(recs, colTop, colMat, {}, {});
    return f;
}
} // namespace

TEST_CASE("surfelize cube: 8 surfels with unit normals")
{
    const VoxelField f = cubeField();
    REQUIRE(f.valid());

    SurfelParams p;
    p.smoothNormals = false;
    p.terrainHeightfieldNormals = false;
    const SurfelSet set = buildSurfels(f, p);

    // every cell of a 2x2x2 block touches air -> all 8 emit surfels
    CHECK(set.surfels.size() == 8u);
    for (const auto& s : set.surfels) {
        const glm::vec3 n(s.normal_rV);
        const float len = glm::length(n);
        CHECK(len > 0.99f);
        CHECK(len < 1.01f);
        CHECK(s.pos_rU.w > 0.0f);
        CHECK(s.normal_rV.w > 0.0f);
    }
}

TEST_CASE("surfelize flat plateau: top normals ~+Y")
{
    const VoxelField f = plateauField();
    REQUIRE(f.valid());

    SurfelParams p;
    p.smoothNormals = false;
    p.terrainHeightfieldNormals = false;
    const SurfelSet set = buildSurfels(f, p);

    int upCount = 0;
    for (const auto& s : set.surfels)
        if (glm::vec3(s.normal_rV).y > 0.5f)
            ++upCount;
    // all 25 top-face cells face up-ish (9 interior straight up, 16 edge
    // cells diagonal); the freestanding sides face sideways/down
    CHECK(upCount == 25);
}

TEST_CASE("surfelize step: side cells get lateral normals")
{
    const VoxelField f = stepField();
    REQUIRE(f.valid());

    SurfelParams p;
    p.smoothNormals = false;
    p.terrainHeightfieldNormals = false;
    const SurfelSet set = buildSurfels(f, p);

    // the 4-cell cliff face (x=514, y=597..600) exposes -X... the tall
    // patch's east side at x=514 faces the short patch -> +X normals
    int eastCount = 0;
    for (const auto& s : set.surfels)
        if (glm::vec3(s.normal_rV).x > 0.8f)
            ++eastCount;
    CHECK(eastCount > 0);
}

TEST_CASE("surfelize real world: sane count and chunk ranges")
{
    LayeredWorld& lw = allLayersWorld();
    REQUIRE(lw.loaded());
    const VoxelField& field = lw.field();
    REQUIRE(field.valid());

    SurfelParams p;
    p.smoothNormals = true;
    p.terrainHeightfieldNormals = true;
    const SurfelSet set = buildSurfels(field, p);

    CAPTURE(set.surfels.size());
    CAPTURE(set.terrainCount);
    CAPTURE(set.objectCount);
    CAPTURE(set.buildMs);
    CHECK(set.surfels.size() >= 500000u);
    CHECK(set.surfels.size() <= 2500000u);
    CHECK(set.terrainCount > 0u);
    CHECK(set.objectCount > 0u);
    CHECK(set.chunkRange.size() == size_t(GRID_N * GRID_N * GRID_N) + 1u);
    CHECK(set.chunkRange.front() == 0u);
    CHECK(set.chunkRange.back() == uint32_t(set.surfels.size()));
    for (size_t i = 1; i < set.chunkRange.size(); ++i)
        CHECK(set.chunkRange[i] >= set.chunkRange[i - 1]);
}

TEST_CASE("surfelize determinism: two builds identical")
{
    LayeredWorld& lw = allLayersWorld();
    REQUIRE(lw.loaded());
    const VoxelField& field = lw.field();
    REQUIRE(field.valid());

    SurfelParams p;
    p.smoothNormals = true;
    p.terrainHeightfieldNormals = true;
    const SurfelSet a = buildSurfels(field, p);
    const SurfelSet b = buildSurfels(field, p);
    REQUIRE(a.surfels.size() == b.surfels.size());
    // no NaN/Inf may survive: NaN != NaN would break == below and poison GPU
    for (size_t i = 0; i < a.surfels.size(); ++i) {
        const float* fa = &a.surfels[i].pos_rU.x;
        for (int k = 0; k < 16; ++k)
            CHECK(std::isfinite(fa[k]));
        if (!std::isfinite(fa[0]))
            break;
    }
    CHECK(a.surfels == b.surfels);
    CHECK(a.chunkRange == b.chunkRange);
}

TEST_CASE("surfelize chunk bucketing covers every surfel exactly once")
{
    LayeredWorld& lw = allLayersWorld();
    REQUIRE(lw.loaded());
    const VoxelField& field = lw.field();
    REQUIRE(field.valid());

    SurfelParams p;
    p.smoothNormals = false;
    p.terrainHeightfieldNormals = false;
    const SurfelSet set = buildSurfels(field, p);
    REQUIRE(!set.surfels.empty());

    size_t total = 0;
    for (size_t c = 0; c + 1 < set.chunkRange.size(); ++c)
        total += set.chunkRange[c + 1] - set.chunkRange[c];
    CHECK(total == set.surfels.size());
}
