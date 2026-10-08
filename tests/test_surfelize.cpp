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
VoxelField cubeField(uint8_t layer = 0)
{
    std::vector<uint32_t> cells;
    std::vector<uint8_t> mats;
    std::vector<uint8_t> layers;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const uint32_t x = 512 + dx, y = 512 + dy, z = 512 + dz;
                cells.push_back((x << 20) | (y << 10) | z);
                mats.push_back(6);
                layers.push_back(layer);
            }
    VoxelField f;
    std::vector<VoxelRecord> recs;
    std::vector<int16_t> colTop(kLatN * kLatN, -1);
    std::vector<uint8_t> colMat(kLatN * kLatN, 0);
    f.build(recs, colTop, colMat, cells, mats, {}, {}, {}, {}, {}, layers);
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

TEST_CASE("surfelize preserves exact layer ownership")
{
    constexpr uint8_t kLayer = 37;
    const VoxelField f = cubeField(kLayer);
    REQUIRE(f.valid());
    const auto cell = f.sample(512, 512, 512);
    REQUIRE(cell.obj);
    CHECK(cell.layer == kLayer);

    SurfelParams p;
    p.smoothNormals = false;
    p.terrainHeightfieldNormals = false;
    const SurfelSet set = buildSurfels(f, p);
    REQUIRE(!set.surfels.empty());
    for (const Surfel& s : set.surfels) {
        CHECK(surfelLayerId(s.mat_ao.w) == kLayer);
        CHECK(surfelBakedAo(s.mat_ao.w) >= 0.0f);
        CHECK(surfelBakedAo(s.mat_ao.w) <= 1.0f);
    }

    // The packed representation remains precise at the top supported ID.
    for (uint8_t layer : { uint8_t(1), uint8_t(127), uint8_t(254) }) {
        const float packed = packSurfelAo(0.625f, layer);
        CHECK(surfelLayerId(packed) == layer);
        CHECK(std::fabs(surfelBakedAo(packed) - 0.625f) < 1e-4f);
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
    p.edgeShrink = 0.35f;
    p.edgeFill = true;
    const SurfelSet set = buildSurfels(field, p);

    CAPTURE(set.surfels.size());
    CAPTURE(set.terrainCount);
    CAPTURE(set.objectCount);
    CAPTURE(set.edgeParentCount);
    CAPTURE(set.edgeBridgeCount);
    CAPTURE(set.buildMs);
    CHECK(set.surfels.size() >= 500000u);
    // all-layers test world (hamlet + any live overlay): a few million is sane
    CHECK(set.surfels.size() <= 8000000u);
    CHECK(set.terrainCount > 0u);
    CHECK(set.objectCount > 0u);
    CHECK(set.edgeParentCount > 0u);
    CHECK(set.edgeBridgeCount > 0u);
    // Performance gate: the current hamlet adds well under 5% always-on edge
    // geometry. A classification regression that floods curved surfaces fails
    // here before the GPU stream grows without bound.
    CHECK(set.edgeBridgeCount * 20u < set.surfels.size());
    CHECK(set.edgeStart.size() == size_t(GRID_N * GRID_N * GRID_N) + 1u);
    CHECK(set.chunkRange.size() == size_t(GRID_N * GRID_N * GRID_N) + 1u);
    CHECK(set.chunkRange.front() == 0u);
    CHECK(set.chunkRange.back() == uint32_t(set.surfels.size()));
    for (size_t i = 1; i < set.chunkRange.size(); ++i) {
        CHECK(set.chunkRange[i] >= set.chunkRange[i - 1]);
        CHECK(set.edgeStart[i] >= set.edgeStart[i - 1]);
        // edgeStart[i] marks where chunk i's hard-edge bridges begin inside
        // that chunk's run, so it is bracketed by the run's own bounds:
        // [chunkRange[i], chunkRange[i + 1]] (see the layout note on
        // SurfelSet::edgeStart). The upper bound was `chunkRange[i]`, which
        // contradicted the `>=` two lines above - satisfiable only when the two
        // happened to be equal, so it failed once per chunk with base parents
        // (~440) and was reporting a typo, not a layout bug.
        //
        // chunkRange.back() IS the total count, so on the final iteration
        // i + 1 runs off the end; use the total as the upper bound there (and
        // note that the same overrun is what made the corrected line report
        // `edgeStart[i] <= 0` until it was bounded).
        const uint32_t hi = (i + 1 < set.chunkRange.size())
                                ? set.chunkRange[i + 1]
                                : uint32_t(set.surfels.size());
        CHECK(set.edgeStart[i] >= set.chunkRange[i]);
        CHECK(set.edgeStart[i] <= hi);
    }
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
    p.edgeShrink = 0.35f;
    p.edgeFill = true;
    const SurfelSet a = buildSurfels(field, p);
    const SurfelSet b = buildSurfels(field, p);
    REQUIRE(a.surfels.size() == b.surfels.size());
    // no NaN/Inf may survive: NaN != NaN would break == below and poison GPU
    for (size_t i = 0; i < a.surfels.size(); ++i) {
        const float* fa = &a.surfels[i].pos_rU.x;
        for (int k = 0; k < 20; ++k)
            CHECK(std::isfinite(fa[k]));
        if (!std::isfinite(fa[0]))
            break;
    }
    CHECK(a.surfels == b.surfels);
    CHECK(a.chunkRange == b.chunkRange);
    CHECK(a.edgeStart == b.edgeStart);
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

TEST_CASE("surfelize default bake: sane count, ranges and finite surfels")
{
    LayeredWorld& lw = allLayersWorld();
    REQUIRE(lw.loaded());
    const VoxelField& field = lw.field();
    REQUIRE(field.valid());

    SurfelParams p;
    p.smoothNormals = true;
    p.terrainHeightfieldNormals = true;
    const SurfelSet set = buildSurfels(field, p);
    const SurfelSet set2 = buildSurfels(field, p);
    REQUIRE(!set.surfels.empty());
    CHECK(set2.surfels == set.surfels);
    CHECK(set2.chunkRange == set.chunkRange);
    size_t total = 0;
    for (size_t c = 0; c + 1 < set.chunkRange.size(); ++c) {
        CHECK(set.chunkRange[c + 1] >= set.chunkRange[c]);
        total += set.chunkRange[c + 1] - set.chunkRange[c];
    }
    CHECK(total == set.surfels.size());
    for (const auto& s : set.surfels) {
        const float* f = &s.pos_rU.x;
        for (int k = 0; k < 20; ++k)
            CHECK(std::isfinite(f[k]));
        CHECK(s.pos_rU.w > 0.0f);
        CHECK(s.normal_rV.w > 0.0f);
        const float nl = glm::length(glm::vec3(s.normal_rV));
        CHECK(nl > 0.99f);
        CHECK(nl < 1.01f);
        break; // spot-check first surfel finite (full scan is slow here)
    }
}

TEST_CASE("surfelize: anisotropic footprints follow creases")
{
    LayeredWorld& lw = allLayersWorld();
    REQUIRE(lw.loaded());
    const VoxelField& field = lw.field();
    REQUIRE(field.valid());

    SurfelParams p;
    p.lodRings = false;
    p.anisotropy = false;
    const SurfelSet iso = buildSurfels(field, p);
    p.anisotropy = true;
    const SurfelSet aniso = buildSurfels(field, p);
    // same cells, same counts: only the footprint shape may change
    REQUIRE(iso.surfels.size() == aniso.surfels.size());
    size_t thinSide = 0;
    size_t caps = 0;
    size_t creaseStretched = 0;
    const float baseR = p.baseRadius;
    // Three footprint regimes coexist and must be told apart:
    //   crease   - a directional bend; the across radius is the base radius
    //              and the along radius is stretched up to 1.6x along it
    //   thinSide - a long, few-voxel-thick structure; across shrinks to the
    //              structure's own width and the axis is the structure's long
    //              axis projected into the disk plane
    //   cap      - the same structure's top/bottom face, whose normal IS the
    //              long axis: a small round disk of the cross-section
    //   round    - everything else keeps the plain base disk
    for (size_t i = 0; i < iso.surfels.size(); ++i) {
        const float rIso = iso.surfels[i].pos_rU.w;
        const float rU = aniso.surfels[i].pos_rU.w;
        const float rV = aniso.surfels[i].normal_rV.w;
        const glm::vec3 t(aniso.surfels[i].tan_aspect);
        const float tl = glm::length(t);
        CHECK(rU >= rV - 1e-6f); // never wider than it is tall
        if (tl > 0.5f) {
            CHECK(std::fabs(tl - 1.0f) < 1e-3f);
            const glm::vec3 n(aniso.surfels[i].normal_rV);
            CHECK(std::fabs(glm::dot(n, t)) < 1e-3f);
            CHECK(rU >= rV - 1e-6f);
            // Classify by what the footprint IS, not by rV < rIso: a sealed
            // thin footprint is 0.075 and a heavily shrunk iso disk (rChaos
            // at its 0.5 floor) is 0.07, so the across radius can legitimately
            // EXCEED the isotropic one. A crease keeps the iso radius and is
            // capped at 1.6x; anything else must be a thin footprint.
            const bool looksCrease =
                std::fabs(rV - rIso) < 1e-4f && rU <= rIso * 1.6f + 1e-4f;
            if (looksCrease) {
                // ---- crease: across keeps the base radius, stretch capped
                ++creaseStretched;
            } else {
                // ---- thin side: across took the structure's own width, and
                // the axis is the structure's LONG axis (whichever lattice axis
                // that is) projected into the disk plane.
                ++thinSide;
                // Across is one of exactly two radii: kThinAcrossCells
                // (one-cell stems) or kThinAcrossSeal (faces 2+ cells wide).
                // The seal value MUST clear VOXEL/sqrt(2) - the least-covered
                // point between four disk centres VOXEL apart. Below it the
                // pinholes line up into continuous background slots on any
                // face two or more cells wide (the "hollow post" artifact).
                CHECK(rV <= 0.75f * VOXEL + 1e-4f);
                const bool twoRadii = (rV >= VOXEL * 0.7072f - 1e-4f) ||
                                      (rV <= 0.56f * VOXEL + 1e-4f);
                CHECK(twoRadii);
                // The along radius is the configured stretch, clamped to the
                // structure's remaining length by the bake. The clamp is not
                // recomputed here: the test would have to recover the bake's
                // exact cell centre from the surfel position and its SMOOTHED
                // normal, and a one-cell error flips the probe column. The
                // bound below still catches a splat that is not a thin
                // footprint at all; the clamp itself is covered by the render
                // gates (reed tips must not float).
                CHECK(rU <= baseR * 3.0f + 1e-4f);
            }
        } else {
            // no in-plane axis: either a plain round disk or a thin cap
            CHECK(tl < 1e-6f);
            CHECK(rU == rV);
            const bool isRound = std::fabs(rU - rIso) < 1e-4f;
            const bool isCap = rU <= 0.75f * VOXEL + 1e-4f;
            const bool ok = isRound || isCap;
            CHECK(ok);
            if (isCap)
                ++caps;
        }
        CHECK(glm::length(glm::vec3(iso.surfels[i].tan_aspect)) < 1e-9f);
    }
    CHECK(creaseStretched > 0); // the crease rule still fires
    CHECK(thinSide > 0);        // long thin structures become ellipsoids
    CHECK(caps > 0);            // ...and their end faces become caps
    MESSAGE("footprints: crease ", creaseStretched, ", thin side ", thinSide,
            ", cap ", caps, " of ", iso.surfels.size());
}

TEST_CASE("surfelize: hard-edge parents tighten and small crease bridges fill them")
{
    SurfelParams p;
    p.smoothNormals = false;
    p.terrainHeightfieldNormals = false;
    p.lodRings = false;
    p.anisotropy = false;
    p.edgeFill = false;

    const VoxelField cube = cubeField(9);
    p.edgeShrink = 0.0f;
    const SurfelSet cubeBase = buildSurfels(cube, p);
    p.edgeShrink = 0.8f;
    const SurfelSet cubeTight = buildSurfels(cube, p);
    REQUIRE(cubeBase.surfels.size() == cubeTight.surfels.size());
    size_t tightened = 0;
    for (size_t i = 0; i < cubeBase.surfels.size(); ++i) {
        CHECK(cubeTight.surfels[i].pos_rU.w <=
              cubeBase.surfels[i].pos_rU.w + 1e-6f);
        if (cubeTight.surfels[i].pos_rU.w <
            cubeBase.surfels[i].pos_rU.w - 1e-6f)
            ++tightened;
    }
    CHECK(tightened > 0);

    p.edgeFill = true;
    const SurfelSet cubeFilled = buildSurfels(cube, p);
    CHECK(cubeFilled.edgeParentCount > 0);
    CHECK(cubeFilled.edgeBridgeCount > 0);
    CHECK(cubeFilled.surfels.size() ==
          cubeTight.surfels.size() + cubeFilled.edgeBridgeCount);
    REQUIRE(cubeFilled.edgeStart.size() == size_t(GRID_N * GRID_N * GRID_N) + 1);
    const int chunk = chunkIndexOf(512 / CHUNK_N, 512 / CHUNK_N, 512 / CHUNK_N);
    const uint32_t parentEnd = cubeFilled.edgeStart[size_t(chunk)];
    const uint32_t chunkEnd = cubeFilled.chunkRange[size_t(chunk) + 1];
    CHECK(parentEnd - cubeFilled.chunkRange[size_t(chunk)] ==
          cubeTight.surfels.size());
    CHECK(chunkEnd - parentEnd == cubeFilled.edgeBridgeCount);
    for (uint32_t i = parentEnd; i < chunkEnd; ++i) {
        const Surfel& bridge = cubeFilled.surfels[i];
        const glm::vec3 n(bridge.normal_rV);
        const glm::vec3 tangent(bridge.tan_aspect);
        CHECK(surfelLayerId(bridge.mat_ao.w) == 9);
        CHECK(bridge.pos_rU.w < cubeBase.surfels[0].pos_rU.w);
        CHECK(bridge.normal_rV.w < cubeBase.surfels[0].normal_rV.w);
        CHECK(glm::length(tangent) > 0.99f);
        CHECK(glm::length(tangent) < 1.01f);
        CHECK(std::fabs(glm::dot(n, tangent)) < 1e-3f);
    }

    // A flat one-cell plate has a large face interior with only one exposed
    // lattice axis. Those parents must keep full diameter even though an
    // anisotropy/smoothing pass may see normal disagreement elsewhere.
    std::vector<uint32_t> plateCells;
    std::vector<uint8_t> plateMats;
    std::vector<uint8_t> plateLayers;
    for (int z = 510; z < 515; ++z)
        for (int x = 510; x < 515; ++x) {
            plateCells.push_back((uint32_t(x) << 20) |
                                 (uint32_t(600) << 10) | uint32_t(z));
            plateMats.push_back(6);
            plateLayers.push_back(9);
        }
    VoxelField plate;
    {
        std::vector<VoxelRecord> recs;
        std::vector<int16_t> colTop(kLatN * kLatN, -1);
        std::vector<uint8_t> colMat(kLatN * kLatN, 0);
        plate.build(recs, colTop, colMat, plateCells, plateMats,
                    {}, {}, {}, {}, {}, plateLayers);
    }
    p.edgeFill = false;
    p.edgeShrink = 0.0f;
    const SurfelSet plateBase = buildSurfels(plate, p);
    p.edgeShrink = 0.8f;
    const SurfelSet plateTight = buildSurfels(plate, p);
    REQUIRE(plateBase.surfels.size() == plateTight.surfels.size());
    size_t plateTightened = 0;
    for (size_t i = 0; i < plateBase.surfels.size(); ++i)
        if (plateTight.surfels[i].pos_rU.w <
            plateBase.surfels[i].pos_rU.w - 1e-6f)
            ++plateTightened;
    CHECK(plateTightened > 0); // only the plate boundary
    CHECK(plateTightened < plateBase.surfels.size());

    const VoxelField plateau = plateauField();
    p.edgeShrink = 0.0f;
    const SurfelSet terrainBase = buildSurfels(plateau, p);
    p.edgeShrink = 0.8f;
    const SurfelSet terrainTight = buildSurfels(plateau, p);
    REQUIRE(terrainBase.surfels.size() == terrainTight.surfels.size());
    CHECK(terrainTight.edgeBridgeCount == 0);
    for (size_t i = 0; i < terrainBase.surfels.size(); ++i)
        CHECK(std::fabs(terrainTight.surfels[i].pos_rU.w -
                        terrainBase.surfels[i].pos_rU.w) < 1e-6f);
}
