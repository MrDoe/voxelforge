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
