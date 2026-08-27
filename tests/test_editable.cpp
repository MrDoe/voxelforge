// Tests for EditableWorld::importLayer — runtime placement of foreign .vxw
// layers at the picked anchor. Layer files store absolute lattice coords;
// import must translate the object's bottom-center onto the anchor and append
// only those records to ai_edits.vxw.
#include "voxel/editable_world.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/common.hpp"
#include <doctest/doctest.h>
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
