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
