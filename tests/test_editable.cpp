// Tests for EditableWorld::importLayer — runtime placement of foreign .vxw
// layers at the picked anchor. Layer files store absolute lattice coords;
// import must translate the object's bottom-center onto the anchor and append
// only those records to ai_edits.vxw.
#include "voxel/editable_world.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/common.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

using namespace vf::voxel;

namespace {
struct TempDir {
    std::filesystem::path path;
    TempDir()
        : path(std::filesystem::temp_directory_path() /
               ("vf_import_test_" + std::to_string(::getpid())))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::string str() const { return path.string(); }
};

WorldFileMeta testMeta()
{
    return { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
}

// writes a 2x2x2 block of mat `mat` with its min corner at (x,y,z)
bool writeBlockLayer(const std::string& file, int x, int y, int z)
{
    WorldFileData d;
    d.meta = testMeta();
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                VoxelRecord v;
                v.x = uint16_t(x + dx);
                v.y = uint16_t(y + dy);
                v.z = uint16_t(z + dz);
                v.r = 200; v.g = 30; v.b = 30; v.a = 255;
                v.materialId = 6;
                d.voxels.push_back(v);
            }
    return worldfile::write(file, d);
}
} // namespace

TEST_CASE("importLayer stamps a foreign layer at the anchor")
{
    TempDir tmp;
    const std::string src = tmp.path / "thing.vxw";
    // baked far away at absolute coords, as heightmap_gen would
    REQUIRE(writeBlockLayer(src, 900, 40, 120));

    EditableWorld ed(tmp.str());
    CHECK(ed.load());
    // bottom-center of the block is ((900+901)/2, 40, (120+121)/2) = (900,40,120)
    const size_t added = ed.importLayer(src, glm::ivec3(100, 60, 200));
    CHECK(added == 8);

    bool foundAll = true;
    for (const auto& v : ed.records()) {
        glm::ivec3 c(v.x, v.y, v.z);
        foundAll &= c.x >= 100 && c.x <= 101;
        foundAll &= c.y >= 60 && c.y <= 61;
        foundAll &= c.z >= 200 && c.z <= 201;
        foundAll &= (v.materialId == 6 && v.r == 200 && v.g == 30 && v.b == 30);
    }
    CHECK(foundAll);
    CHECK(ed.size() == 8);

    // persisted to ai_edits.vxw in the temp asset dir
    EditableWorld reloaded(tmp.str());
    CHECK(reloaded.load());
    CHECK(reloaded.size() == 8);

    // manifest got the enabled ai_edits entry
    std::vector<worldfile::WorldLayer> layers;
    REQUIRE(worldfile::loadManifest(tmp.path / "world.json", layers));
    bool editsEnabled = false;
    for (const auto& l : layers)
        if (l.file == EditableWorld::kFileName)
            editsEnabled = l.enabled;
    CHECK(editsEnabled);
}

TEST_CASE("importLayer dedupes, rejects bad meta and clips out-of-bounds")
{
    TempDir tmp;
    const std::string src = tmp.path / "thing.vxw";
    REQUIRE(writeBlockLayer(src, 500, 70, 500));

    EditableWorld ed(tmp.str());
    ed.load();
    const glm::ivec3 anchor(300, 20, 300);
    CHECK(ed.importLayer(src, anchor) == 8);
    // second import at the same spot: every cell already claimed
    CHECK(ed.importLayer(src, anchor) == 0);
    CHECK(ed.size() == 8); // nothing duplicated

    WorldFileData bad;
    bad.meta = testMeta();
    bad.meta.gridN = 999; // incompatible lattice
    VoxelRecord v;
    v.x = 1; v.y = 1; v.z = 1;
    bad.voxels.push_back(v);
    const std::string badPath = tmp.path / "bad.vxw";
    REQUIRE(worldfile::write(badPath, bad));
    CHECK(ed.importLayer(badPath, anchor) == 0);
    CHECK(ed.importLayer(tmp.path / "missing.vxw", anchor) == 0);
    CHECK(ed.size() == 8);

    // partial clip: block centred at x=501 moved to anchor x=0 drops x=-1
    const std::string edgeSrc = tmp.path / "edge.vxw";
    WorldFileData e;
    e.meta = testMeta();
    for (int dx = 0; dx <= 2; ++dx) {
        VoxelRecord r;
        r.x = uint16_t(500 + dx);
        r.y = 10;
        r.z = 500;
        e.voxels.push_back(r);
    }
    REQUIRE(worldfile::write(edgeSrc, e));
    CHECK(ed.importLayer(edgeSrc, glm::ivec3(0, 5, 0)) == 2);
}

TEST_CASE("makeVoxelRecord: explicit colour/response override palette")
{
    // mat 6 (wood) palette is (0.62,0.33,0.10) -> (158,84,25), refl 70 rough 160
    VoxelRecord a = makeVoxelRecord(10, 20, 30, 6);
    CHECK(a.materialId == 6);
    CHECK(a.r == 158);
    CHECK(a.g == 84);
    CHECK(a.b == 25);
    CHECK(a.reflectivity == 70);
    CHECK(a.roughness == 160);

    // explicit rgb/refl/rough overrides
    VoxelRecord b = makeVoxelRecord(10, 20, 30, 6, 255, 0, 0, 200, 50);
    CHECK(b.r == 255);
    CHECK(b.g == 0);
    CHECK(b.b == 0);
    CHECK(b.reflectivity == 200);
    CHECK(b.roughness == 50);

    // out-of-range coords clamp into the 1024^3 lattice
    VoxelRecord c = makeVoxelRecord(-5, 5000, 1024, 0);
    CHECK(c.x == 0);
    CHECK(c.y == 1023);
    CHECK(c.z == 1023);
}

TEST_CASE("writeObjectLayer / deleteObjectLayer round-trip")
{
    TempDir tmp;
    EditableWorld ed(tmp.str());

    // seed a manifest with a protected landscape layer so the manifest is never
    // empty after we delete our object layer (loadManifest requires >=1 layer)
    std::vector<worldfile::WorldLayer> seed;
    worldfile::WorldLayer land;
    land.file = "landscape.vxw";
    land.role = "landscape";
    land.name = "landscape";
    land.enabled = true;
    land.listed = true;
    seed.push_back(land);
    REQUIRE(worldfile::writeManifest(tmp.path / "world.json", seed));

    std::vector<VoxelRecord> recs;
    recs.push_back(makeVoxelRecord(512, 256, 512, 8, 255, 0, 0));
    recs.push_back(makeVoxelRecord(513, 256, 512, 6));
    REQUIRE(ed.writeObjectLayer("myrock", recs));

    // file written and manifest entry exists + enabled
    CHECK(std::filesystem::exists(tmp.path / "myrock.vxw"));
    std::vector<worldfile::WorldLayer> layers;
    REQUIRE(worldfile::loadManifest(tmp.path / "world.json", layers));
    bool ok = false, enabled = false;
    for (const auto& l : layers)
        if (l.file == "myrock.vxw") {
            ok = true;
            enabled = l.enabled;
        }
    CHECK(ok);
    CHECK(enabled);

    // round-trip through worldfile::read
    WorldFileData back;
    REQUIRE(worldfile::read(tmp.path / "myrock.vxw", back));
    CHECK(back.voxels.size() == 2);
    CHECK(back.voxels[0].materialId == 8);
    CHECK(back.voxels[0].r == 255);
    CHECK(back.voxels[0].g == 0);
    CHECK(back.voxels[0].b == 0);
    CHECK(back.voxels[1].materialId == 6);
    CHECK(back.voxels[1].reflectivity == 70);

    // overwrite == modify path
    std::vector<VoxelRecord> mod;
    mod.push_back(makeVoxelRecord(600, 300, 600, 4));
    REQUIRE(ed.writeObjectLayer("myrock", mod));
    WorldFileData back2;
    REQUIRE(worldfile::read(tmp.path / "myrock.vxw", back2));
    CHECK(back2.voxels.size() == 1);

    // delete removes file + manifest entry
    REQUIRE(ed.deleteObjectLayer("myrock"));
    CHECK(!std::filesystem::exists(tmp.path / "myrock.vxw"));
    std::vector<worldfile::WorldLayer> layers2;
    REQUIRE(worldfile::loadManifest(tmp.path / "world.json", layers2));
    bool gone = true;
    for (const auto& l : layers2)
        if (l.file == "myrock.vxw")
            gone = false;
    CHECK(gone);

    // name sanitization keeps only [A-Za-z0-9_-], so path separators are
    // stripped (no traversal possible) and "illegal" names become safe local
    // files or empty (rejected)
    CHECK(EditableWorld::sanitizeLayerName("../escape") == "escape");
    CHECK(ed.writeObjectLayer("../escape", recs) == true);
    CHECK(std::filesystem::exists(tmp.path / "escape.vxw"));
    CHECK(EditableWorld::sanitizeLayerName("a/b") == "ab");
    CHECK(EditableWorld::sanitizeLayerName("///") == "");
    CHECK(ed.writeObjectLayer("", recs) == false);
}

TEST_CASE("composed object: primitive building blocks + round-trip AABB")
{
    TempDir tmp;
    EditableWorld ed(tmp.str());
    std::vector<worldfile::WorldLayer> seed;
    worldfile::WorldLayer land;
    land.file = "landscape.vxw";
    land.role = "landscape";
    land.name = "landscape";
    land.enabled = true;
    land.listed = true;
    seed.push_back(land);
    REQUIRE(worldfile::writeManifest(tmp.path / "world.json", seed));

    // mimic the write_object "shapes" path using the same primitives:
    // a body ellipsoid + four leg cylinders anchored at a base point
    const glm::ivec3 base(500, 200, 500);
    std::vector<VoxelRecord> recs;
    auto part = ed.makeEllipsoid(base + glm::ivec3(0, 4, 0),
                                  glm::vec3(0.9f, 0.5f, 0.4f), 6);
    CHECK(!part.empty());
    recs.insert(recs.end(), part.begin(), part.end());
    for (int dx : { -6, 6 })
        for (int dz : { 3, -3 }) {
            auto leg = ed.makeCylinderY(base + glm::ivec3(dx, 0, dz), 0.12f,
                                         0.9f, 6);
            CHECK(!leg.empty());
            recs.insert(recs.end(), leg.begin(), leg.end());
        }

    REQUIRE(ed.writeObjectLayer("critter", recs));
    WorldFileData back;
    REQUIRE(worldfile::read(tmp.path / "critter.vxw", back));
    CHECK(back.voxels.size() == recs.size());

    // CPU-side proportion check (what read_object's AABB gives the agent)
    int mn[3] = { 1 << 30, 1 << 30, 1 << 30 };
    int mx[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
    for (const auto& v : back.voxels) {
        int c[3] = { v.x, v.y, v.z };
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], c[a]);
            mx[a] = std::max(mx[a], c[a]);
        }
    }
    CHECK(mx[0] - mn[0] > 10); // body+legs span > 1 m along x
    CHECK(mx[1] - mn[1] > 10); // legs (y=200..) to body top span > 1 m
}

TEST_CASE("carve cylinder opens the whole disk it starts at")
{
    TempDir tmp;
    EditableWorld ed(tmp.str());
    REQUIRE(ed.load());

    // flat ground beside the river: the pick lands on the surface cell and the
    // axis is exactly opposite to up - the degenerate branch of
    // glm::rotation(up, axisDir). An exact surface test dropped half of the
    // base-plane layer there, and a flat cap also left a one-cell roof over the
    // +-1 cell of terrain relief around the pick: the scoop never flooded.
    const glm::ivec3 anchor(512, 508, 512);
    const float radius = 3.0f, depth = 3.0f;
    const std::vector<VoxelRecord> recs = ed.makeOrientedCylinder(
        anchor, glm::vec3(0.f, -1.f, 0.f), radius, depth, 2, /*carve=*/true);
    REQUIRE(!recs.empty());

    auto has = [&](int x, int y, int z) {
        for (const VoxelRecord& v : recs)
            if (int(v.x) == x && int(v.y) == y && int(v.z) == z)
                return true;
        return false;
    };

    // the scoop reaches the surface band above the pick (2 cells) and no more
    CHECK(has(anchor.x, anchor.y + 2, anchor.z));
    CHECK(!has(anchor.x, anchor.y + 3, anchor.z));
    // floor exactly at the picked depth (3 m = 30 cells)
    CHECK(has(anchor.x, anchor.y - 30, anchor.z));
    CHECK(!has(anchor.x, anchor.y - 31, anchor.z));
    // side boundary cells are kept (inclusive surface), one further out is not
    CHECK(has(anchor.x + 30, anchor.y, anchor.z));
    CHECK(!has(anchor.x + 31, anchor.y, anchor.z));

    // the top layer covers every column of the disk - not just the half where
    // the degenerate rotation happened to keep local.y <= 0
    int expected = 0, found = 0;
    for (int dz = -31; dz <= 31; ++dz)
        for (int dx = -31; dx <= 31; ++dx) {
            if (VOXEL * std::hypot(float(dx), float(dz)) > radius + 1e-4f)
                continue;
            ++expected;
            if (has(anchor.x + dx, anchor.y + 2, anchor.z + dz))
                ++found;
        }
    CHECK(expected > 2700);
    CHECK(found == expected);

    // the add shell keeps the exact [0, length] extent: no interior volume and
    // nothing above the base plane beyond the shell band
    const std::vector<VoxelRecord> shell = ed.makeOrientedCylinder(
        anchor, glm::vec3(0.f, -1.f, 0.f), radius, depth, 2, /*carve=*/false);
    auto inShell = [&](int x, int y, int z) {
        for (const VoxelRecord& v : shell)
            if (int(v.x) == x && int(v.y) == y && int(v.z) == z)
                return true;
        return false;
    };
    CHECK(!inShell(anchor.x, anchor.y - 15, anchor.z)); // interior stays empty
    CHECK(inShell(anchor.x, anchor.y + 2, anchor.z));   // shell cap band
}

TEST_CASE("per-voxel add and carve touch exactly one cell")
{
    TempDir tmp;
    EditableWorld ed(tmp.str());
    REQUIRE(ed.load());

    const glm::ivec3 anchor(512, 300, 512);

    // Carve removes the voxel under the cursor, untouched.
    const std::vector<VoxelRecord> carve =
        ed.makeSingleVoxel(anchor, glm::vec3(0.f, 1.f, 0.f), 6, /*step=*/false);
    REQUIRE(carve.size() == 1);
    CHECK(int(carve[0].x) == anchor.x);
    CHECK(int(carve[0].y) == anchor.y);
    CHECK(int(carve[0].z) == anchor.z);
    CHECK(carve[0].materialId == 6);

    // Add steps one cell OUT along the dominant axis of the normal: the pick
    // lands on solid material, so adding the picked cell would do nothing.
    // A floor pick (+Y) stacks on top, a wall pick (-Z) hangs off the face.
    for (const auto& [n, want] : std::vector<std::pair<glm::vec3, glm::ivec3>>{
             { glm::vec3(0.f, 1.f, 0.f), glm::ivec3(anchor.x, anchor.y + 1, anchor.z) },
             { glm::vec3(0.f, -1.f, 0.f), glm::ivec3(anchor.x, anchor.y - 1, anchor.z) },
             { glm::vec3(0.f, 0.f, -1.f), glm::ivec3(anchor.x, anchor.y, anchor.z - 1) },
             { glm::vec3(1.f, 0.f, 0.f), glm::ivec3(anchor.x + 1, anchor.y, anchor.z) } }) {
        const std::vector<VoxelRecord> add =
            ed.makeSingleVoxel(anchor, n, 3, /*step=*/true);
        REQUIRE(add.size() == 1);
        CHECK(glm::ivec3(add[0].x, add[0].y, add[0].z) == want);
        CHECK(add[0].materialId == 3);
    }

    // a smoothed corner normal picks ONE axis, not round(n / VOXEL) (which
    // would step 7 cells on two axes and land nowhere near the surface)
    const std::vector<VoxelRecord> corner =
        ed.makeSingleVoxel(anchor, glm::vec3(0.7f, 0.7f, 0.f), 6, true);
    REQUIRE(corner.size() == 1);
    const glm::ivec3 ct(corner[0].x, corner[0].y, corner[0].z);
    CHECK((ct == glm::ivec3(anchor.x, anchor.y + 1, anchor.z) ||
           ct == glm::ivec3(anchor.x + 1, anchor.y, anchor.z)));
    CHECK(abs(ct.x - anchor.x) <= 1);
    CHECK(abs(ct.y - anchor.y) <= 1);
    CHECK(ct.z == anchor.z);

    // a degenerate normal still steps somewhere sane
    const std::vector<VoxelRecord> zero =
        ed.makeSingleVoxel(anchor, glm::vec3(0.f), 6, true);
    REQUIRE(zero.size() == 1);
    CHECK(glm::ivec3(zero[0].x, zero[0].y, zero[0].z).y == anchor.y + 1);

    // stepping out of the lattice yields nothing instead of a wrapped cell
    CHECK(ed.makeSingleVoxel(glm::ivec3(0, 300, 512), glm::vec3(-1.f, 0.f, 0.f),
                             6, true).empty());
    CHECK(ed.makeSingleVoxel(glm::ivec3(1023, 300, 512), glm::vec3(1.f, 0.f, 0.f),
                             6, true).empty());
    CHECK(ed.makeSingleVoxel(glm::ivec3(0, 0, 0), glm::vec3(0.f, -1.f, 0.f), 6,
                             true).empty());
    // ...but the unstepped pick at the origin is still emitted
    CHECK(ed.makeSingleVoxel(glm::ivec3(0, 0, 0), glm::vec3(0.f, -1.f, 0.f), 6,
                             false).size() == 1);
}

TEST_CASE("add dome grows out along the picked surface normal")
{
    TempDir tmp;
    EditableWorld ed(tmp.str());
    REQUIRE(ed.load());

    auto index = [](const std::vector<VoxelRecord>& v) {
        std::vector<uint64_t> keys;
        keys.reserve(v.size());
        for (const VoxelRecord& r : v)
            keys.push_back((uint64_t(r.x) << 42) | (uint64_t(r.y) << 21) |
                           uint64_t(r.z));
        return keys;
    };
    // A vertical wall: the axis is +X, i.e. nothing to do with world up. The
    // old rasterizer spanned the dome height in world Y, which clipped the
    // footprint (the plane ACROSS the axis) to y >= anchor.y: on this cell set
    // the old code emitted 0 of 4612 cells below the pick, so the brush never
    // thickened the wall - it raised a bulge above the click.
    const glm::ivec3 anchor(512, 300, 512);
    const float radius = 1.0f, height = 1.5f;
    const std::vector<VoxelRecord> wall =
        ed.makeDome(anchor, glm::vec3(1.f, 0.f, 0.f), radius, height, 6);
    REQUIRE(!wall.empty());
    const std::vector<uint64_t> keys = index(wall);
    auto has = [&](int x, int y, int z) {
        const uint64_t k = (uint64_t(uint32_t(x)) << 42) |
                           (uint64_t(uint32_t(y)) << 21) | uint64_t(uint32_t(z));
        return std::find(keys.begin(), keys.end(), k) != keys.end();
    };

    // grows the full depth out of the surface along the normal (1.5 m = 15
    // cells) and no further
    CHECK(has(anchor.x + 15, anchor.y, anchor.z));
    CHECK(!has(anchor.x + 16, anchor.y, anchor.z));
    // the footprint is a full disk in the plane ACROSS the normal: cells a
    // full radius above and below the hit are covered (world up/down used to
    // be the only extent the loop spanned, so a wall got nothing)
    CHECK(has(anchor.x + 5, anchor.y - 10, anchor.z));
    CHECK(has(anchor.x + 5, anchor.y + 10, anchor.z));
    CHECK(!has(anchor.x + 5, anchor.y - 11, anchor.z));
    // the wall thickens over its WHOLE footprint and only the outer lip rounds
    // over: at the footprint edge (1.0 m off axis) the growth is a full
    // heightM - c = 1.0 m straight, then the fillet c = 0.5 m closes in
    CHECK(has(anchor.x + 10, anchor.y + 10, anchor.z));
    CHECK(!has(anchor.x + 11, anchor.y + 10, anchor.z));
    // nothing is emitted behind the surface (the picked wall's far side is
    // never eroded), beyond the one-voxel base layer
    CHECK(has(anchor.x - 1, anchor.y, anchor.z));
    CHECK(!has(anchor.x - 5, anchor.y, anchor.z));
    // the growth follows the axis: +Z is off the wall, -Z too
    CHECK(!has(anchor.x + 5, anchor.y, anchor.z + 12));
    CHECK(!has(anchor.x + 5, anchor.y, anchor.z - 12));

    // terrain (axis = +Y) still raises a rounded plateau of the same size
    const std::vector<VoxelRecord> floor =
        ed.makeDome(anchor, glm::vec3(0.f, 1.f, 0.f), radius, height, 6);
    const std::vector<uint64_t> fkeys = index(floor);
    auto fhas = [&](int x, int y, int z) {
        const uint64_t k = (uint64_t(uint32_t(x)) << 42) |
                           (uint64_t(uint32_t(y)) << 21) | uint64_t(uint32_t(z));
        return std::find(fkeys.begin(), fkeys.end(), k) != fkeys.end();
    };
    REQUIRE(!floor.empty());
    CHECK(fhas(anchor.x, anchor.y + 15, anchor.z));
    CHECK(!fhas(anchor.x, anchor.y + 16, anchor.z));
    CHECK(fhas(anchor.x + 10, anchor.y + 5, anchor.z));
    CHECK(fhas(anchor.x - 10, anchor.y + 5, anchor.z));
    CHECK(fhas(anchor.x + 10, anchor.y + 10, anchor.z));   // straight side
    CHECK(!fhas(anchor.x + 10, anchor.y + 12, anchor.z));  // past the fillet
    CHECK(!fhas(anchor.x, anchor.y - 5, anchor.z));

    // a shallow growth (height < radius) is a wide filleted slab, not a narrow
    // dome: the whole 4 m footprint is covered and the fillet (c = 0.15 m)
    // closes the last 0.15 m
    const std::vector<VoxelRecord> low =
        ed.makeDome(anchor, glm::vec3(0.f, 0.f, -1.f), 2.0f, 0.3f, 6);
    const std::vector<uint64_t> lkeys = index(low);
    auto lhas = [&](int x, int y, int z) {
        const uint64_t k = (uint64_t(uint32_t(x)) << 42) |
                           (uint64_t(uint32_t(y)) << 21) | uint64_t(uint32_t(z));
        return std::find(lkeys.begin(), lkeys.end(), k) != lkeys.end();
    };
    REQUIRE(!low.empty());
    CHECK(lhas(anchor.x, anchor.y, anchor.z - 3));   // 0.3 m = 3 cells: the top
    CHECK(!lhas(anchor.x, anchor.y, anchor.z - 4));
    CHECK(lhas(anchor.x + 20, anchor.y, anchor.z));  // footprint edge: 2.0 m
    CHECK(!lhas(anchor.x + 21, anchor.y, anchor.z));
    CHECK(lhas(anchor.x, anchor.y + 20, anchor.z));
    CHECK(!lhas(anchor.x, anchor.y + 21, anchor.z));
    // the footprint edge is straight for the first 0.15 m, then the fillet
    CHECK(lhas(anchor.x + 20, anchor.y, anchor.z - 1));
    CHECK(!lhas(anchor.x + 20, anchor.y, anchor.z - 2));
    // one voxel behind the surface is part of the volume by design (it seals
    // the growth against the surface, like the carve scoop's base layer)
    CHECK(lhas(anchor.x, anchor.y, anchor.z + 1));
    CHECK(!lhas(anchor.x, anchor.y, anchor.z + 2));
    // the world bounds never clip the volume: a 2 m radius at the top of the
    // lattice still yields the full footprint
    const std::vector<VoxelRecord> high =
        ed.makeDome(glm::ivec3(512, 1023, 512), glm::vec3(0.f, 1.f, 0.f),
                    2.0f, 0.3f, 6);
    const std::vector<uint64_t> hkeys = index(high);
    auto hhas = [&](int x, int y, int z) {
        const uint64_t k = (uint64_t(uint32_t(x)) << 42) |
                           (uint64_t(uint32_t(y)) << 21) | uint64_t(uint32_t(z));
        return std::find(hkeys.begin(), hkeys.end(), k) != hkeys.end();
    };
    CHECK(hhas(512 + 20, 1023, 512));
}
