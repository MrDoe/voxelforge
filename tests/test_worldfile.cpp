#include "voxel/world.hpp"
#include "voxel/worldfile.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <cstdio>

using namespace vf::voxel;

namespace {
std::string tmpPath()
{
    return (std::filesystem::temp_directory_path() / "vf_test_world.vxw").string();
}

WorldFileData sampleData()
{
    WorldFileData d;
    d.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    d.chunkGrid = { -1, 5, 9, -1 };
    d.childBase = { 0u, 8u };
    d.payload = { 0xFFu, 0x1Fu };
    d.handles = { 0xFFFFFFFFu, 4u, kSolidHandle, 6u, 8u, 10u, 12u, 14u,
                  16u, kEmptyHandle, 20u, 22u, 24u, 26u, 28u, 30u };
    // two voxels worth of brick words: rgb+sdf | a+refl+rough+mat
    d.bricks = { 0x40C02880u, 0x03E628FFu, 0x60C00020u, 0x018732FFu };
    for (int i = 0; i < 3; ++i) {
        VoxelRecord v;
        v.x = uint16_t(10 + i);
        v.y = uint16_t(500);
        v.z = uint16_t(1000 - i);
        v.r = uint8_t(30 + i);
        v.g = uint8_t(120);
        v.b = uint8_t(60);
        v.a = 255;
        v.reflectivity = uint8_t(35 * (i + 1));
        v.roughness = uint8_t(200);
        v.materialId = uint8_t(i);
        d.voxels.push_back(v);
    }
    return d;
}
} // namespace

TEST_CASE("worldfile roundtrip preserves all data")
{
    WorldFileData src = sampleData();
    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));

    WorldFileData out;
    REQUIRE(worldfile::read(path, out));
    CHECK(out.meta.worldSize == src.meta.worldSize);
    CHECK(out.meta.voxelSize == src.meta.voxelSize);
    CHECK(out.meta.waterLevel == src.meta.waterLevel);
    CHECK(out.meta.gridN == src.meta.gridN);
    CHECK(out.chunkGrid == src.chunkGrid);
    CHECK(out.childBase == src.childBase);
    CHECK(out.payload == src.payload);
    CHECK(out.handles == src.handles);
    CHECK(out.bricks == src.bricks);
    REQUIRE(out.voxels.size() == src.voxels.size());
    for (size_t i = 0; i < src.voxels.size(); ++i) {
        const VoxelRecord& a = src.voxels[i];
        const VoxelRecord& b = out.voxels[i];
        CHECK(b.x == a.x);
        CHECK(b.y == a.y);
        CHECK(b.z == a.z);
        CHECK(b.r == a.r);
        CHECK(b.reflectivity == a.reflectivity);
        CHECK(b.roughness == a.roughness);
        CHECK(b.materialId == a.materialId);
    }
    glm::vec3 p = out.voxels[0].position(out.meta);
    CHECK(p.x > -0.5f * WORLD);
    CHECK(p.x < 0.0f);
    std::remove(path.c_str());
}

TEST_CASE("worldfile rejects corrupted payloads")
{
    WorldFileData src = sampleData();
    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));

    FILE* f = fopen(path.c_str(), "r+b");
    REQUIRE(f != nullptr);
    fseek(f, -10, SEEK_END); // flip a byte inside the record region
    int c = fgetc(f);
    fseek(f, -1, SEEK_CUR);
    fputc(c ^ 0xFF, f);
    fclose(f);

    WorldFileData out;
    CHECK_FALSE(worldfile::read(path, out));
    std::remove(path.c_str());
}

TEST_CASE("worldfile rejects wrong magic")
{
    WorldFileData src = sampleData();
    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));
    FILE* f = fopen(path.c_str(), "r+b");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_SET);
    fputc('X', f);
    fclose(f);
    WorldFileData out;
    CHECK_FALSE(worldfile::read(path, out));
    std::remove(path.c_str());
}

TEST_CASE("manifest roundtrip and layered dedupe")
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "vf_layers_test";
    std::filesystem::create_directories(dir);

    // two record-only layers sharing one cell: first layer must win
    auto mk = [&](uint16_t x, uint8_t mat) {
        VoxelRecord v;
        v.x = x;
        v.y = 512;
        v.z = 512;
        v.materialId = mat;
        return v;
    };
    worldfile::WorldLayer a{ "a.vxw", "object", "a", { 1.f, 2.f, 3.f }, 45.f };
    worldfile::WorldLayer b{ "b.vxw", "object", "b", {}, 0.f };
    b.enabled = false; // disabled layers stay on disk but leave the merge
    CHECK(worldfile::writeManifest((dir / "world.json").string(), { a, b }));

    std::vector<worldfile::WorldLayer> loaded;
    REQUIRE(worldfile::loadManifest((dir / "world.json").string(), loaded));
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].file == "a.vxw");
    CHECK(loaded[0].role == "object");
    CHECK(loaded[0].name == "a");
    CHECK(loaded[0].pos[0] == doctest::Approx(1.f));
    CHECK(loaded[0].rotDeg == doctest::Approx(45.f));
    CHECK(loaded[0].enabled == true);
    CHECK(loaded[1].enabled == false); // bool literal parsed back

    WorldFileData da, db;
    da.meta = db.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    da.voxels = { mk(100, 6), mk(101, 6) };
    db.voxels = { mk(101, 2), mk(102, 2) }; // 101 overlaps -> b loses there
    REQUIRE(worldfile::write((dir / "a.vxw").string(), da));
    REQUIRE(worldfile::write((dir / "b.vxw").string(), db));

    std::vector<VoxelRecord> merged;
    WorldFileMeta expected{ WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    REQUIRE(worldfile::readLayered((dir / "world.json").string(), expected, merged));
    REQUIRE(merged.size() == 2); // only a - b is disabled
    CHECK(merged[0].materialId == 6);
    CHECK(merged[1].materialId == 6);

    // re-enable b: overlap cell must go to a (earlier layer wins)
    loaded[1].enabled = true;
    CHECK(worldfile::writeManifest((dir / "world.json").string(), loaded));
    REQUIRE(worldfile::readLayered((dir / "world.json").string(), expected, merged));
    REQUIRE(merged.size() == 3);
    CHECK(merged[1].materialId == 6);
    CHECK(merged[2].materialId == 2);

    // meta mismatch must be rejected
    expected.voxelSize = 0.5f;
    CHECK_FALSE(worldfile::readLayered((dir / "world.json").string(), expected, merged));

    std::filesystem::remove_all(dir);
}

TEST_CASE("record-only layer files are valid VXW (empty SVO sections)")
{
    std::string path = tmpPath();
    WorldFileData d;
    d.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    VoxelRecord only;
    only.x = 7;
    only.materialId = 4;
    d.voxels.push_back(only);
    REQUIRE(worldfile::write(path, d));
    WorldFileData out;
    REQUIRE(worldfile::read(path, out));
    CHECK(out.chunkGrid.empty());
    CHECK(out.bricks.empty());
    REQUIRE(out.voxels.size() == 1);
    CHECK(out.voxels[0].x == 7);
    std::remove(path.c_str());
}
